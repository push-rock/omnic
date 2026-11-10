/* archive：.omnic v1 实现（ARCHIVE_SPEC §3/§4）。
 * pack 单遍写出：Header → Data（块流式落盘，块/条目双层 CRC 边写边算）→
 * Index(zstd) → 回填 Header n_blocks → Footer。
 * unpack：Header/Footer 双端校验 → 索引 CRC + 结构校验 → 逐块解压切条目，
 * 坏块跳过隔离（AR-ERR-001）。CRC-32 自带（懒初始化表），不绑可选 codec。
 * A3 范围：store 块 only；压缩块与决策分桶在 A4 接入（fmt!=0 当前为桩）。 */
/* _DEFAULT_SOURCE 须先于任何系统头（同 fs.c）：严格 C11 下 pthread 声明需显式开启 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "core/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/util.h"
#include "core/blockio.h"
#include "core/dispatcher.h"
#include "core/registry.h"
#include "core/arc_sync.h"

#ifndef OMNIC_CODEC_ZSTD
/* 格式固有依赖 zstd（INDEX_ZSTD 恒置位）；无 zstd 构建 stub 保 lib ABI */
omnic_status omnic_pack(const char *root, const char *archive_path,
                        const omnic_options *o, omnic_report *rep) {
    (void)root;
    (void)archive_path;
    (void)o;
    (void)rep;
    omnic_set_error("archive requires zstd build");
    return OMNIC_E_USAGE;
}
omnic_status omnic_unpack(const char *archive_path, const char *dest_dir,
                          const omnic_options *o, omnic_report *rep) {
    (void)archive_path;
    (void)dest_dir;
    (void)o;
    (void)rep;
    omnic_set_error("archive requires zstd build");
    return OMNIC_E_USAGE;
}
#else /* ---- 完整实现 ---- */

/* ---- CRC-32/ISO-HDLC ---- */
static uint32_t s_crc_tab[256];
static int s_crc_ready = 0;

static uint32_t arc_crc32(uint32_t crc, const void *buf, size_t n) {
    if (!s_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            s_crc_tab[i] = c;
        }
        s_crc_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = s_crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- little-endian ---- */
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ---- 64 位偏移 seek（u64 offset 纪律）---- */
#ifdef _WIN32
static int arc_seek(FILE *f, uint64_t off) {
    return _fseeki64(f, (__int64)off, SEEK_SET);
}
#else
#include <sys/types.h>
static int arc_seek(FILE *f, uint64_t off) {
    return fseeko(f, (off_t)off, SEEK_SET);
}
#endif

/* ---- 动态缓冲 ---- */
typedef struct {
    uint8_t *buf;
    size_t n, cap;
} bytebuf;

static int bb_put(bytebuf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *buf = (uint8_t *)realloc(b->buf, cap);
        if (buf == NULL) return -1;
        b->buf = buf;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, p, n);
    b->n += n;
    return 0;
}

/* ---- 布局 ---- */
#define ARC_HDR 32u
#define ARC_FTR 32u
#define ARC_ENT_FIX 26u  /* type/pad/block_id/in_off/size/crc */
#define ARC_BLK_ROW 32u
static const uint8_t ARC_MAGIC_H[8] = {'O', 'M', 'N', 'I', 'C', 'A', 'R', 'C'};
static const uint8_t ARC_MAGIC_F[8] = {'O', 'M', 'N', 'I', 'C', 'E', 'N', 'D'};

#ifndef OMNIC_CODEC_ZSTD
#error "internal: stub must precede helpers"
#endif
#include <zstd.h>

/* ---- 内存表 ---- */
typedef struct {
    uint8_t fmt, level;
    uint64_t offset;    /* 相对 data_off */
    uint64_t comp_size; /* store 块 = raw_size */
    uint64_t raw_size;
    uint32_t crc;       /* 落盘字节 CRC */
} arc_block;

typedef struct {
    char *name;
    uint8_t type; /* 0=file 1=dir */
    uint32_t block_id;
    uint64_t in_off, size;
    uint32_t crc;
    long long mtime;
} arc_entry;

typedef struct {
    arc_block *blk;
    size_t nblk;
    arc_entry *ent;
    size_t nent;
} arc_index;

static void arc_index_mem_free(arc_index *ix) {
    if (ix == NULL) return;
    for (size_t i = 0; i < ix->nent; i++) free(ix->ent[i].name);
    free(ix->ent);
    free(ix->blk);
    memset(ix, 0, sizeof(*ix));
}

/* ---- 条目名校验（ARCHIVE_SPEC §3.7）---- */
static int arc_name_valid(const char *s) {
    if (s == NULL || s[0] == '\0') return 0;
    if (s[0] == '/' || strchr(s, '\\') != NULL) return 0;
    for (const char *p = s; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20u) return 0;
        if (p[0] == '/' && (p[1] == '/' || p[1] == '\0')) return 0;
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0'))
            return 0;
    }
    if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == '\0')) return 0;
    return strlen(s) < 65535u;
}

static char *arc_dup_name(const char *s) {
    if (!arc_name_valid(s)) return NULL;
    char *p = (char *)malloc(strlen(s) + 1);
    if (p != NULL) strcpy(p, s);
    return p;
}

static int cmp_strp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ---- 索引明文序列化：块表(32B/块) ‖ 条目表(name_len/name/26B[+mtime8]) ---- */
static int arc_index_serialize(const arc_block *blk, size_t nblk, const arc_entry *ent,
                               size_t nent, int has_mtime, bytebuf *out) {
    for (size_t i = 0; i < nblk; i++) {
        uint8_t row[ARC_BLK_ROW];
        memset(row, 0, sizeof(row));
        row[0] = blk[i].fmt;
        row[1] = blk[i].level;
        put64(row + 4, blk[i].offset);
        put64(row + 12, blk[i].comp_size);
        put64(row + 20, blk[i].raw_size);
        put32(row + 28, blk[i].crc);
        if (bb_put(out, row, sizeof(row)) != 0) return -1;
    }
    for (size_t i = 0; i < nent; i++) {
        uint16_t nl = (uint16_t)strlen(ent[i].name);
        if (bb_put(out, &nl, 2) != 0) return -1;
        if (bb_put(out, ent[i].name, nl) != 0) return -1;
        uint8_t row[ARC_ENT_FIX + 8];
        memset(row, 0, sizeof(row));
        row[0] = ent[i].type;
        put32(row + 2, ent[i].block_id);
        put64(row + 6, ent[i].in_off);
        put64(row + 14, ent[i].size);
        put32(row + 22, ent[i].crc);
        if (has_mtime) put64(row + 26, (uint64_t)ent[i].mtime);
        if (bb_put(out, row, has_mtime ? ARC_ENT_FIX + 8 : ARC_ENT_FIX) != 0) return -1;
    }
    return 0;
}

/* ---- unpack：索引解析（全部结构校验在此，ARCHIVE_SPEC §4.1）---- */
static omnic_status arc_index_parse(const uint8_t *p, size_t n, uint32_t nent,
                                    uint32_t nblk, int has_mtime, arc_index *ix) {
    memset(ix, 0, sizeof(*ix));
    if (nblk > 0) {
        if (n < (size_t)nblk * ARC_BLK_ROW) return OMNIC_E_DATA;
        ix->blk = (arc_block *)calloc(nblk, sizeof(arc_block));
        if (ix->blk == NULL) return OMNIC_E_SYSTEM;
        ix->nblk = nblk;
        for (uint32_t i = 0; i < nblk; i++) {
            const uint8_t *r = p + (size_t)i * ARC_BLK_ROW;
            ix->blk[i].fmt = r[0];
            ix->blk[i].level = r[1];
            ix->blk[i].offset = get64(r + 4);
            ix->blk[i].comp_size = get64(r + 12);
            ix->blk[i].raw_size = get64(r + 20);
            ix->blk[i].crc = get32(r + 28);
            if (ix->blk[i].comp_size == 0 && ix->blk[i].raw_size == 0) {
                /* 空块（仅含 0 字节文件的 store 块）合法，块区零字节 */
            } else if (ix->blk[i].comp_size == 0 || ix->blk[i].raw_size == 0 ||
                       ix->blk[i].raw_size > (1u << 30)) { /* D14 炸弹预检 */
                return OMNIC_E_DATA;
            }
        }
        p += (size_t)nblk * ARC_BLK_ROW;
        n -= (size_t)nblk * ARC_BLK_ROW;
    }
    if (nent > 0) {
        ix->ent = (arc_entry *)calloc(nent, sizeof(arc_entry));
        if (ix->ent == NULL) return OMNIC_E_SYSTEM;
        ix->nent = nent;
    }
    size_t rowlen = ARC_ENT_FIX + (has_mtime ? 8u : 0u);
    for (uint32_t i = 0; i < nent; i++) {
        if (n < 2) return OMNIC_E_DATA;
        uint16_t nl = get16(p);
        p += 2;
        n -= 2;
        if (nl == 0 || n < (size_t)nl + rowlen) return OMNIC_E_DATA;
        char *name = (char *)malloc((size_t)nl + 1);
        if (name == NULL) return OMNIC_E_SYSTEM;
        memcpy(name, p, nl);
        name[nl] = '\0';
        p += nl;
        n -= nl;
        if (!arc_name_valid(name)) {
            omnic_set_error("entry name escapes archive root: %s — refusing (corrupt archive)",
                            name);
            free(name);
            return OMNIC_E_DATA;
        }
        ix->ent[i].name = name;
        ix->ent[i].type = p[0];
        ix->ent[i].block_id = get32(p + 2);
        ix->ent[i].in_off = get64(p + 6);
        ix->ent[i].size = get64(p + 14);
        ix->ent[i].crc = get32(p + 22);
        if (has_mtime) ix->ent[i].mtime = (long long)get64(p + 26);
        p += rowlen;
        n -= rowlen;
        if (ix->ent[i].type == 0) {
            if (ix->ent[i].block_id >= ix->nblk) return OMNIC_E_DATA;
            if (ix->ent[i].in_off > (1u << 30) || ix->ent[i].size > (1u << 30)) return OMNIC_E_DATA;
            const arc_block *b = &ix->blk[ix->ent[i].block_id];
            if (ix->ent[i].in_off + ix->ent[i].size > b->raw_size) return OMNIC_E_DATA;
        } else if (ix->ent[i].type != 1) {
            return OMNIC_E_DATA;
        }
    }
    /* 条目名唯一性（§4.1）：指针排序后查相邻，O(n log n) */
    if (nent > 1) {
        const char **names = (const char **)malloc((size_t)nent * sizeof(char *));
        if (names == NULL) return OMNIC_E_SYSTEM;
        for (uint32_t i = 0; i < nent; i++) names[i] = ix->ent[i].name;
        qsort(names, nent, sizeof(char *), cmp_strp);
        for (uint32_t i = 1; i < nent; i++) {
            if (strcmp(names[i - 1], names[i]) == 0) {
                omnic_set_error("duplicate entry name in archive: %s", names[i]);
                free(names);
                return OMNIC_E_DATA;
            }
        }
        free(names);
    }
    return OMNIC_OK;
}

/* ---- pack 决策与分桶（A4，AR-PKG-001..004）----
 * 桶 = 同 (codec,level) 建议的文件聚合（D10：字典共享域 ≈ codec 相同性）；
 * skip 类进 store 桶（--no-skip 时视同 unknown 走 zstd/19，实验口径）。 */
typedef struct {
    char codec[16];
    int level;
    int is_store;
    size_t *idx; /* pack 文件序（指向 fs/ent 数组） */
    size_t n, cap;
    unsigned long long total;
} arc_bucket;

static int bucket_push(arc_bucket *b, size_t file_i, unsigned long long sz) {
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        size_t *p = (size_t *)realloc(b->idx, cap * sizeof(size_t));
        if (p == NULL) return -1;
        b->idx = p;
        b->cap = cap;
    }
    b->idx[b->n++] = file_i;
    b->total += sz;
    return 0;
}

static void buckets_free(arc_bucket *b, size_t nb) {
    for (size_t i = 0; i < nb; i++) free(b[i].idx);
    free(b);
}

/* 桶序确定性：大小降序，平局按 (codec,level) 字典序 */
static int bucket_cmp(const void *pa, const void *pb) {
    const arc_bucket *a = (const arc_bucket *)pa, *b = (const arc_bucket *)pb;
    if (a->total != b->total) return a->total > b->total ? -1 : 1;
    int c = strcmp(a->codec, b->codec);
    if (c != 0) return c;
    return a->level - b->level;
}

/* 决策 pass 的逐文件计划 */
typedef struct {
    char codec[16];
    int level;
    int store;
} file_plan;

/* 块表扩容入表（offset 相对 data_off） */
static int arc_block_new(arc_index *ix, size_t *cap, uint8_t fmt, uint8_t level,
                         uint64_t offset) {
    if (ix->nblk == *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        arc_block *b = (arc_block *)realloc(ix->blk, c * sizeof(arc_block));
        if (b == NULL) return -1;
        ix->blk = b;
        *cap = c;
    }
    memset(&ix->blk[ix->nblk], 0, sizeof(arc_block));
    ix->blk[ix->nblk].fmt = fmt;
    ix->blk[ix->nblk].level = level;
    ix->blk[ix->nblk].offset = offset;
    ix->nblk++;
    return 0;
}

/* 内存块压缩（块写出与桶级试压共用；ops 须 can_compress） */
static omnic_status arc_compress_mem(const uint8_t *buf, size_t n,
                                     const omnic_codec_ops *ops, int level, int threads,
                                     uint8_t **out, size_t *out_len) {
    omnic_reader r;
    omnic_writer w;
    omnic_reader_init_mem(&r, buf, n);
    omnic_writer_init_mem(&w);
    omnic_status st = ops->compress(&r, &w, level, threads);
    uint8_t *m = omnic_writer_mem_drain(&w, out_len);
    omnic_reader_fini(&r);
    if (st != OMNIC_OK) {
        free(m);
        return st;
    }
    *out = m;
    return OMNIC_OK;
}

/* ---- pack（A3：store-only；A4 接决策分桶与压缩块）---- */
omnic_status omnic_pack(const char *root, const char *archive_path,
                        const omnic_options *o, omnic_report *rep) {
    (void)o;
    if (root == NULL || archive_path == NULL) return OMNIC_E_USAGE;
    omnic_report tmp;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    FILE *f = NULL; /* 早期 goto fail 路径需要（初始化先于全部 goto） */
    file_plan *plan = NULL;
    arc_bucket *bkts = NULL;
    size_t nb = 0;

    /* T1/AR-ERR-002：输出归档不得位于源树内（自吞风险） */
    {
        char *ra = omnic_fs_abspath(root);
        char *pa = omnic_fs_abspath(archive_path);
        int inside = (ra != NULL && pa != NULL && omnic_fs_path_inside(ra, pa));
        free(ra);
        free(pa);
        if (inside) {
            omnic_set_error("output archive is inside the source tree: %s — use -o outside %s",
                            archive_path, root);
            return OMNIC_E_USAGE;
        }
    }

    size_t n = 0, skipped = 0;
    omnic_fs_entry *fs = omnic_fs_walk(root, &n, &skipped);
    if (fs == NULL && n == 0) {
        omnic_set_error("fs walk failed: %s", root);
        return OMNIC_E_SYSTEM;
    }
    if (n > 65535u) {
        omnic_set_error("too many entries (%zu > 65535) — v1 limit", n);
        free(fs);
        return OMNIC_E_USAGE;
    }

    arc_index ix;
    memset(&ix, 0, sizeof(ix));
    ix.ent = (arc_entry *)calloc(n ? n : 1, sizeof(arc_entry));
    if (ix.ent == NULL) {
        free(fs);
        return OMNIC_E_SYSTEM;
    }
    ix.nent = n;
    for (size_t i = 0; i < n; i++) {
        ix.ent[i].name = arc_dup_name(fs[i].rel);
        if (ix.ent[i].name == NULL) {
            omnic_set_error("entry name not representable: %s", fs[i].rel);
            goto fail;
        }
        ix.ent[i].type = fs[i].is_dir ? 1 : 0;
        ix.ent[i].mtime = fs[i].mtime;
    }

    f = omnic_fs_fopen(archive_path, "wb");
    if (f == NULL) {
        /* 父目录不存在则自动创建（产品友好），重试一次 */
        char parent[OMNIC_FS_PATH_MAX];
        if (snprintf(parent, sizeof(parent), "%s", archive_path) < (int)sizeof(parent)) {
            char *slash = strrchr(parent, '/');
            char *bs = strrchr(parent, '\\');
            if (bs != NULL && (slash == NULL || bs > slash)) slash = bs;
            if (slash != NULL) {
                *slash = '\0';
                if (omnic_fs_mkdir_p(parent) == OMNIC_OK)
                    f = omnic_fs_fopen(archive_path, "wb");
            }
        }
    }
    if (f == NULL) {
        omnic_set_error("cannot create archive: %s", archive_path);
        goto fail;
    }
    uint64_t arc_pos = ARC_HDR, data_off = ARC_HDR;
    uint64_t block_max = (o != NULL && o->block_mib > 0 && o->block_mib <= 1024)
                             ? (uint64_t)o->block_mib * 1024u * 1024u
                             : (uint64_t)OMNIC_ARC_BLOCK_MIB * 1024u * 1024u;

    uint8_t hdr[ARC_HDR];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, ARC_MAGIC_H, 8);
    put16(hdr + 8, OMNIC_ARC_VERSION);
    put16(hdr + 10, OMNIC_ARC_F_MTIME | OMNIC_ARC_F_INDEX_ZSTD);
    put32(hdr + 12, 0); /* n_blocks 回填于尾部 */
    put32(hdr + 16, (uint32_t)n);
    put64(hdr + 20, data_off);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto io_fail;

    size_t tbl_cap = 64;
    ix.blk = (arc_block *)malloc(tbl_cap * sizeof(arc_block));
    if (ix.blk == NULL) goto oom;
    double t0 = omnic_now_sec();
    unsigned quiet = (o != NULL && (o->flags & OMNIC_F_QUIET));
    unsigned explain = (o != NULL && (o->flags & OMNIC_F_EXPLAIN));
    int per_file = (o != NULL && (o->flags & OMNIC_F_PER_FILE));
    int threads = (o != NULL) ? o->threads : 0;

    /* ---- 决策 pass：逐文件分类建议（~1ms/文件，决策器只采样不落盘） ---- */
    plan = (file_plan *)calloc(n ? n : 1, sizeof(file_plan));
    if (plan == NULL) goto oom;
    for (size_t i = 0; i < n; i++) {
        if (ix.ent[i].type == 1) continue;
        char src[OMNIC_FS_PATH_MAX * 2];
        if (snprintf(src, sizeof(src), "%s/%s", root, fs[i].rel) >= (int)sizeof(src)) {
            omnic_set_error("path too long: %s", fs[i].rel);
            goto fail;
        }
        omnic_report arep;
        memset(&arep, 0, sizeof(arep));
        if (omnic_run_analyze(src, o, &arep) != OMNIC_OK) goto fail;
        if (strcmp(arep.codec, OMNIC_SKIP_CODEC) == 0) arep.skipped = true; /* 哨兵兜底 */
        if (arep.skipped && o != NULL && (o->flags & OMNIC_F_NO_SKIP)) {
            /* --no-skip：已压资产视同 unknown 入压缩桶（实验口径 zstd/19） */
            snprintf(plan[i].codec, sizeof(plan[i].codec), "zstd");
            plan[i].level = 19;
        } else if (arep.skipped || fs[i].size == 0) {
            /* SYS-CMP-003 skip 直通；0 字节文件按规格 §4.5 归 store（压空流无意义） */
            plan[i].store = 1;
        } else {
            snprintf(plan[i].codec, sizeof(plan[i].codec), "%s", arep.codec);
            plan[i].level = arep.level;
        }
        if (explain)
            printf("  %s: %s -> %s\n", fs[i].rel, omnic_class_str(arep.klass),
                   plan[i].store ? "pass-through" : plan[i].codec);
        rep->in_bytes += fs[i].size;
    }
    if (!quiet)
        printf("  classified %llu entries in %.2fs\n", (unsigned long long)n,
                omnic_now_sec() - t0);

    /* ---- 分桶（D10：同 (codec,level) 聚合；per-file 模式每文件独立桶） ---- */
    for (size_t i = 0; i < n; i++) {
        if (ix.ent[i].type == 1) continue;
        arc_bucket *t = NULL;
        if (plan[i].store) {
            for (size_t k = 0; k < nb; k++)
                if (bkts[k].is_store) { t = &bkts[k]; break; }
            if (t == NULL) {
                t = (arc_bucket *)realloc(bkts, (nb + 1) * sizeof(arc_bucket));
                if (t == NULL) goto oom;
                bkts = t;
                t = &bkts[nb++];
                memset(t, 0, sizeof(*t));
                t->is_store = 1;
            }
        } else if (per_file) {
            t = (arc_bucket *)realloc(bkts, (nb + 1) * sizeof(arc_bucket));
            if (t == NULL) goto oom;
            bkts = t;
            t = &bkts[nb++];
            memset(t, 0, sizeof(*t));
            snprintf(t->codec, sizeof(t->codec), "%s", plan[i].codec);
            t->level = plan[i].level;
        } else {
            for (size_t k = 0; k < nb; k++)
                if (!bkts[k].is_store && bkts[k].level == plan[i].level &&
                    strcmp(bkts[k].codec, plan[i].codec) == 0) {
                    t = &bkts[k];
                    break;
                }
            if (t == NULL) {
                t = (arc_bucket *)realloc(bkts, (nb + 1) * sizeof(arc_bucket));
                if (t == NULL) goto oom;
                bkts = t;
                t = &bkts[nb++];
                memset(t, 0, sizeof(*t));
                snprintf(t->codec, sizeof(t->codec), "%s", plan[i].codec);
                t->level = plan[i].level;
            }
        }
        if (bucket_push(t, i, fs[i].size) != 0) goto oom;
    }
    /* 桶序统一 qsort（大小降序；store 桶自然沉浮由总量决定）——确定性 */
    qsort(bkts, nb, sizeof(arc_bucket), bucket_cmp);

    /* ---- 桶写出 ---- */
    for (size_t bi = 0; bi < nb; bi++) {
        arc_bucket *b = &bkts[bi];
        if (b->is_store) {
            /* store：流式块（A3 路径），双 CRC 边写边算 */
            uint32_t blk_crc = 0;
            int blk_open = 0;
            for (size_t fi = 0; fi < b->n; fi++) {
                size_t i = b->idx[fi];
                char src[OMNIC_FS_PATH_MAX * 2];
                if (snprintf(src, sizeof(src), "%s/%s", root, fs[i].rel) >= (int)sizeof(src)) {
                    omnic_set_error("path too long: %s", fs[i].rel);
                    goto fail;
                }
                FILE *in = omnic_fs_fopen(src, "rb");
                if (in == NULL) {
                    omnic_set_error("cannot read source file: %s", src);
                    goto fail;
                }
                uint64_t cur = blk_open ? ix.blk[ix.nblk - 1].raw_size : 0;
                if (!blk_open || (cur > 0 && (cur + fs[i].size > block_max ||
                                              fs[i].size > block_max))) {
                    if (blk_open) ix.blk[ix.nblk - 1].crc = blk_crc;
                    if (arc_block_new(&ix, &tbl_cap, 0, 0, arc_pos - data_off) != 0) {
                        fclose(in);
                        goto oom;
                    }
                    blk_crc = 0;
                    blk_open = 1;
                }
                arc_block *cb = &ix.blk[ix.nblk - 1];
                ix.ent[i].block_id = (uint32_t)(ix.nblk - 1);
                ix.ent[i].in_off = cb->raw_size;
                ix.ent[i].size = fs[i].size;
                uint32_t file_crc = 0;
                uint8_t buf[64 * 1024];
                size_t r;
                while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
                    if (fwrite(buf, 1, r, f) != r) {
                        fclose(in);
                        goto io_fail;
                    }
                    arc_pos += r;
                    cb->raw_size += r;
                    cb->comp_size = cb->raw_size;
                    file_crc = arc_crc32(file_crc, buf, r);
                    blk_crc = arc_crc32(blk_crc, buf, r);
                }
                int bad = ferror(in);
                fclose(in);
                if (bad) {
                    omnic_set_error("read failed: %s", src);
                    goto fail;
                }
                ix.ent[i].crc = file_crc;
            }
            if (blk_open) ix.blk[ix.nblk - 1].crc = blk_crc;
            continue;
        }

        /* 压缩桶：内存块 + 桶级双候选（AR-PKG-003）。
         * 双候选仅 best_ratio 档：B 候选 xz/9 是 ratio 冲刺；速度档（BAL/FC/FD）
         * 查表即用——把 xz/9 塞给速度档用户会让 pack 时间失控（实测 BAL 19s）。 */
        const omnic_codec_ops *ops = omnic_codec_find(b->codec);
        if (ops == NULL || !ops->can_compress) {
            omnic_set_error("codec not available for bucket: %s", b->codec);
            goto fail;
        }
        int level = b->level;
        omnic_profile prof = (o != NULL) ? o->profile : OMNIC_PROFILE_BALANCED;
        const omnic_codec_ops *alt = NULL;
        if (prof == OMNIC_PROFILE_BEST_RATIO && b->total >= (4u << 20) &&
            !(strcmp(b->codec, "xz") == 0 && level >= 9)) {
            alt = omnic_codec_find("xz"); /* 桶级双候选的 B 候选 */
        }
        size_t fi = 0;
        int first = 1;
        while (fi < b->n) {
            /* 构块（文件边界对齐，单文件超限独立成块） */
            size_t bcap_bytes = (size_t)block_max;
            uint8_t *blk = NULL;
            uint64_t blk_len = 0;
            while (fi < b->n) {
                size_t i = b->idx[fi];
                if (blk_len > 0 && blk_len + fs[i].size > block_max) break;
                if (blk == NULL || fs[i].size + blk_len > bcap_bytes) {
                    size_t want = blk == NULL ? (size_t)block_max : (size_t)(fs[i].size + blk_len);
                    if (want < (size_t)(fs[i].size + blk_len)) want = (size_t)(fs[i].size + blk_len);
                    uint8_t *nb2 = (uint8_t *)realloc(blk, want);
                    if (nb2 == NULL) {
                        free(blk);
                        goto oom;
                    }
                    blk = nb2;
                    bcap_bytes = want;
                }
                char src[OMNIC_FS_PATH_MAX * 2];
                if (snprintf(src, sizeof(src), "%s/%s", root, fs[i].rel) >= (int)sizeof(src)) {
                    free(blk);
                    omnic_set_error("path too long: %s", fs[i].rel);
                    goto fail;
                }
                FILE *in = omnic_fs_fopen(src, "rb");
                if (in == NULL) {
                    free(blk);
                    omnic_set_error("cannot read source file: %s", src);
                    goto fail;
                }
                uint32_t file_crc = 0;
                size_t got = 0;
                while (got < fs[i].size) {
                    size_t r = fread(blk + blk_len, 1, (size_t)(fs[i].size - got), in);
                    if (r == 0) break;
                    file_crc = arc_crc32(file_crc, blk + blk_len, r);
                    blk_len += r;
                    got += r;
                }
                int bad = ferror(in);
                fclose(in);
                if (bad || got != fs[i].size) {
                    free(blk);
                    omnic_set_error("read failed: %s", src);
                    goto fail;
                }
                ix.ent[i].block_id = (uint32_t)ix.nblk; /* 本块即将入表 */
                ix.ent[i].in_off = blk_len - fs[i].size;
                ix.ent[i].size = fs[i].size;
                ix.ent[i].crc = file_crc;
                fi++;
            }
            /* 压缩（首块且桶 ≥4MiB 时双候选全量试压，AR-PKG-003） */
            uint8_t *out = NULL;
            size_t out_len = 0;
            if (first && alt != NULL) {
                uint8_t *oa = NULL, *ob = NULL;
                size_t la = 0, lb = 0;
                omnic_status sa = arc_compress_mem(blk, (size_t)blk_len, ops, level, threads, &oa, &la);
                if (sa != OMNIC_OK) {
                    free(blk);
                    goto fail;
                }
                omnic_status sb = arc_compress_mem(blk, (size_t)blk_len, alt, 9, threads, &ob, &lb);
                if (sb != OMNIC_OK) {
                    free(oa);
                    free(blk);
                    goto fail;
                }
                if (lb < la) {
                    free(oa);
                    out = ob;
                    out_len = lb;
                    ops = alt;
                    level = 9;
                } else {
                    free(ob);
                    out = oa;
                    out_len = la;
                }
            } else {
                if (arc_compress_mem(blk, (size_t)blk_len, ops, level, threads, &out, &out_len) !=
                    OMNIC_OK) {
                    free(blk);
                    goto fail;
                }
            }
            first = 0;
            free(blk);
            if (arc_block_new(&ix, &tbl_cap, (uint8_t)ops->fmt, (uint8_t)level,
                              arc_pos - data_off) != 0) {
                free(out);
                goto oom;
            }
            arc_block *cb = &ix.blk[ix.nblk - 1];
            cb->comp_size = out_len;
            cb->raw_size = blk_len;
            cb->crc = arc_crc32(0, out, out_len);
            if (fwrite(out, 1, out_len, f) != out_len) {
                free(out);
                goto io_fail;
            }
            arc_pos += out_len;
            free(out);
            if (!quiet)
                printf("  block: %s/%d  %llu -> %zu bytes (%.2f%%)\n", ops->name, level,
                       (unsigned long long)blk_len, out_len,
                       blk_len ? 100.0 * (double)out_len / (double)blk_len : 0.0);
        }
    }
    buckets_free(bkts, nb);
    bkts = NULL;
    nb = 0;
    free(plan);
    plan = NULL;
    rep->seconds = omnic_now_sec() - t0;

    /* 索引（zstd）+ Footer；n_blocks 回填 Header */
    bytebuf plain = {NULL, 0, 0};
    if (arc_index_serialize(ix.blk, ix.nblk, ix.ent, ix.nent, 1, &plain) != 0) goto oom;
    size_t zcap = ZSTD_compressBound(plain.n);
    uint8_t *zbuf = (uint8_t *)malloc(zcap);
    if (zbuf == NULL) {
        free(plain.buf);
        goto oom;
    }
    size_t zn = ZSTD_compress(zbuf, zcap, plain.buf, plain.n, 3);
    free(plain.buf);
    if (ZSTD_isError(zn)) {
        free(zbuf);
        omnic_set_error("index compress failed");
        goto fail;
    }
    uint64_t index_off = arc_pos;
    if (fwrite(zbuf, 1, zn, f) != zn) {
        free(zbuf);
        goto io_fail;
    }
    uint32_t index_crc = arc_crc32(0, zbuf, (size_t)zn);
    free(zbuf);

    uint32_t nb32 = (uint32_t)ix.nblk;
    uint8_t nbrow[4];
    put32(nbrow, nb32);
    if (arc_seek(f, 12) != 0 || fwrite(nbrow, 1, 4, f) != 4) goto io_fail;
    if (arc_seek(f, index_off + zn) != 0) goto io_fail; /* 回填后回到尾部写 Footer */

    uint8_t ftr[ARC_FTR];
    memcpy(ftr, ARC_MAGIC_F, 8);
    put64(ftr + 8, index_off);
    put64(ftr + 16, zn);
    put32(ftr + 24, index_crc);
    put16(ftr + 28, (uint16_t)ix.nent);
    put16(ftr + 30, (uint16_t)ix.nblk);
    if (fwrite(ftr, 1, sizeof(ftr), f) != sizeof(ftr)) goto io_fail;

    rep->out_bytes = index_off + zn + ARC_FTR;
    rep->ratio = rep->in_bytes ? (double)rep->out_bytes / (double)rep->in_bytes : 0.0;
    arc_index_mem_free(&ix);
    free(fs);
    if (fclose(f) != 0) return OMNIC_E_SYSTEM;
    return OMNIC_OK;

io_fail:
    omnic_set_error("write failed on archive: %s", archive_path);
    goto fail;
oom:
    omnic_set_error("out of memory");
fail:
    free(plan);
    buckets_free(bkts, nb);
    arc_index_mem_free(&ix);
    free(fs);
    if (f != NULL) fclose(f);
    return OMNIC_E_SYSTEM;
}

/* ---- unpack ---- */

static int arc_file_exists(const char *path) {
    FILE *f = omnic_fs_fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* 打开归档并解析索引（unpack/list/verify 共用，§4.1 打开顺序）。
 * 成功时 *out_f 与 *ix 归调用方清理（fclose / arc_index_mem_free）。 */
static omnic_status arc_open_index(const char *archive_path, FILE **out_f, arc_index *ix) {
    FILE *f = omnic_fs_fopen(archive_path, "rb");
    if (f == NULL) {
        omnic_set_error("cannot open archive: %s", archive_path);
        return OMNIC_E_SYSTEM;
    }
    uint8_t hdr[ARC_HDR], ftr[ARC_FTR];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, ARC_MAGIC_H, 8) != 0) {
        omnic_set_error("not a valid .omnic archive (bad header magic)");
        fclose(f);
        return OMNIC_E_DATA;
    }
    if (get16(hdr + 8) != OMNIC_ARC_VERSION) {
        omnic_set_error("unsupported .omnic version %u", get16(hdr + 8));
        fclose(f);
        return OMNIC_E_DATA;
    }
    if (fseek(f, -(long)sizeof(ftr), SEEK_END) != 0 ||
        fread(ftr, 1, sizeof(ftr), f) != sizeof(ftr) || memcmp(ftr, ARC_MAGIC_F, 8) != 0) {
        omnic_set_error("not a valid .omnic archive (bad footer magic)");
        fclose(f);
        return OMNIC_E_DATA;
    }
    uint64_t index_off = get64(ftr + 8), index_comp = get64(ftr + 16);
    uint32_t index_crc = get32(ftr + 24);
    uint32_t nent = get16(ftr + 28), nblk = get16(ftr + 30);
    if (get32(hdr + 16) != nent || get32(hdr + 12) != nblk) {
        omnic_set_error("archive header/footer mismatch — corrupt archive");
        fclose(f);
        return OMNIC_E_DATA;
    }
    if (index_off < ARC_HDR || index_comp == 0 || index_comp > (1u << 30)) {
        omnic_set_error("archive index out of range — corrupt archive");
        fclose(f);
        return OMNIC_E_DATA;
    }
    uint8_t *zbuf = (uint8_t *)malloc((size_t)index_comp);
    if (zbuf == NULL) {
        fclose(f);
        return OMNIC_E_SYSTEM;
    }
    if (arc_seek(f, index_off) != 0 ||
        fread(zbuf, 1, (size_t)index_comp, f) != (size_t)index_comp) {
        free(zbuf);
        omnic_set_error("archive index truncated — cannot list or extract");
        fclose(f);
        return OMNIC_E_DATA;
    }
    if (arc_crc32(0, zbuf, (size_t)index_comp) != index_crc) {
        free(zbuf);
        omnic_set_error("archive index corrupted — cannot list or extract");
        fclose(f);
        return OMNIC_E_DATA;
    }
    uint64_t plain_cap = ZSTD_getFrameContentSize(zbuf, (size_t)index_comp);
    if (plain_cap == 0 || plain_cap == ZSTD_CONTENTSIZE_ERROR ||
        plain_cap == ZSTD_CONTENTSIZE_UNKNOWN || plain_cap > (64u << 20)) {
        free(zbuf);
        fclose(f);
        return OMNIC_E_DATA;
    }
    uint8_t *plain = (uint8_t *)malloc((size_t)plain_cap);
    if (plain == NULL) {
        free(zbuf);
        fclose(f);
        return OMNIC_E_SYSTEM;
    }
    size_t plain_n = ZSTD_decompress(plain, (size_t)plain_cap, zbuf, (size_t)index_comp);
    free(zbuf);
    if (ZSTD_isError(plain_n)) {
        free(plain);
        omnic_set_error("archive index corrupted — cannot list or extract");
        fclose(f);
        return OMNIC_E_DATA;
    }
    omnic_status pst = arc_index_parse(plain, plain_n, nent, nblk, 1, ix);
    free(plain);
    if (pst == OMNIC_E_SYSTEM) {
        fclose(f);
        return OMNIC_E_SYSTEM;
    }
    if (pst != OMNIC_OK) {
        if (omnic_last_error()[0] == '\0') omnic_set_error("corrupt archive index");
        fclose(f);
        return OMNIC_E_DATA;
    }
    *out_f = f;
    return OMNIC_OK;
}

/* T2 模板：坏块报告含受影响条目名（至多 10 个，§5.4） */
static void report_bad_block(const arc_index *ix, size_t bi, unsigned fmt, unsigned quiet) {
    if (quiet) return;
    fprintf(stderr, "omnic: block %zu (fmt %u) corrupted — skipped, affected:", bi, fmt);
    size_t shown = 0;
    for (size_t i = 0; i < ix->nent && shown < 10; i++)
        if (ix->ent[i].type == 0 && ix->ent[i].block_id == (uint32_t)bi) {
            fprintf(stderr, " %s", ix->ent[i].name);
            shown++;
        }
    fprintf(stderr, "\n");
}

/* ---- 解包并行写盘（DESIGN-parallel-unpack，v0.3.1）----
 * 建盘路径是解包墙钟大头（8,586 文件 3.8s：syscall+AV 延迟，非 CPU），解码仅
 * 0.56s。主线程逐块解码 + worker 池并行落盘，解码被写盘掩盖。
 * 语义：坏块/条目校验/存在性全在主线程派发前拦截（顺序确定），worker 只做
 * 纯 I/O；提取/失败计数与串行路径同一口径。 */
#define ARC_NWRITER_DEFAULT 8
#define ARC_NWRITER_MAX 64
#define ARC_PAR_MIN_JOBS 32 /* 低于此数走串行：--file 单提取不付池启动成本 */
#define ARC_SLOT_RING 4     /* 在飞块上限 = 背压；内存上界 ≈ 4 × block_max */

/* 落盘一个条目（串行/并行共用）。dst 为完整目标路径。
 * 返回 0=成功 1=无法创建 2=写入不完整。 */
static int write_entry(const char *dst, const uint8_t *data, const arc_entry *e) {
    int opened = 0;
    omnic_status st = omnic_fs_write_file(dst, data, (size_t)e->size, e->mtime, &opened);
    if (st != OMNIC_OK && opened == 0) {
        /* 兜底：目录条目序外缺失（畸形归档）才补建父目录——正常路径目录先行
         * 已建全，逐文件 mkdir_p 的 syscall 开销占解压墙钟大头 */
        char *slash = strrchr(dst, '/');
        if (slash != NULL) {
            *slash = '\0';
            omnic_status ms = omnic_fs_mkdir_p(dst);
            *slash = '/';
            if (ms == OMNIC_OK)
                st = omnic_fs_write_file(dst, data, (size_t)e->size, e->mtime, &opened);
        }
    }
    if (st == OMNIC_OK) return 0;
    return opened ? 2 : 1;
}

/* 解码单块为裸字节（store 直读 / 压缩流解压，块层 CRC 先行）。
 * 返回 0=成功 1=数据损坏（坏块口径）2=无解码器 3=OOM。 */
static int decode_block(FILE *f, const arc_block *b, uint8_t **raw) {
    if (b->fmt == 0) { /* store：直读（+1 防 raw_size==0 的 malloc(0)） */
        uint8_t *m = (uint8_t *)malloc((size_t)b->raw_size + 1);
        if (m == NULL) return 3;
        if (arc_seek(f, ARC_HDR + b->offset) != 0 ||
            fread(m, 1, (size_t)b->raw_size, f) != (size_t)b->raw_size ||
            arc_crc32(0, m, (size_t)b->raw_size) != b->crc) {
            free(m);
            return 1;
        }
        *raw = m;
        return 0;
    }
    const omnic_codec_ops *ops = omnic_codec_for_fmt((omnic_fmt_id)b->fmt);
    if (ops == NULL || !ops->can_decompress) return 2;
    uint8_t *cbuf = (uint8_t *)malloc((size_t)b->comp_size);
    if (cbuf == NULL) return 3;
    if (arc_seek(f, ARC_HDR + b->offset) != 0 ||
        fread(cbuf, 1, (size_t)b->comp_size, f) != (size_t)b->comp_size ||
        arc_crc32(0, cbuf, (size_t)b->comp_size) != b->crc) {
        free(cbuf);
        return 1;
    }
    omnic_reader rr;
    omnic_writer ww;
    omnic_reader_init_mem(&rr, cbuf, (size_t)b->comp_size);
    omnic_writer_init_mem(&ww);
    omnic_status st = ops->decompress(&rr, &ww);
    size_t out_len = 0;
    uint8_t *out_buf = omnic_writer_mem_drain(&ww, &out_len);
    omnic_reader_fini(&rr);
    free(cbuf);
    if (st != OMNIC_OK || out_buf == NULL || out_len != (size_t)b->raw_size) {
        free(out_buf);
        return 1;
    }
    *raw = out_buf;
    return 0;
}

typedef struct arc_job {
    size_t ent_idx;
    int slot;
    struct arc_job *next;
} arc_job;

typedef struct {
    uint8_t *buf; /* busy 期间有效；open 关闭且 pending 归零后回收 */
    size_t pending;
    int busy;
    int open; /* 主线程派发中：worker 不得因 pending 归零回收本槽 */
} arc_wslot;

typedef struct {
    const arc_index *ix; /* 只读共享 */
    const char *dstroot;
    int8_t *result;      /* [ent] 0=成功 1=无法创建 2=短写 3=OOM */
    arc_mutex_t mu;
    arc_cond_t cv_work;  /* worker：队列空且未停机 */
    arc_cond_t cv_done;  /* producer：槽释放 */
    arc_job *head, *tail;
    arc_wslot slot[ARC_SLOT_RING];
    int shutdown;
    arc_thread_t tid[ARC_NWRITER_MAX];
    int nworkers;
} arc_pool;

/* worker：纯 I/O 落盘。槽数据在块关闭（open=0）且 pending 归零前保证有效
 * （无锁读安全：enqueue 的锁序建立 happens-before）；结果码经 result 数组
 * 回传，主线程 drain 后按条目序渲染。 */
ARC_THREAD_FN writer_main(void *arg) {
    arc_pool *p = (arc_pool *)arg;
    for (;;) {
        arc_mutex_lock(&p->mu);
        while (p->head == NULL && !p->shutdown) arc_cond_wait(&p->cv_work, &p->mu);
        arc_job *j = p->head;
        if (j != NULL) {
            p->head = j->next;
            if (p->head == NULL) p->tail = NULL;
        }
        arc_mutex_unlock(&p->mu);
        if (j == NULL) break; /* 停机且队列已清空 */
        const arc_entry *e = &p->ix->ent[j->ent_idx];
        int rc = 3;
        size_t dl = strlen(p->dstroot) + strlen(e->name) + 2;
        char *dst = (char *)malloc(dl);
        if (dst != NULL) {
            snprintf(dst, dl, "%s/%s", p->dstroot, e->name);
            rc = write_entry(dst, p->slot[j->slot].buf + e->in_off, e);
            free(dst);
        }
        arc_mutex_lock(&p->mu);
        p->result[j->ent_idx] = (int8_t)rc;
        arc_wslot *s = &p->slot[j->slot];
        if (--s->pending == 0 && !s->open) {
            free(s->buf);
            s->buf = NULL;
            s->busy = 0;
        }
        arc_mutex_unlock(&p->mu);
        free(j);
        arc_cond_broadcast(&p->cv_done);
    }
    ARC_THREAD_RET_OK;
}

/* 起 worker 池；失败（无一线程可建）返回 -1，调用方退回串行路径。 */
static int pool_init(arc_pool *p, const arc_index *ix, const char *dstroot, int writers) {
    memset(p, 0, sizeof(*p));
    p->ix = ix;
    p->dstroot = dstroot;
    p->result = (int8_t *)calloc(ix->nent ? ix->nent : 1, 1);
    if (p->result == NULL) return -1;
    arc_mutex_init(&p->mu);
    arc_cond_init(&p->cv_work);
    arc_cond_init(&p->cv_done);
    for (int i = 0; i < writers; i++) {
        if (arc_thread_create(&p->tid[i], writer_main, p) != 0) break;
        p->nworkers++;
    }
    if (p->nworkers == 0) {
        free(p->result);
        return -1;
    }
    return 0;
}

/* 等待并占用一个空闲槽（背压：在飞块数 ≤ ARC_SLOT_RING）。
 * 占用期间 buf 由主线程填充（pending==0，无 worker 触碰）。 */
static int pool_acquire_slot(arc_pool *p) {
    arc_mutex_lock(&p->mu);
    for (;;) {
        for (int s = 0; s < ARC_SLOT_RING; s++) {
            if (!p->slot[s].busy) {
                p->slot[s].busy = 1;
                p->slot[s].open = 1;
                p->slot[s].buf = NULL;
                p->slot[s].pending = 0;
                arc_mutex_unlock(&p->mu);
                return s;
            }
        }
        arc_cond_wait(&p->cv_done, &p->mu);
    }
}

/* 派发写作业；OOM 按该条目失败记账（语义同 worker 的 rc=3）。 */
static void pool_enqueue(arc_pool *p, size_t ent_idx, int slot) {
    arc_job *j = (arc_job *)malloc(sizeof(*j));
    arc_mutex_lock(&p->mu);
    if (j == NULL) {
        p->result[ent_idx] = 3;
    } else {
        j->ent_idx = ent_idx;
        j->slot = slot;
        j->next = NULL;
        if (p->tail != NULL) p->tail->next = j;
        else p->head = j;
        p->tail = j;
        p->slot[slot].pending++;
    }
    arc_mutex_unlock(&p->mu);
    if (j != NULL) arc_cond_signal(&p->cv_work);
}

/* 块派发结束：关槽。worker 可能赶在主线程前面清空 pending，open 期间不得
 * 回收（主线程仍要读 buf 做条目 CRC）；关闭时已清零则就地回收，否则留给
 * 最后一个完成的 worker。decode 失败路径复用（buf 可能为 NULL）。 */
static void pool_close_slot(arc_pool *p, int slot) {
    arc_mutex_lock(&p->mu);
    p->slot[slot].open = 0;
    if (p->slot[slot].busy && p->slot[slot].pending == 0) {
        free(p->slot[slot].buf);
        p->slot[slot].buf = NULL;
        p->slot[slot].busy = 0;
    }
    arc_mutex_unlock(&p->mu);
}

/* 停机：置位后广播，worker 排空队列即退出（全部作业完成才 join 返回）。 */
static void pool_shutdown(arc_pool *p) {
    arc_mutex_lock(&p->mu);
    p->shutdown = 1;
    arc_mutex_unlock(&p->mu);
    arc_cond_broadcast(&p->cv_work);
    for (int i = 0; i < p->nworkers; i++) arc_thread_join(p->tid[i]);
}

/* unpack_core：整树提取 / --file 过滤提取 / --verify 校验三种形态共用。
 * only!=NULL 时仅解命中条目所在块；verify_only 时不落盘只验双层 CRC。 */
static omnic_status unpack_core(const char *archive_path, const char *dest_dir,
                                const char *const *only, size_t n_only, int verify_only,
                                const omnic_options *o, omnic_report *rep) {
    if (archive_path == NULL) return OMNIC_E_USAGE;
    omnic_report tmp;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    const char *dstroot = (dest_dir != NULL) ? dest_dir : ".";
    unsigned quiet = (o != NULL && (o->flags & OMNIC_F_QUIET));

    FILE *f = NULL;
    arc_index ix;
    omnic_status ost = arc_open_index(archive_path, &f, &ix);
    if (ost != OMNIC_OK) return ost;

    /* --file 名匹配：未命中即用法错误（可操作）；命中标记条目与所需块 */
    unsigned char *want_ent = NULL, *want_blk = NULL;
    size_t *blk_cnt = NULL, *blk_off = NULL, *ent_ord = NULL;
    if (n_only > 0) {
        want_ent = (unsigned char *)calloc(ix.nent ? ix.nent : 1, 1);
        want_blk = (unsigned char *)calloc(ix.nblk ? ix.nblk : 1, 1);
        if (want_ent == NULL || want_blk == NULL) goto sys_fail_ix;
        size_t missing = 0;
        for (size_t k = 0; k < n_only; k++) {
            size_t i;
            for (i = 0; i < ix.nent; i++) {
                if (ix.ent[i].type == 0 && strcmp(ix.ent[i].name, only[k]) == 0) break;
            }
            if (i == ix.nent) {
                fprintf(stderr, "omnic: --file not in archive: %s\n", only[k]);
                missing++;
            } else {
                want_ent[i] = 1;
                want_blk[ix.ent[i].block_id] = 1;
            }
        }
        if (missing > 0) {
            free(want_ent);
            free(want_blk);
            arc_index_mem_free(&ix);
            fclose(f);
            return OMNIC_E_USAGE;
        }
    }
    /* 分桶（D8）：非目录条目按 block_id 计数排序，替代逐块 O(nent) 全表扫描；
     * 桶计数即坏块失败口径（原 blk_ents）。njobs = 待写作业总数。 */
    blk_cnt = (size_t *)calloc(ix.nblk ? ix.nblk : 1, sizeof(size_t));
    blk_off = (size_t *)malloc((ix.nblk + 1) * sizeof(size_t));
    ent_ord = (size_t *)malloc((ix.nent ? ix.nent : 1) * sizeof(size_t));
    if (blk_cnt == NULL || blk_off == NULL || ent_ord == NULL) goto sys_fail_ix;
    size_t njobs = 0;
    for (size_t i = 0; i < ix.nent; i++)
        if (ix.ent[i].type == 0) {
            blk_cnt[ix.ent[i].block_id]++;
            if (want_ent == NULL || want_ent[i]) njobs++;
        }
    blk_off[0] = 0;
    for (size_t bi = 0; bi < ix.nblk; bi++) blk_off[bi + 1] = blk_off[bi] + blk_cnt[bi];
    {
        size_t *cur = (size_t *)calloc(ix.nblk ? ix.nblk : 1, sizeof(size_t));
        if (cur == NULL) goto sys_fail_ix;
        for (size_t i = 0; i < ix.nent; i++)
            if (ix.ent[i].type == 0)
                ent_ord[blk_off[ix.ent[i].block_id] + cur[ix.ent[i].block_id]++] = i;
        free(cur);
    }

    if (!verify_only && want_ent == NULL && omnic_fs_mkdir_p(dstroot) != OMNIC_OK)
        goto sys_fail_ix;
    /* 目录条目先行（含空目录；仅全量提取） */
    if (!verify_only && want_ent == NULL) {
        for (size_t i = 0; i < ix.nent; i++) {
            if (ix.ent[i].type != 1) continue;
            char *dst = (char *)malloc(strlen(dstroot) + strlen(ix.ent[i].name) + 2);
            if (dst == NULL) goto sys_fail_ix;
            sprintf(dst, "%s/%s", dstroot, ix.ent[i].name);
            omnic_status st = omnic_fs_mkdir_p(dst);
            if (st == OMNIC_OK) omnic_fs_set_mtime(dst, ix.ent[i].mtime);
            free(dst);
            if (st != OMNIC_OK) {
                omnic_set_error("cannot create directory under %s", dstroot);
                goto sys_fail_ix;
            }
        }
    }

    /* 并行判定：verify/单线程/小作业量走串行原路径（--file 单提取 8.7ms 不启池） */
    int writers = ARC_NWRITER_DEFAULT;
    if (o != NULL && o->threads == 1) writers = 1;
    else if (o != NULL && o->threads > 1) writers = o->threads;
    if (writers > ARC_NWRITER_MAX) writers = ARC_NWRITER_MAX;
    arc_pool pool;
    int parallel = (!verify_only && writers > 1 && njobs >= ARC_PAR_MIN_JOBS &&
                    pool_init(&pool, &ix, dstroot, writers) == 0);
    unsigned char *went = NULL;
    if (parallel) {
        went = (unsigned char *)calloc(ix.nent ? ix.nent : 1, 1);
        if (went == NULL) { /* OOM：停池退回串行 */
            pool_shutdown(&pool);
            free(pool.result);
            parallel = 0;
        }
    }

    /* 逐块解压切条目；坏块跳过隔离（AR-ERR-001，T2 模板） */
    size_t extracted = 0, failed = 0, blocks_ok = 0;
    for (size_t bi = 0; bi < ix.nblk; bi++) {
        const arc_block *b = &ix.blk[bi];
        if (want_blk != NULL && !want_blk[bi]) continue; /* --file：无目标条目的块不解 */
        int slot = parallel ? pool_acquire_slot(&pool) : -1;
        uint8_t *raw = NULL; /* 本块原始字节（store 直读 / 压缩解压产物） */
        int drc = decode_block(f, b, &raw);
        if (drc != 0) {
            if (drc == 1) report_bad_block(&ix, bi, b->fmt, quiet);
            else if (drc == 2) omnic_set_error("block %zu: no decoder for format %u", bi, b->fmt);
            else omnic_set_error("out of memory");
            failed += blk_cnt[bi]; /* 坏块的条目全计失败：extracted N/M 口径 */
            if (parallel) pool_close_slot(&pool, slot);
            continue;
        }
        if (parallel) pool.slot[slot].buf = raw; /* open 且 pending==0：无 worker 触碰，直接赋值 */
        blocks_ok++;
        for (size_t k = blk_off[bi]; k < blk_off[bi + 1]; k++) {
            size_t i = ent_ord[k];
            arc_entry *e = &ix.ent[i];
            if (want_ent != NULL && !want_ent[i]) continue; /* --file：仅目标条目 */
            /* 块内偏移越界防御（无溢出写法）；条目 CRC 为双层校验内层（§4.2） */
            if (e->size > b->raw_size || e->in_off > b->raw_size - e->size ||
                arc_crc32(0, raw + e->in_off, (size_t)e->size) != e->crc) {
                if (!quiet)
                    fprintf(stderr, "omnic: entry corrupted — skipped: %s\n", e->name);
                failed++;
                continue;
            }
            if (verify_only) { /* --verify：校验通过即计入，不落盘 */
                rep->out_bytes += e->size;
                extracted++;
                continue;
            }
            /* --file 目标已存在且未 -f → 拒绝（T5）；并行路径派发前主线程拦截 */
            if (want_ent != NULL) {
                size_t dl = strlen(dstroot) + strlen(e->name) + 2;
                char *dst = (char *)malloc(dl);
                if (dst == NULL) { /* OOM 边角：按条目失败计，避免并行收尾复杂化 */
                    failed++;
                    continue;
                }
                sprintf(dst, "%s/%s", dstroot, e->name);
                int deny = arc_file_exists(dst) && !(o != NULL && (o->flags & OMNIC_F_FORCE));
                if (deny) omnic_set_error("'%s' already exists — use -f to overwrite", dst);
                free(dst);
                if (deny) {
                    failed++;
                    continue;
                }
            }
            if (parallel) {
                went[i] = 1;
                pool_enqueue(&pool, i, slot);
            } else {
                size_t dl = strlen(dstroot) + strlen(e->name) + 2;
                char *dst = (char *)malloc(dl);
                if (dst == NULL) {
                    free(raw);
                    goto sys_fail_ix;
                }
                sprintf(dst, "%s/%s", dstroot, e->name);
                int rc = write_entry(dst, raw + e->in_off, e);
                if (rc == 1) omnic_set_error("cannot create: %s", dst);
                free(dst);
                if (rc != 0) {
                    failed++;
                    continue;
                }
                rep->out_bytes += e->size;
                extracted++;
            }
        }
        if (parallel) pool_close_slot(&pool, slot);
        else free(raw);
    }
    if (parallel) {
        pool_shutdown(&pool);
        /* drain 后按条目序结算：worker 纯 I/O 结果码（1=无法创建 3=OOM） */
        for (size_t i = 0; i < ix.nent; i++) {
            if (went[i] != 1) continue;
            if (pool.result[i] == 0) {
                rep->out_bytes += ix.ent[i].size;
                extracted++;
                continue;
            }
            failed++;
            if (pool.result[i] == 1) {
                size_t dl = strlen(dstroot) + strlen(ix.ent[i].name) + 2;
                char *dst = (char *)malloc(dl);
                if (dst != NULL) {
                    sprintf(dst, "%s/%s", dstroot, ix.ent[i].name);
                    omnic_set_error("cannot create: %s", dst);
                    free(dst);
                }
            } else if (pool.result[i] == 3) {
                omnic_set_error("out of memory");
            }
        }
        free(pool.result);
        free(went);
    }
    free(blk_cnt);
    free(blk_off);
    free(ent_ord);
    free(want_ent);
    free(want_blk);
    arc_index_mem_free(&ix);
    fclose(f);
    rep->in_bytes = rep->out_bytes;
    if (verify_only) {
        if (!quiet)
            printf("verified %zu/%zu files, %zu blocks OK\n", extracted, extracted + failed,
                   blocks_ok);
    } else if (failed > 0 && !quiet) {
        fprintf(stderr, "omnic: extracted %zu/%zu files (%zu failed)\n", extracted,
                extracted + failed, failed);
    }
    return failed ? OMNIC_E_DATA : OMNIC_OK;

sys_fail_ix:
    free(blk_cnt);
    free(blk_off);
    free(ent_ord);
    free(want_ent);
    free(want_blk);
    arc_index_mem_free(&ix);
    fclose(f);
    return OMNIC_E_SYSTEM;
}

omnic_status omnic_unpack(const char *archive_path, const char *dest_dir,
                          const omnic_options *o, omnic_report *rep) {
    return unpack_core(archive_path, dest_dir, NULL, 0, 0, o, rep);
}

omnic_status omnic_unpack_files(const char *archive_path, const char *dest_dir,
                                const char *const *names, size_t n_names,
                                const omnic_options *o, omnic_report *rep) {
    return unpack_core(archive_path, dest_dir, names, n_names, 0, o, rep);
}

omnic_status omnic_arc_verify(const char *archive_path, const omnic_options *o,
                              omnic_report *rep) {
    return unpack_core(archive_path, NULL, NULL, 0, 1, o, rep);
}

/* --list：只读索引不读 Data 区（§5.2）；ratio 为该条目所在块级压缩率 */
omnic_status omnic_arc_list(const char *archive_path, const omnic_options *o) {
    if (archive_path == NULL) return OMNIC_E_USAGE;
    FILE *f = NULL;
    arc_index ix;
    omnic_status st = arc_open_index(archive_path, &f, &ix);
    if (st != OMNIC_OK) return st;
    if (!(o != NULL && (o->flags & OMNIC_F_QUIET))) {
        size_t files = 0, dirs = 0;
        for (size_t i = 0; i < ix.nent; i++) {
            const arc_entry *e = &ix.ent[i];
            if (e->type == 1) {
                printf("%s\t-\tdir\n", e->name);
                dirs++;
                continue;
            }
            const arc_block *b = &ix.blk[e->block_id];
            printf("%s\t%llu\t%s\t%.1f%%\n", e->name, (unsigned long long)e->size,
                   b->fmt == 0 ? "store" : omnic_fmt_str((omnic_fmt_id)b->fmt),
                   b->raw_size ? 100.0 * (double)b->comp_size / (double)b->raw_size : 0.0);
            files++;
        }
        printf("%zu files, %zu dirs, %zu blocks\n", files, dirs, ix.nblk);
    }
    arc_index_mem_free(&ix);
    fclose(f);
    return OMNIC_OK;
}

#endif /* OMNIC_CODEC_ZSTD */
