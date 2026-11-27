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
/* 条目定长部分：v1 = type/pad/block_id/in_off/size/crc（26B，mtime 另计）；
 * v2 = type/flags/block_id/in_off/size/crc/mtime（34B，可选字段按固定次序跟在后面） */
#define ARC_ENT_FIX_V1 26u
#define ARC_ENT_FIX 34u
#define ARC_BLK_ROW 32u
static const uint8_t ARC_MAGIC_H[8] = {'O', 'M', 'N', 'I', 'C', 'A', 'R', 'C'};
static const uint8_t ARC_MAGIC_F[8] = {'O', 'M', 'N', 'I', 'C', 'E', 'N', 'D'};
#define ARC_NO_BLOCK 0xFFFFFFFFu /* 目录 / 硬链接条目的 block_id */

/* 条目类型与 v2 标志位（FORMAT.md §EntryTable）。未知标志位一律拒绝，扩展须升版本 */
enum { ARC_T_FILE = 0, ARC_T_DIR = 1, ARC_T_SYMLINK = 2, ARC_T_HARDLINK = 3 };
#define ARC_EF_LINK_DIR 0x01u      /* 符号链接的目标是目录 */
#define ARC_EF_LINK_JUNCTION 0x02u /* Windows 目录联接（目标为绝对路径） */
#define ARC_EF_HAS_CTIME 0x04u
#define ARC_EF_HAS_MODE 0x08u
#define ARC_EF_HAS_ATTRS 0x10u
#define ARC_EF_HAS_LINKTO 0x40u /* 硬链接：link_to 字段存在 */
#define ARC_EF_KNOWN 0x5Fu
#define ARC_LINK_MAX (1u << 20) /* 链接目标的字节上限：畸形索引不得撑爆内存 */

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
    uint8_t type; /* ARC_T_* */
    uint8_t flags; /* ARC_EF_* */
    uint32_t block_id; /* 有数据的条目（文件、符号链接）所在块；否则 ARC_NO_BLOCK */
    uint64_t in_off, size; /* 符号链接的 size = 目标串字节数 */
    uint32_t crc;
    long long mtime;
    long long ctime;  /* 0 = 未存 */
    unsigned mode;    /* 0 = 未存 */
    unsigned attrs;   /* 0 = 未存 */
    uint32_t link_to; /* 硬链接：指向的文件条目下标 */
} arc_entry;

typedef struct {
    arc_block *blk;
    size_t nblk;
    arc_entry *ent;
    size_t nent;
    unsigned version;
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

/* ---- 索引明文序列化（写入端只写 v2）：块表(32B/块) ‖ 条目表。
 * 条目 = name_len/name + 定长 34B + 可选字段（次序固定：link_to, ctime, mode, attrs）。 ---- */
static int arc_index_serialize(const arc_block *blk, size_t nblk, const arc_entry *ent,
                               size_t nent, bytebuf *out) {
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
        uint8_t nlb[2];
        put16(nlb, nl);
        if (bb_put(out, nlb, 2) != 0) return -1;
        if (bb_put(out, ent[i].name, nl) != 0) return -1;
        uint8_t row[ARC_ENT_FIX + 4 + 8 + 2 + 2];
        memset(row, 0, sizeof(row));
        row[0] = ent[i].type;
        row[1] = ent[i].flags;
        put32(row + 2, ent[i].block_id);
        put64(row + 6, ent[i].in_off);
        put64(row + 14, ent[i].size);
        put32(row + 22, ent[i].crc);
        put64(row + 26, (uint64_t)ent[i].mtime);
        size_t len = ARC_ENT_FIX;
        if (ent[i].flags & ARC_EF_HAS_LINKTO) {
            put32(row + len, ent[i].link_to);
            len += 4;
        }
        if (ent[i].flags & ARC_EF_HAS_CTIME) {
            put64(row + len, (uint64_t)ent[i].ctime);
            len += 8;
        }
        if (ent[i].flags & ARC_EF_HAS_MODE) {
            put16(row + len, (uint16_t)ent[i].mode);
            len += 2;
        }
        if (ent[i].flags & ARC_EF_HAS_ATTRS) {
            put16(row + len, (uint16_t)ent[i].attrs);
            len += 2;
        }
        if (bb_put(out, row, len) != 0) return -1;
    }
    return 0;
}

/* ---- unpack：索引解析（全部结构校验在此，FORMAT.md）。v1 与 v2 的条目记录分别解析，
 * 归一到同一张内存表。---- */
static omnic_status arc_index_parse(const uint8_t *p, size_t n, uint32_t nent,
                                    uint32_t nblk, unsigned version, int has_mtime,
                                    arc_index *ix) {
    memset(ix, 0, sizeof(*ix));
    ix->version = version;
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
    const size_t rowlen =
        (version == 1) ? ARC_ENT_FIX_V1 + (has_mtime ? 8u : 0u) : ARC_ENT_FIX;
    /* 计数来自不可信的头：按每条记录的最小长度（name_len + 1 字节名字 + 定长）先卡一道，
     * 再按它分配 */
    if (nent > 0 && (uint64_t)nent * (2u + 1u + rowlen) > (uint64_t)n) return OMNIC_E_DATA;
    if (nent > 0) {
        ix->ent = (arc_entry *)calloc(nent, sizeof(arc_entry));
        if (ix->ent == NULL) return OMNIC_E_SYSTEM;
        ix->nent = nent;
    }
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
        arc_entry *e = &ix->ent[i];
        e->name = name;
        e->type = p[0];
        e->block_id = get32(p + 2);
        e->in_off = get64(p + 6);
        e->size = get64(p + 14);
        e->crc = get32(p + 22);
        if (version == 1) {
            if (has_mtime) e->mtime = (long long)get64(p + 26);
            if (e->type != ARC_T_FILE && e->type != ARC_T_DIR) return OMNIC_E_DATA; /* v1 只认文件与目录 */
        } else {
            e->flags = p[1];
            e->mtime = (long long)get64(p + 26);
        }
        p += rowlen;
        n -= rowlen;
        if (version == 1) {
            /* v1 目录条目的块号：规范写 0xFFFFFFFF，参考实现实际写的是 0——两种都收 */
            if (e->type == ARC_T_DIR) {
                if (e->size != 0 || e->in_off != 0 || e->crc != 0) return OMNIC_E_DATA;
                e->block_id = ARC_NO_BLOCK;
            } else if (e->block_id >= ix->nblk || e->in_off > (1u << 30) || e->size > (1u << 30) ||
                       e->in_off + e->size > ix->blk[e->block_id].raw_size) {
                return OMNIC_E_DATA;
            }
            continue;
        }
        /* v2：未知标志位一律拒绝；可选字段按固定次序跟随 */
        if (e->type > ARC_T_HARDLINK || (e->flags & ~ARC_EF_KNOWN) != 0) return OMNIC_E_DATA;
        if ((e->flags & ARC_EF_HAS_LINKTO) != 0) {
            if (n < 4) return OMNIC_E_DATA;
            e->link_to = get32(p);
            p += 4;
            n -= 4;
        }
        if ((e->flags & ARC_EF_HAS_CTIME) != 0) {
            if (n < 8) return OMNIC_E_DATA;
            e->ctime = (long long)get64(p);
            p += 8;
            n -= 8;
        }
        if ((e->flags & ARC_EF_HAS_MODE) != 0) {
            if (n < 2) return OMNIC_E_DATA;
            e->mode = get16(p);
            if (e->mode & ~07777u) return OMNIC_E_DATA;
            p += 2;
            n -= 2;
        }
        if ((e->flags & ARC_EF_HAS_ATTRS) != 0) {
            if (n < 2) return OMNIC_E_DATA;
            e->attrs = get16(p);
            if (e->attrs & ~OMNIC_FS_ATTR_MASK) return OMNIC_E_DATA;
            p += 2;
            n -= 2;
        }
        if (e->type == ARC_T_FILE || e->type == ARC_T_SYMLINK) {
            if (e->block_id >= ix->nblk || e->in_off > (1u << 30) || e->size > (1u << 30) ||
                e->in_off + e->size > ix->blk[e->block_id].raw_size)
                return OMNIC_E_DATA;
            if (e->type == ARC_T_SYMLINK) { /* 链接标志只属于链接条目；目标串非空、有上限 */
                if (e->size == 0 || e->size > ARC_LINK_MAX ||
                    ((e->flags & ARC_EF_LINK_DIR) != 0 && (e->flags & ARC_EF_LINK_JUNCTION) != 0))
                    return OMNIC_E_DATA;
            } else if ((e->flags & (ARC_EF_LINK_DIR | ARC_EF_LINK_JUNCTION | ARC_EF_HAS_LINKTO)) != 0) {
                return OMNIC_E_DATA;
            }
        } else {
            if (e->size != 0 || e->in_off != 0 || e->crc != 0 || e->block_id != ARC_NO_BLOCK)
                return OMNIC_E_DATA;
            if (e->type == ARC_T_DIR) {
                if ((e->flags & (ARC_EF_LINK_DIR | ARC_EF_LINK_JUNCTION | ARC_EF_HAS_LINKTO)) != 0)
                    return OMNIC_E_DATA;
            } else if ((e->flags & ARC_EF_HAS_LINKTO) == 0) {
                return OMNIC_E_DATA; /* 硬链接必须指明目标条目 */
            }
        }
    }
    /* 硬链接的目标必须是普通文件条目：不认另一条硬链接，也不认自己 */
    for (uint32_t i = 0; i < ix->nent; i++) {
        if (ix->ent[i].type != ARC_T_HARDLINK) continue;
        uint32_t t = ix->ent[i].link_to;
        if (t == i || t >= ix->nent || ix->ent[t].type != ARC_T_FILE) return OMNIC_E_DATA;
    }
    /* 计数与内容必须严格吻合：索引尾部有剩余字节 = 头部计数被改小，不能悄悄少解条目 */
    if (n != 0) return OMNIC_E_DATA;
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

/* 文件身份（硬链接识别）：分类时每个普通文件顺手取一次（Windows 上约 16 µs，
 * 摊在分类池里；POSIX 上就是一次 stat）。全 0 = 未知。 */
typedef struct {
    unsigned long long dev, ino;
    size_t idx;
} arc_fid;

static int fid_cmp(const void *pa, const void *pb) {
    const arc_fid *a = (const arc_fid *)pa, *b = (const arc_fid *)pb;
    if (a->dev != b->dev) return a->dev < b->dev ? -1 : 1;
    if (a->ino != b->ino) return a->ino < b->ino ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

/* 同一文件身份的多个目录项：索引序里首个持有数据，其余成为指向它的硬链接条目
 * （与线程调度无关，产物确定）。ids 按下标对应条目，调用后被排序。 */
static void mark_hardlinks(arc_index *ix, arc_fid *ids, size_t n) {
    qsort(ids, n, sizeof(*ids), fid_cmp);
    for (size_t k = 1; k < n; k++) {
        const arc_fid *a = &ids[k - 1], *b = &ids[k];
        if (b->dev == 0 && b->ino == 0) continue;
        if (a->dev != b->dev || a->ino != b->ino) continue;
        size_t owner = a->idx; /* 第三个及以后的链接：前一项可能已是硬链接条目 */
        while (ix->ent[owner].type == ARC_T_HARDLINK) owner = ix->ent[owner].link_to;
        ix->ent[b->idx].type = ARC_T_HARDLINK;
        ix->ent[b->idx].flags |= ARC_EF_HAS_LINKTO;
        ix->ent[b->idx].link_to = (uint32_t)owner;
    }
}

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

/* ---- 并行分类池（DESIGN-parallel-pack-classify）：条目级独立作业，
 * 批量领号 + plan[i] 按下标私有写，无队列无共享写路径。错误只记录首个
 * （路径引用 fs[].rel，生命周期覆盖 join），join 后主线程统一写报错——
 * g_err 非线程安全，worker 不直接碰。 ---- */
#define CLS_BATCH 16
#define ARC_NWRITER_DEFAULT 8
#define ARC_NWRITER_MAX 64
#define ARC_PAR_MIN_JOBS 32 /* 条目数门槛：--file 单提取不付池启动成本 */
#define ARC_PAR_MIN_BYTES (8u << 20) /* 写量门槛：少而大的 solid 形态同样启池 */

typedef struct {
    const omnic_fs_entry *fs;
    const arc_index *ix;
    const char *root;
    const omnic_options *o;
    file_plan *plan;
    arc_fid *ids; /* [i]：按下标私有写，join 后主线程统一识别硬链接 */
    size_t n, next;
    int stop;
    omnic_status first_err;
    const char *err_rel;
    arc_mutex_t lock;
} cls_ctx;

static int cls_one(cls_ctx *c, size_t i) {
    char src[OMNIC_FS_PATH_MAX * 2];
    if (snprintf(src, sizeof(src), "%s/%s", c->root, c->fs[i].rel) >= (int)sizeof(src)) return -1;
    omnic_report arep;
    memset(&arep, 0, sizeof(arep));
    if (omnic_run_analyze(src, c->o, &arep) != OMNIC_OK) return -1;
    if (strcmp(arep.codec, OMNIC_SKIP_CODEC) == 0) arep.skipped = true; /* 哨兵兜底 */
    if (arep.skipped && c->o != NULL && (c->o->flags & OMNIC_F_NO_SKIP)) {
        /* --no-skip：已压资产视同 unknown 入压缩桶（实验口径 zstd/19） */
        snprintf(c->plan[i].codec, sizeof(c->plan[i].codec), "zstd");
        c->plan[i].level = 19;
    } else if (arep.skipped || c->fs[i].size == 0) {
        /* SYS-CMP-003 skip 直通；0 字节文件按规格 §4.5 归 store */
        c->plan[i].store = 1;
    } else {
        snprintf(c->plan[i].codec, sizeof(c->plan[i].codec), "%s", arep.codec);
        c->plan[i].level = arep.level;
    }
    (void)omnic_fs_file_id(src, &c->ids[i].dev, &c->ids[i].ino); /* 取不到按未知：不当硬链接 */
    return 0;
}

ARC_THREAD_FN cls_worker(void *arg) {
    cls_ctx *c = (cls_ctx *)arg;
    for (;;) {
        size_t i0 = 0, cnt = 0;
        arc_mutex_lock(&c->lock);
        if (!c->stop) {
            i0 = c->next;
            cnt = (c->n - i0 > CLS_BATCH) ? CLS_BATCH : c->n - i0;
            c->next += cnt;
        }
        arc_mutex_unlock(&c->lock);
        if (cnt == 0) break;
        for (size_t k = 0; k < cnt; k++) {
            size_t i = i0 + k;
            if (c->ix->ent[i].type != ARC_T_FILE) continue; /* 目录、链接不分类 */
            if (cls_one(c, i) != 0) {
                arc_mutex_lock(&c->lock);
                if (c->first_err == OMNIC_OK) {
                    c->first_err = OMNIC_E_SYSTEM;
                    c->err_rel = c->fs[i].rel;
                    c->stop = 1;
                }
                arc_mutex_unlock(&c->lock);
                break;
            }
        }
    }
    ARC_THREAD_RET_OK;
}

/* ---- 并行块压缩池（DESIGN-parallel-pack-compress）：主线程构块派发，
 * worker 压缩，回执按派发序收——保序 = 块表/crc/offset 与串行一致，
 * 产物逐字节不变。作业结构 POD，blk 所有权随作业转移，回执带回 out 所有权。
 * 失败记首个错停派发，join 后统一报（g_err 非线程安全，worker 不碰）。 ---- */
#define CMP_PROBE_SAMPLE (4u << 20) /* 桶级候选采样：4MiB 前缀定全桶 codec */

typedef struct cmp_job {
    uint8_t *blk;
    uint64_t blk_len;
    const omnic_codec_ops *ops;
    int level;
    const omnic_codec_ops *alt; /* 双候选 B（best_ratio 首块），NULL=无 */
    size_t sample;              /* >0：采样探针（借用 blk 不转移，双压定夺不产块） */
    size_t seq;                 /* 派发序号：回执按此入槽，主线程按序消费保序 */
    struct cmp_job *next;
} cmp_job;

typedef struct cmp_result {
    uint8_t *out;
    size_t out_len;
    uint64_t raw;             /* 回执带回块长（入表 raw_size 用） */
    const omnic_codec_ops *win_ops;
    int win_level;
    omnic_status st;
    size_t seq_mark;          /* 写槽时的派发序号+1；0=未写（calloc 初始） */
    int decide;               /* 采样探针回执：只定 codec，无块产出 */
} cmp_result;

/* 每序号块的条目归属：构块时记录，flush 回填 block_id（入表序号此刻才定） */
typedef struct cmp_meta {
    const size_t *bidx; /* 桶内文件下标表（b->idx，buckets 存活期覆盖 flush） */
    size_t fi0, fi1;     /* 本块覆盖的桶内条目范围 [fi0, fi1) */
} cmp_meta;

typedef struct {
    cmp_job *head, *tail;     /* 待压作业 FIFO */
    cmp_result *res;           /* 回执环形缓冲（容量 workers+1） */
    cmp_meta *meta;            /* 每序号块的条目范围（flush 时回填 block_id） */
    size_t res_cap;
    int stop;
    arc_mutex_t lock;
    arc_cond_t job_cv;   /* worker 等作业 */
    arc_cond_t res_cv;   /* 主线程等回执 */
} cmp_ctx;

static void cmp_result_free(cmp_result *r) {
    free(r->out);
    r->out = NULL;
}

/* worker：弹作业→压缩→回执入环。无队列深度限制：主线程构块速度天然低于
 * 压缩速度，内存峰值 = 在途块数（受 workers 限制，见派发侧背压）。 */
ARC_THREAD_FN cmp_worker(void *arg) {
    cmp_ctx *c = (cmp_ctx *)arg;
    for (;;) {
        cmp_job *j = NULL;
        arc_mutex_lock(&c->lock);
        while (j == NULL && !c->stop) {
            if (c->head != NULL) {
                j = c->head;
                c->head = j->next;
                if (c->head == NULL) c->tail = NULL;
            } else {
                arc_cond_wait(&c->job_cv, &c->lock);
            }
        }
        arc_mutex_unlock(&c->lock);
        if (j == NULL) break; /* stop 且队列空 */

        /* T12：块内 codec 恒单线程（threads=0）——块级并行是全部并行度，
         * 由此 -T>1 与 -T1 产物逐字节相同（结构保证，非小块巧合）。 */
        cmp_result r;
        memset(&r, 0, sizeof(r));
        r.raw = j->blk_len;
        r.st = OMNIC_E_SYSTEM;
        if (j->sample > 0) {
            /* 采样探针（AR-PKG-003）：4MiB 前缀双压定全桶 codec，不产块。
             * blk 为借用（主线程等回执，生命周期安全），用毕不释放。 */
            uint8_t *oa = NULL, *ob = NULL;
            size_t la = 0, lb = 0;
            omnic_status sa = arc_compress_mem(j->blk, j->sample, j->ops, j->level,
                                               0, &oa, &la);
            omnic_status sb = sa == OMNIC_OK
                                  ? arc_compress_mem(j->blk, j->sample, j->alt, 9, 0, &ob, &lb)
                                  : sa;
            free(oa);
            free(ob);
            if (sa == OMNIC_OK && sb == OMNIC_OK) {
                if (lb < la) {
                    r.win_ops = j->alt;
                    r.win_level = 9;
                } else {
                    r.win_ops = j->ops;
                    r.win_level = j->level;
                }
                r.decide = 1;
                r.st = OMNIC_OK;
            } else {
                r.st = (sa != OMNIC_OK) ? sa : sb;
            }
        } else if (j->alt != NULL) {
            /* 双候选：A 全量试压，B 候选 xz/9 定级试压，小者胜（AR-PKG-003 平移） */
            uint8_t *oa = NULL, *ob = NULL;
            size_t la = 0, lb = 0;
            omnic_status sa = arc_compress_mem(j->blk, (size_t)j->blk_len, j->ops, j->level,
                                               0, &oa, &la);
            if (sa == OMNIC_OK) {
                omnic_status sb = arc_compress_mem(j->blk, (size_t)j->blk_len, j->alt, 9,
                                                   0, &ob, &lb);
                if (sb == OMNIC_OK) {
                    if (lb < la) {
                        free(oa);
                        r.out = ob; r.out_len = lb;
                        r.win_ops = j->alt; r.win_level = 9;
                    } else {
                        free(ob);
                        r.out = oa; r.out_len = la;
                        r.win_ops = j->ops; r.win_level = j->level;
                    }
                    r.st = OMNIC_OK;
                } else {
                    free(oa);
                    r.st = sb;
                }
            } else {
                r.st = sa;
            }
        } else {
            r.st = arc_compress_mem(j->blk, (size_t)j->blk_len, j->ops, j->level,
                                    0, &r.out, &r.out_len);
            if (r.st == OMNIC_OK) {
                r.win_ops = j->ops;
                r.win_level = j->level;
            }
        }

        arc_mutex_lock(&c->lock);
        size_t jseq = j->seq; /* free 前留取：入槽序号 */
        if (j->sample == 0) free(j->blk); /* 探针借用 blk，不释放 */
        free(j);
        /* 回执按派发序入槽（非完成序）：槽 = seq % cap，主线程按序消费。
         * mark=seq+1 是"本序号已写"的唯一凭证（完成乱序时 done 计数
         * 会假阳性）；同槽复用受 res_cap=workers+1 与窗口背压保护。 */
        r.seq_mark = jseq + 1;
        c->res[jseq % c->res_cap] = r;
        arc_cond_signal(&c->res_cv);
        arc_mutex_unlock(&c->lock);
    }
    ARC_THREAD_RET_OK;
}

/* 派发：入队并唤醒。seq 由调用方递增（= 派发序号）。 */
static int cmp_submit(cmp_ctx *c, cmp_job *j) {
    j->next = NULL;
    arc_mutex_lock(&c->lock);
    if (c->stop) { /* 失败后停收：释放所有权，主循环走失败路径 */
        arc_mutex_unlock(&c->lock);
        if (j->sample == 0) free(j->blk); /* 探针借用 blk 不释放，调用方善后 */
        free(j);
        return -1;
    }
    if (c->tail != NULL) c->tail->next = j;
    else c->head = j;
    c->tail = j;
    arc_cond_signal(&c->job_cv);
    arc_mutex_unlock(&c->lock);
    return 0;
}

/* 收据：等到 seq 序号回执（严格保序）。完成是乱序的——"完成计数>seq"
 * 不蕴含"seq 已写"（后序号先完成时假阳性），故对号入座：mark==seq+1
 * 才是本序号回执已落槽。失败回执也在其序号到位时报。 */
static cmp_result *cmp_reap(cmp_ctx *c, size_t seq) {
    for (;;) {
        arc_mutex_lock(&c->lock);
        if (c->res[seq % c->res_cap].seq_mark == seq + 1) {
            arc_mutex_unlock(&c->lock);
            return &c->res[seq % c->res_cap];
        }
        arc_cond_wait(&c->res_cv, &c->lock);
        arc_mutex_unlock(&c->lock);
    }
}

/* 收一笔回执并完成块收尾（入表/写出/打印）。ops/level 出参回写候选胜者
 * （非 probe 恒等；传 NULL 表示调用方不关心——常规窗口回收）。
 * 消费后置空槽 out 防双重释放。 */
static omnic_status cmp_flush_one(cmp_ctx *c, arc_index *ix, size_t *tbl_cap,
                                  FILE *f, uint64_t *arc_pos, uint64_t data_off,
                                  unsigned quiet, size_t *reaped,
                                  const omnic_codec_ops **ops, int *level) {
    size_t seq = *reaped;
    cmp_result *r = cmp_reap(c, seq);
    *reaped = seq + 1;
    if (r->st != OMNIC_OK) {
        omnic_set_error("block compress failed (%s)", omnic_status_str(r->st));
        cmp_result_free(r);
        return r->st;
    }
    if (r->decide) {
        /* 采样探针回执：胜者已由派发侧窥视取走（cmp_reap 不推进消费），
         * 此处按序空过即可，无块入表/写出、不动 ops/level（可能已是他桶） */
        return OMNIC_OK;
    }
    uint8_t *out = r->out;
    size_t out_len = r->out_len;
    uint64_t blk_len = r->raw;
    const omnic_codec_ops *wops = r->win_ops;
    int wlevel = r->win_level;
    r->out = NULL; /* 所有权移交 */
    if (ops != NULL) {
        *ops = wops;
        *level = wlevel;
    }
    /* 块号在此刻回填（入表时才确定）：构块时 ix.nblk 尚未推进到本块 */
    cmp_meta *m = &c->meta[seq % c->res_cap];
    for (size_t k = m->fi0; k < m->fi1; k++)
        ix->ent[m->bidx[k]].block_id = (uint32_t)(ix->nblk);
    if (arc_block_new(ix, tbl_cap, (uint8_t)wops->fmt, (uint8_t)wlevel,
                      *arc_pos - data_off) != 0) {
        free(out);
        omnic_set_error("out of memory");
        return OMNIC_E_SYSTEM;
    }
    arc_block *cb = &ix->blk[ix->nblk - 1];
    cb->comp_size = out_len;
    cb->raw_size = blk_len;
    cb->crc = omnic_crc32(0, out, out_len);
    if (fwrite(out, 1, out_len, f) != out_len) {
        free(out);
        omnic_set_error("write failed");
        return OMNIC_E_SYSTEM;
    }
    *arc_pos += out_len;
    free(out);
    if (!quiet)
        printf("  block: %s/%d  %llu -> %zu bytes (%.2f%%)\n", wops->name, wlevel,
               (unsigned long long)blk_len, out_len,
               blk_len ? 100.0 * (double)out_len / (double)blk_len : 0.0);
    return OMNIC_OK;
}

/* 停池：唤醒 worker 退出、join、清队列/回执残留。成功收尾与 fail 路径共用。
 * join 后无并发，队列/回执直取直放。 */
static void cmp_pool_shutdown(cmp_ctx *c, arc_thread_t *tid, int spawned) {
    arc_mutex_lock(&c->lock);
    c->stop = 1;
    arc_cond_broadcast(&c->job_cv);
    arc_mutex_unlock(&c->lock);
    for (int w = 0; w < spawned; w++) arc_thread_join(tid[w]);
    cmp_job *j = c->head;
    while (j != NULL) {
        cmp_job *nx = j->next;
        free(j->blk);
        free(j);
        j = nx;
    }
    for (size_t k = 0; k < c->res_cap; k++) cmp_result_free(&c->res[k]);
    free(c->res);
    c->res = NULL;
    free(c->meta);
    c->meta = NULL;
}

/* ---- pack（A3：store-only；A4 接决策分桶与压缩块）---- */

/* 未收录条目的上报：非 strict 逐类计数打到 stderr；strict 记为错误（CLI 退出前打印） */
static void report_skipped(const char *root, const omnic_fs_skip_report *sk, int strict) {
    if (strict) {
        omnic_set_error("%zu entries under %s cannot be archived (--strict): %zu special files or "
                        "unsupported links, %zu unreadable, %zu paths too long",
                        sk->total, root, sk->special, sk->unreadable, sk->too_long);
        return;
    }
    fprintf(stderr, "omnic: skipped %zu entries under %s (not in the archive): %zu special files or "
                    "unsupported links, %zu unreadable, %zu paths too long\n",
            sk->total, root, sk->special, sk->unreadable, sk->too_long);
    for (size_t i = 0; i < sk->n_first; i++)
        fprintf(stderr, "omnic:   skipped: %s%s\n", sk->first[i],
                (i + 1 == sk->n_first && sk->total > sk->n_first) ? " ..." : "");
}

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
    char tmp_path[OMNIC_FS_PATH_MAX];
    tmp_path[0] = '\0';
    arc_fid *ids = NULL; /* 文件身份表：分类后用于识别硬链接 */

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

    /* 归档先写同目录的临时文件，成功后才改名到 archive_path：失败的路上，输出
     * 路径上原有的归档（可能是上一份好备份）不会被截断 */
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", archive_path) >= (int)sizeof(tmp_path)) {
        omnic_set_error("archive path too long: %s", archive_path);
        return OMNIC_E_USAGE;
    }

    size_t n = 0;
    omnic_fs_skip_report sk;
    unsigned want = (o != NULL && (o->flags & OMNIC_F_CTIME) != 0) ? OMNIC_FS_WANT_CTIME : 0;
    omnic_fs_entry *fs = omnic_fs_walk_ex(root, &n, &sk, want);
    if (fs == NULL && n == 0) {
        omnic_set_error("fs walk failed: %s", root);
        return OMNIC_E_SYSTEM;
    }
    if (sk.total > 0) {
        int strict = (o != NULL && (o->flags & OMNIC_F_STRICT) != 0);
        if (strict || o == NULL || (o->flags & OMNIC_F_QUIET) == 0) report_skipped(root, &sk, strict);
        if (strict) {
            omnic_fs_entries_free(fs, n);
            return OMNIC_E_DATA;
        }
    }
    if (n > 0xFFFFFFFFull) { /* v2：条目计数 32 位；实践中先撞内存 */
        omnic_set_error("too many entries (%zu)", n);
        omnic_fs_entries_free(fs, n);
        return OMNIC_E_USAGE;
    }

    /* T12 块压缩池局部量：先于一切 goto fail 声明并置零（fail 标签要读） */
    cmp_ctx cmp;
    memset(&cmp, 0, sizeof(cmp));
    int cmp_on = 0;
    int cmp_spawned = 0;
    size_t cmp_seq = 0;    /* 已派发数 */
    size_t cmp_reaped = 0; /* 已收笔数（含写出） */
    arc_thread_t cmp_tid[ARC_NWRITER_MAX];

    arc_index ix;
    memset(&ix, 0, sizeof(ix));
    ix.ent = (arc_entry *)calloc(n ? n : 1, sizeof(arc_entry));
    if (ix.ent == NULL) {
        omnic_fs_entries_free(fs, n);
        return OMNIC_E_SYSTEM;
    }
    ix.nent = n;
    ix.version = OMNIC_ARC_VERSION;
    for (size_t i = 0; i < n; i++) {
        arc_entry *e = &ix.ent[i];
        e->name = arc_dup_name(fs[i].rel);
        if (e->name == NULL) {
            omnic_set_error("entry name not representable: %s", fs[i].rel);
            goto fail;
        }
        e->mtime = fs[i].mtime;
        e->block_id = ARC_NO_BLOCK; /* 有数据的条目落块时回填 */
        if (fs[i].link_kind != 0) {
            e->type = ARC_T_SYMLINK; /* 目标串当数据存进 store 桶 */
            if (fs[i].link_kind == OMNIC_FS_LINK_DIR_SYMLINK) e->flags |= ARC_EF_LINK_DIR;
            else if (fs[i].link_kind == OMNIC_FS_LINK_JUNCTION) e->flags |= ARC_EF_LINK_JUNCTION;
        } else if (fs[i].is_dir) {
            e->type = ARC_T_DIR;
        } else {
            e->type = ARC_T_FILE;
        }
        /* 可选字段：有才存——普通文件/目录的记录因此与 v1 等长 */
        if (fs[i].ctime != 0) {
            e->ctime = fs[i].ctime;
            e->flags |= ARC_EF_HAS_CTIME;
        }
        if (fs[i].has_mode && fs[i].link_kind == 0) {
            e->mode = fs[i].mode;
            e->flags |= ARC_EF_HAS_MODE;
        }
        if (fs[i].attrs != 0) {
            e->attrs = fs[i].attrs;
            e->flags |= ARC_EF_HAS_ATTRS;
        }
    }

    f = omnic_fs_fopen(tmp_path, "wb");
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
                    f = omnic_fs_fopen(tmp_path, "wb");
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

    /* ---- 决策 pass：逐文件分类建议。explain/小树/-T 1 走串行；默认 8 worker
     * 并行（分类是 pack 大头：游戏树实测 3.82s/5.26s）。串行与并行语义一致，
     * 区别仅 in_bytes 累加点（并行在 join 后按同一口径补算）。 ---- */
    plan = (file_plan *)calloc(n ? n : 1, sizeof(file_plan));
    if (plan == NULL) goto oom;
    ids = (arc_fid *)calloc(n ? n : 1, sizeof(arc_fid));
    if (ids == NULL) goto oom;
    for (size_t i = 0; i < n; i++) ids[i].idx = i;
    size_t n_files = 0;
    for (size_t i = 0; i < n; i++)
        if (ix.ent[i].type == 0) n_files++;
    int cls_workers = ARC_NWRITER_DEFAULT;
    if (o != NULL && o->threads == 1) cls_workers = 1;
    else if (o != NULL && o->threads > 1) cls_workers = o->threads;
    if (cls_workers > ARC_NWRITER_MAX) cls_workers = ARC_NWRITER_MAX;
    if (n_files > 0 && (size_t)cls_workers > n_files) cls_workers = (int)n_files;
    int cls_parallel = (!explain && cls_workers > 1 && n_files >= ARC_PAR_MIN_JOBS);
    if (cls_parallel) {
        cls_ctx c;
        memset(&c, 0, sizeof(c));
        c.fs = fs;
        c.ix = &ix;
        c.root = root;
        c.o = o;
        c.plan = plan;
        c.ids = ids;
        c.n = n;
        c.first_err = OMNIC_OK;
        arc_mutex_init(&c.lock);
        arc_thread_t tid[ARC_NWRITER_MAX];
        int spawned = 0;
        for (; spawned < cls_workers; spawned++)
            if (arc_thread_create(&tid[spawned], cls_worker, &c) != 0) break;
        if (spawned == 0) {
            cls_parallel = 0; /* 线程创建全败：串行兜底 */
        } else {
            for (int w = 0; w < spawned; w++) arc_thread_join(tid[w]);
            if (c.first_err != OMNIC_OK) {
                omnic_set_error("cannot analyze '%s/%s'", root, c.err_rel);
                goto fail;
            }
            for (size_t i = 0; i < n; i++)
                if (ix.ent[i].type == 0) rep->in_bytes += fs[i].size;
        }
    }
    if (!cls_parallel) {
        for (size_t i = 0; i < n; i++) {
        if (ix.ent[i].type != ARC_T_FILE) continue; /* 目录、链接不分类 */
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
        (void)omnic_fs_file_id(src, &ids[i].dev, &ids[i].ino); /* 取不到按未知：不当硬链接 */
        if (explain)
            printf("  %s: %s -> %s\n", fs[i].rel, omnic_class_str(arep.klass),
                   plan[i].store ? "pass-through" : plan[i].codec);
        rep->in_bytes += fs[i].size;
        }
    }
    mark_hardlinks(&ix, ids, n);
    free(ids);
    ids = NULL;
    if (!quiet)
        printf("  classified %llu entries in %.2fs\n", (unsigned long long)n,
                omnic_now_sec() - t0);

    /* ---- 分桶（D10：同 (codec,level) 聚合；per-file 模式每文件独立桶） ---- */
    for (size_t i = 0; i < n; i++) {
        if (ix.ent[i].type != ARC_T_FILE && ix.ent[i].type != ARC_T_SYMLINK) continue;
        arc_bucket *t = NULL;
        if (plan[i].store || ix.ent[i].type == ARC_T_SYMLINK) { /* 链接的目标串原样进 store 桶 */
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

    /* ---- 并行块压缩池（T12）：threads 语义与分类池一致（0=默认8、1=串行）。
     * 构块留主线程，压缩重叠进行；回执按派发序收——保序 = 块表/产物与串行一致。
     * 局部量已在函数头声明置零（fail 标签先于本块可达）。 ---- */
    int cmp_workers = ARC_NWRITER_DEFAULT;
    if (threads == 1) cmp_workers = 1;
    else if (threads > 1) cmp_workers = threads;
    if (cmp_workers > ARC_NWRITER_MAX) cmp_workers = ARC_NWRITER_MAX;
    cmp_on = (cmp_workers > 1 && !explain);
    if (cmp_on) {
        cmp.res_cap = (size_t)cmp_workers + 1;
        cmp.res = (cmp_result *)calloc(cmp.res_cap, sizeof(cmp_result));
        cmp.meta = (cmp_meta *)calloc(cmp.res_cap, sizeof(cmp_meta));
        if (cmp.res == NULL || cmp.meta == NULL) {
            free(cmp.res);
            free(cmp.meta);
            cmp.res = NULL;
            cmp.meta = NULL;
            cmp_on = 0; /* OOM 退回串行，正确性不受影响 */
        } else {
            arc_mutex_init(&cmp.lock);
            arc_cond_init(&cmp.job_cv);
            arc_cond_init(&cmp.res_cv);
            (void)omnic_crc32(0, "", 0); /* 预热分表：worker 可能先于主线程首调 */
            for (; cmp_spawned < cmp_workers; cmp_spawned++)
                if (arc_thread_create(&cmp_tid[cmp_spawned], cmp_worker, &cmp) != 0) break;
            if (cmp_spawned == 0) {
                free(cmp.res);
                free(cmp.meta);
                cmp.res = NULL;
                cmp.meta = NULL;
                cmp_on = 0; /* 线程创建全败：串行兜底 */
            }
        }
    }

    /* ---- 桶写出 ---- */
    for (size_t bi = 0; bi < nb; bi++) {
        arc_bucket *b = &bkts[bi];
        if (b->is_store) {
            /* store 直写文件/直入表：先排空在途压缩块，锁住表序/写序与串行
             * 一致（压缩桶之间靠 seq 保序无需排空，跨桶重叠保留到最后） */
            if (cmp_on) {
                while (cmp_reaped < cmp_seq) {
                    if (cmp_flush_one(&cmp, &ix, &tbl_cap, f, &arc_pos, data_off, quiet,
                                      &cmp_reaped, NULL, NULL) != OMNIC_OK)
                        goto fail;
                }
            }
            /* store：流式块（A3 路径），双 CRC 边写边算 */
            uint32_t blk_crc = 0;
            int blk_open = 0;
            for (size_t fi = 0; fi < b->n; fi++) {
                size_t i = b->idx[fi];
                const char *mem = (ix.ent[i].type == ARC_T_SYMLINK) ? fs[i].link_target : NULL;
                char src[OMNIC_FS_PATH_MAX * 2];
                FILE *in = NULL;
                if (mem == NULL) { /* 符号链接的"内容"是目标串（内存）；普通文件读盘 */
                    if (snprintf(src, sizeof(src), "%s/%s", root, fs[i].rel) >= (int)sizeof(src)) {
                        omnic_set_error("path too long: %s", fs[i].rel);
                        goto fail;
                    }
                    in = omnic_fs_fopen(src, "rb");
                    if (in == NULL) {
                        omnic_set_error("cannot read source file: %s", src);
                        goto fail;
                    }
                }
                uint64_t cur = blk_open ? ix.blk[ix.nblk - 1].raw_size : 0;
                if (!blk_open || (cur > 0 && (cur + fs[i].size > block_max ||
                                              fs[i].size > block_max))) {
                    if (blk_open) ix.blk[ix.nblk - 1].crc = blk_crc;
                    if (arc_block_new(&ix, &tbl_cap, 0, 0, arc_pos - data_off) != 0) {
                        if (in != NULL) fclose(in);
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
                size_t done = 0;
                /* 按遍历时的长度写：文件此后变短就中止——短写会让同块后续条目的偏移全错 */
                while (done < fs[i].size) {
                    size_t r;
                    if (mem != NULL) {
                        r = (size_t)(fs[i].size - done);
                        if (r > sizeof(buf)) r = sizeof(buf);
                        memcpy(buf, mem + done, r);
                    } else {
                        r = fread(buf, 1, sizeof(buf), in);
                        if (r == 0) break;
                    }
                    if (fwrite(buf, 1, r, f) != r) {
                        if (in != NULL) fclose(in);
                        goto io_fail;
                    }
                    arc_pos += r;
                    cb->raw_size += r;
                    cb->comp_size = cb->raw_size;
                    file_crc = omnic_crc32(file_crc, buf, r);
                    blk_crc = omnic_crc32(blk_crc, buf, r);
                    done += r;
                }
                if (in != NULL) {
                    int rd_err = ferror(in);
                    int short_read = (done != fs[i].size);
                    fclose(in);
                    if (rd_err) {
                        omnic_set_error("read failed: %s", src);
                        goto fail;
                    }
                    if (short_read) {
                        omnic_set_error("source file changed size while packing: %s", src);
                        goto fail;
                    }
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
            size_t fi0 = fi; /* 池路径：本块条目范围起点（flush 回填 block_id 用） */
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
                    file_crc = omnic_crc32(file_crc, blk + blk_len, r);
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
            /* 压缩（首块且桶 ≥4MiB 时双候选全量试压，AR-PKG-003）。
             * 池路径：构块即派发，窗口满（在途>workers）收最旧回执写出——
             * 压缩与后续构块重叠；桶级候选首块同步收（胜者定全桶 ops/level，
             * 与串行语义一致）。串行路径逐字保留（-T1/explain/兜底）。 */
            if (cmp_on) {
                /* 背压（任何提交之前）：在途压回 ≤ spawned。meta 环槽在提交时
                 * 写入——只有先消费才能保证槽复用不踩未消费项（res 环由
                 * worker 的 mark 写入天然晚于此约束，一并被覆盖保护）。 */
                while (cmp_seq - cmp_reaped > (size_t)cmp_spawned) {
                    if (cmp_flush_one(&cmp, &ix, &tbl_cap, f, &arc_pos, data_off, quiet,
                                      &cmp_reaped, NULL, NULL) != OMNIC_OK)
                        goto fail;
                }
                /* 采样探针（桶级候选首块）：4MiB 前缀双压定全桶 codec。回执用
                 * 窥视（等标记不推进消费序号）——胜者尽早到手，后续块尽早
                 * 派发，不被前序慢块的按序消费卡住；槽由后续 flush 按序空过。
                 * blk 为借用，主线程窥视等待期生命周期安全。 */
                if (first && alt != NULL) {
                    size_t sn = (size_t)blk_len < CMP_PROBE_SAMPLE ? (size_t)blk_len
                                                                   : CMP_PROBE_SAMPLE;
                    cmp_job *pj = (cmp_job *)malloc(sizeof(cmp_job));
                    if (pj == NULL) {
                        free(blk);
                        goto oom;
                    }
                    pj->blk = blk;
                    pj->blk_len = blk_len;
                    pj->sample = sn;
                    pj->ops = ops;
                    pj->level = level;
                    pj->alt = alt;
                    pj->seq = cmp_seq++;
                    if (cmp_submit(&cmp, pj) != 0) {
                        free(blk); /* 探针借用被拒：本方善后 */
                        goto fail;
                    }
                    cmp_result *pr = cmp_reap(&cmp, pj->seq); /* 窥视：不消费 */
                    if (pr->st != OMNIC_OK) {
                        omnic_set_error("probe compress failed (%s)",
                                        omnic_status_str(pr->st));
                        goto fail;
                    }
                    ops = pr->win_ops; /* 胜者定全桶，首块随即按此派发 */
                    level = pr->win_level;
                    /* 探针占了一个在途名额：实块提交前再压一次窗口 */
                    while (cmp_seq - cmp_reaped > (size_t)cmp_spawned) {
                        if (cmp_flush_one(&cmp, &ix, &tbl_cap, f, &arc_pos, data_off, quiet,
                                          &cmp_reaped, NULL, NULL) != OMNIC_OK)
                            goto fail;
                    }
                }
                cmp_job *j = (cmp_job *)malloc(sizeof(cmp_job));
                if (j == NULL) {
                    free(blk);
                    goto oom;
                }
                j->blk = blk;
                j->blk_len = blk_len;
                j->sample = 0;
                j->ops = ops;
                j->level = level;
                j->alt = NULL; /* 桶 codec 已由探针定夺 */
                j->seq = cmp_seq++;
                cmp.meta[j->seq % cmp.res_cap] = (cmp_meta){b->idx, fi0, fi};
                blk = NULL; /* 所有权随作业转移 */
                first = 0;
                if (cmp_submit(&cmp, j) != 0) goto fail; /* 池停：前序失败 */
                continue; /* 本块入表/写出已随回执完成 */
            }
            /* 串行路径：桶级候选同款采样探针（与池路径同一决策 = 同一产物） */
            if (first && alt != NULL) {
                size_t sn = (size_t)blk_len < CMP_PROBE_SAMPLE ? (size_t)blk_len
                                                               : CMP_PROBE_SAMPLE;
                uint8_t *oa = NULL, *ob = NULL;
                size_t la = 0, lb = 0;
                omnic_status sa = arc_compress_mem(blk, sn, ops, level, threads, &oa, &la);
                omnic_status sb = sa == OMNIC_OK
                                      ? arc_compress_mem(blk, sn, alt, 9, threads, &ob, &lb)
                                      : sa;
                free(oa);
                free(ob);
                if (sa != OMNIC_OK || sb != OMNIC_OK) {
                    free(blk);
                    goto fail;
                }
                if (lb < la) {
                    ops = alt;
                    level = 9;
                }
            }
            uint8_t *out = NULL;
            size_t out_len = 0;
            {
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
            cb->crc = omnic_crc32(0, out, out_len);
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
    /* 收尾排空：跨桶在途块按序写出（桶间无 probe 依赖，重叠到最后） */
    if (cmp_on) {
        while (cmp_reaped < cmp_seq) {
            if (cmp_flush_one(&cmp, &ix, &tbl_cap, f, &arc_pos, data_off, quiet,
                              &cmp_reaped, NULL, NULL) != OMNIC_OK)
                goto fail;
        }
    }
    /* 池停机：回执已清，停 worker 后索引/Footer 才安全落笔 */
    if (cmp_on) {
        cmp_pool_shutdown(&cmp, cmp_tid, cmp_spawned);
        cmp_on = 0;
    }
    buckets_free(bkts, nb);
    bkts = NULL;
    nb = 0;
    free(plan);
    plan = NULL;
    rep->seconds = omnic_now_sec() - t0;

    /* 索引（zstd）+ Footer；n_blocks 回填 Header */
    bytebuf plain = {NULL, 0, 0};
    if (arc_index_serialize(ix.blk, ix.nblk, ix.ent, ix.nent, &plain) != 0) goto oom;
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
    uint32_t index_crc = omnic_crc32(0, zbuf, (size_t)zn);
    free(zbuf);

    uint32_t nb32 = (uint32_t)ix.nblk;
    uint8_t nbrow[4];
    put32(nbrow, nb32);
    if (arc_seek(f, 12) != 0 || fwrite(nbrow, 1, 4, f) != 4) goto io_fail;
    if (arc_seek(f, index_off + zn) != 0) goto io_fail; /* 回填后回到尾部写 Footer */

    uint8_t ftr[ARC_FTR];
    memset(ftr, 0, sizeof(ftr)); /* 尾部的计数字节在 v2 保留为 0：不能留栈上的旧值 */
    memcpy(ftr, ARC_MAGIC_F, 8);
    put64(ftr + 8, index_off);
    put64(ftr + 16, zn);
    put32(ftr + 24, index_crc);
    /* v2：尾部的 4 个计数字节保留为 0——计数以头部的 32 位为准（v1 在那里只有 16 位） */
    if (fwrite(ftr, 1, sizeof(ftr), f) != sizeof(ftr)) goto io_fail;

    rep->out_bytes = index_off + zn + ARC_FTR;
    rep->ratio = rep->in_bytes ? (double)rep->out_bytes / (double)rep->in_bytes : 0.0;
    arc_index_mem_free(&ix);
    omnic_fs_entries_free(fs, n);
    fs = NULL; /* 之后的 goto fail 还会再走一遍清理 */
    if (fclose(f) != 0) {
        f = NULL;
        omnic_set_error("write failed on archive: %s", archive_path);
        goto fail;
    }
    f = NULL;
    /* 发布：同目录改名是原子的，输出路径上只可能是旧归档或完整的新归档 */
    if (omnic_fs_replace(tmp_path, archive_path) != OMNIC_OK) {
        omnic_set_error("cannot replace archive (open in another process?): %s", archive_path);
        goto fail;
    }
    return OMNIC_OK;

io_fail:
    omnic_set_error("write failed on archive: %s", archive_path);
    goto fail;
oom:
    omnic_set_error("out of memory");
fail:
    if (cmp_on) cmp_pool_shutdown(&cmp, cmp_tid, cmp_spawned);
    free(plan);
    free(ids);
    buckets_free(bkts, nb);
    arc_index_mem_free(&ix);
    omnic_fs_entries_free(fs, n);
    if (f != NULL) fclose(f);
    if (tmp_path[0] != '\0') omnic_fs_remove(tmp_path);
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
    unsigned version = get16(hdr + 8);
    if (version < OMNIC_ARC_VERSION_MIN || version > OMNIC_ARC_VERSION) {
        omnic_set_error(version > OMNIC_ARC_VERSION
                            ? "archive needs a newer omnic (format version %u)"
                            : "unsupported .omnic version %u",
                        version);
        fclose(f);
        return OMNIC_E_DATA;
    }
    unsigned hflags = get16(hdr + 10);
    /* 未知标志位明确拒绝：否则按旧布局去读带新字段的索引只会读出错数据。
     * v2 的记录恒带 mtime，所以头部标志必须是那两位本身；v1 的 bit0 表示有无 mtime。 */
    if ((hflags & ~(OMNIC_ARC_F_MTIME | OMNIC_ARC_F_INDEX_ZSTD)) != 0 ||
        (hflags & OMNIC_ARC_F_INDEX_ZSTD) == 0 ||
        (version == OMNIC_ARC_VERSION && hflags != (OMNIC_ARC_F_MTIME | OMNIC_ARC_F_INDEX_ZSTD))) {
        omnic_set_error("unsupported archive flags 0x%x", hflags);
        fclose(f);
        return OMNIC_E_DATA;
    }
    /* 读取端按固定的数据区起点算块偏移：别处的值不能默默放过 */
    if (get32(hdr + 20) != ARC_HDR) {
        omnic_set_error("unsupported data offset %u", get32(hdr + 20));
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
    /* 计数以 32 位的头为准（v2 起）；v1 另与尾部的 16 位计数交叉核对 */
    uint32_t nent = get32(hdr + 16), nblk = get32(hdr + 12);
    if (version == 1 && (get16(ftr + 28) != nent || get16(ftr + 30) != nblk)) {
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
    if (omnic_crc32(0, zbuf, (size_t)index_comp) != index_crc) {
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
    /* v2 的记录恒带 mtime；v1 才看头部标志位 */
    omnic_status pst = arc_index_parse(plain, plain_n, nent, nblk, version,
                                        version == 1 ? ((hflags & OMNIC_ARC_F_MTIME) != 0) : 1, ix);
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
        if ((ix->ent[i].type == ARC_T_FILE || ix->ent[i].type == ARC_T_SYMLINK) &&
            ix->ent[i].block_id == (uint32_t)bi) {
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
#define ARC_SLOT_RING 4     /* 在飞块上限 = 背压；内存上界 ≈ 4 × block_max */

/* 落盘一个条目（串行/并行共用）。dst 为完整目标路径；e 的 mtime/ctime/权限位/属性
 * 与数据一并恢复。force：-f，既存的只读文件先删后建。
 * 返回 0=成功 1=无法创建 2=写入不完整。 */
static int write_entry(const char *dst, const uint8_t *data, const arc_entry *e, int force) {
    omnic_fs_meta m;
    m.mtime = e->mtime;
    m.ctime = e->ctime;
    m.mode = e->mode;
    m.attrs = e->attrs;
    int opened = 0;
    omnic_status st = omnic_fs_write_file_meta(dst, data, (size_t)e->size, &m, force, &opened);
    if (st != OMNIC_OK && opened == 0) {
        /* 兜底：目录条目序外缺失（畸形归档）才补建父目录——正常路径目录先行
         * 已建全，逐文件 mkdir_p 的 syscall 开销占解压墙钟大头 */
        char *slash = strrchr(dst, '/');
        if (slash != NULL) {
            *slash = '\0';
            omnic_status ms = omnic_fs_mkdir_p(dst);
            *slash = '/';
            if (ms == OMNIC_OK)
                st = omnic_fs_write_file_meta(dst, data, (size_t)e->size, &m, force, &opened);
        }
    }
    if (st == OMNIC_OK) return 0;
    return opened ? 2 : 1;
}

/* 条目落盘后的权限收尾（目录条目在文件全部写完后调用） */
static omnic_status set_entry_meta(const char *dst, const arc_entry *e) {
    omnic_fs_meta m;
    m.mtime = e->mtime;
    m.ctime = e->ctime;
    m.mode = e->mode;
    m.attrs = e->attrs;
    return omnic_fs_set_meta(dst, &m);
}

/* 解包侧的条目有数据：文件，以及目标串存在数据块里的符号链接 */
static int ent_has_data(const arc_entry *e) {
    return e->type == ARC_T_FILE || e->type == ARC_T_SYMLINK;
}

/* 解码单块为裸字节（store 直读 / 压缩流解压，块层 CRC 先行）。
 * 返回 0=成功 1=数据损坏（坏块口径）2=无解码器 3=OOM。 */
static int decode_block(FILE *f, const arc_block *b, uint8_t **raw) {
    if (b->fmt == 0) { /* store：直读（+1 防 raw_size==0 的 malloc(0)） */
        uint8_t *m = (uint8_t *)malloc((size_t)b->raw_size + 1);
        if (m == NULL) return 3;
        if (arc_seek(f, ARC_HDR + b->offset) != 0 ||
            fread(m, 1, (size_t)b->raw_size, f) != (size_t)b->raw_size ||
            omnic_crc32(0, m, (size_t)b->raw_size) != b->crc) {
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
        omnic_crc32(0, cbuf, (size_t)b->comp_size) != b->crc) {
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

/* ---- 并行解码池（B/T1-parallel-decode）：verify 与 unpack 的解码段 worker 化。
 * 模式平移自 T12 压缩池——主线程按块序派发，worker 独立句柄读归档+解压，
 * 回执按派发序（seq_mark 对号）保序收割：块序/坏块语义与串行完全一致。
 * 写盘池照旧（两层流水：解码 worker 产出 → 写盘 worker 落盘）。
 * 背压：在途 ≤ workers，回执环 cap=workers+1。 ---- */
typedef struct dec_job {
    const arc_block *b;   /* 指向 ix.blk（索引存活期覆盖整个池生命周期） */
    size_t seq;
    struct dec_job *next;
} dec_job;

typedef struct {
    uint8_t *raw;        /* 解码产物（drc==0 时有效，所有权交主线程） */
    int drc;             /* decode_block 的返回码 */
    size_t seq_mark;     /* 写槽序号+1；0=未写 */
} dec_result;

typedef struct {
    const char *path;    /* 归档路径：worker 各自 fopen 独立句柄 */
    dec_job *head, *tail;
    dec_result *res;     /* 回执环（cap=workers+1） */
    size_t res_cap;
    int stop;
    arc_mutex_t lock;
    arc_cond_t job_cv;
    arc_cond_t res_cv;
} dec_ctx;

static void dec_result_free(dec_result *r) {
    free(r->raw);
    r->raw = NULL;
}

ARC_THREAD_FN dec_worker(void *arg) {
    dec_ctx *c = (dec_ctx *)arg;
    FILE *f = omnic_fs_fopen(c->path, "rb"); /* 独立句柄：OS 页缓存共享 */
    for (;;) {
        dec_job *j = NULL;
        arc_mutex_lock(&c->lock);
        while (j == NULL && !c->stop) {
            if (c->head != NULL) {
                j = c->head;
                c->head = j->next;
                if (c->head == NULL) c->tail = NULL;
            } else {
                arc_cond_wait(&c->job_cv, &c->lock);
            }
        }
        arc_mutex_unlock(&c->lock);
        if (j == NULL) break; /* stop 且队列空 */

        dec_result r;
        memset(&r, 0, sizeof(r));
        if (f != NULL) {
            r.drc = decode_block(f, j->b, &r.raw);
        } else {
            r.drc = 3; /* 句柄耗尽按 OOM 口径：主线程统一失败处理 */
        }

        arc_mutex_lock(&c->lock);
        size_t jseq = j->seq;
        free(j);
        r.seq_mark = jseq + 1;
        c->res[jseq % c->res_cap] = r;
        arc_cond_signal(&c->res_cv);
        arc_mutex_unlock(&c->lock);
    }
    if (f != NULL) fclose(f);
    ARC_THREAD_RET_OK;
}

/* 派发：入队并唤醒（池停时释放作业，主循环走失败路径） */
static int dec_submit(dec_ctx *c, dec_job *j) {
    j->next = NULL;
    arc_mutex_lock(&c->lock);
    if (c->stop) {
        arc_mutex_unlock(&c->lock);
        free(j);
        return -1;
    }
    if (c->tail != NULL) c->tail->next = j;
    else c->head = j;
    c->tail = j;
    arc_cond_signal(&c->job_cv);
    arc_mutex_unlock(&c->lock);
    return 0;
}

/* 保序收割：等到 seq 序号回执。返回槽指针（只读消费，raw 所有权经出参转移） */
static dec_result *dec_reap(dec_ctx *c, size_t seq) {
    for (;;) {
        arc_mutex_lock(&c->lock);
        if (c->res[seq % c->res_cap].seq_mark == seq + 1) {
            arc_mutex_unlock(&c->lock);
            return &c->res[seq % c->res_cap];
        }
        arc_cond_wait(&c->res_cv, &c->lock);
        arc_mutex_unlock(&c->lock);
    }
}

/* 停机：唤醒 worker 退出、join、清队列/回执残留 */
static void dec_pool_shutdown(dec_ctx *c, arc_thread_t *tid, int spawned) {
    arc_mutex_lock(&c->lock);
    c->stop = 1;
    arc_cond_broadcast(&c->job_cv);
    arc_mutex_unlock(&c->lock);
    for (int w = 0; w < spawned; w++) arc_thread_join(tid[w]);
    dec_job *j = c->head;
    while (j != NULL) {
        dec_job *nx = j->next;
        free(j);
        j = nx;
    }
    for (size_t k = 0; k < c->res_cap; k++) dec_result_free(&c->res[k]);
    free(c->res);
    c->res = NULL;
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
    int force;           /* -f：既存的只读文件先删后建（只读，worker 可见） */
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
            rc = write_entry(dst, p->slot[j->slot].buf + e->in_off, e, p->force);
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
static int pool_init(arc_pool *p, const arc_index *ix, const char *dstroot, int writers,
                     int force) {
    memset(p, 0, sizeof(*p));
    p->ix = ix;
    p->dstroot = dstroot;
    p->force = force;
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

/* --file 模式匹配三种：尾 '/' 目录前缀、含 * / ? 走 glob、其余精确路径。
 * glob 极简实现（无转义，* 跨 /，大小写敏感——条目名在归档内原样保留）。 */
static int glob_match(const char *pat, const char *s) {
    const char *star = NULL, *ss = NULL;
    while (*s != '\0') {
        if (*pat == *s || *pat == '?') {
            pat++;
            s++;
        } else if (*pat == '*') {
            star = pat++;
            ss = s;
        } else if (star != NULL) {
            pat = star + 1;
            s = ++ss;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

static int name_matches(const char *pat, size_t pat_len, const char *name) {
    if (pat_len > 0 && pat[pat_len - 1] == '/') {
        size_t pl = pat_len - 1;
        return strncmp(name, pat, pl) == 0 && (name[pl] == '/' || name[pl] == '\0');
    }
    int has_glob = 0;
    for (size_t i = 0; i < pat_len; i++) {
        if (pat[i] == '*' || pat[i] == '?') {
            has_glob = 1;
            break;
        }
    }
    if (has_glob) return glob_match(pat, name);
    return strcmp(pat, name) == 0;
}

/* ---- 链接类条目与目录权限的收尾（文件全部落盘之后） ----
 * - 硬链接：全量提取时真建硬链接；--file 单选时目标条目未必在场，就地物化成一份副本。
 * - 符号链接最后建：任何文件都不会经由刚建的链接写到输出目录外面去。
 * - 目录的权限位/属性最后设、先子后父：只读的目录不能挡住往里写文件、改子目录。 */
typedef struct {
    size_t *ent;   /* 条目下标，按索引序 */
    char **target; /* 目标串（malloc） */
    size_t n, cap;
} arc_linkq;

static void linkq_free(arc_linkq *q) {
    for (size_t k = 0; k < q->n; k++) free(q->target[k]);
    free(q->target);
    free(q->ent);
    memset(q, 0, sizeof(*q));
}

/* 符号链接的目标先攒着（块缓冲会被回收）；返回 0=入队 */
static int linkq_push(arc_linkq *q, size_t i, const arc_entry *e, const uint8_t *raw) {
    if (q->n == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 16;
        size_t *ne = (size_t *)realloc(q->ent, cap * sizeof(*ne));
        if (ne == NULL) return -1;
        q->ent = ne;
        char **nt = (char **)realloc(q->target, cap * sizeof(*nt));
        if (nt == NULL) return -1;
        q->target = nt;
        q->cap = cap;
    }
    char *t = (char *)malloc((size_t)e->size + 1);
    if (t == NULL) return -1;
    memcpy(t, raw + e->in_off, (size_t)e->size);
    t[e->size] = '\0';
    q->ent[q->n] = i;
    q->target[q->n] = t;
    q->n++;
    return 0;
}

static char *join_dst(const char *root, const char *name) {
    size_t dl = strlen(root) + strlen(name) + 2;
    char *dst = (char *)malloc(dl);
    if (dst != NULL) sprintf(dst, "%s/%s", root, name);
    return dst;
}

static int target_is_absolute(const char *t) {
    return t[0] == '/' || t[0] == '\\' ||
           (((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z')) && t[1] == ':');
}

static void finish_links(const arc_index *ix, const char *dstroot, const unsigned char *want_ent,
                         int force, arc_linkq *links, FILE *f, size_t *extracted, size_t *failed,
                         unsigned quiet) {
    int selective = (want_ent != NULL);
    int err_set = 0; /* 报错只记第一条（g_err 只有一个槽） */
    /* 目录权限：倒序即先子后父（遍历是先父后子） */
    for (size_t i = ix->nent; i-- > 0;) {
        const arc_entry *e = &ix->ent[i];
        if (e->type != ARC_T_DIR || selective || (e->mode == 0 && e->attrs == 0)) continue;
        char *dst = join_dst(dstroot, e->name);
        if (dst == NULL) {
            (*failed)++;
            continue;
        }
        if (set_entry_meta(dst, e) != OMNIC_OK) {
            (*failed)++;
            if (!err_set) {
                omnic_set_error("cannot set permissions on directory: %s", dst);
                err_set = 1;
            }
        }
        free(dst);
    }
    /* 硬链接：目标文件此刻已在盘上 */
    for (size_t i = 0; i < ix->nent; i++) {
        const arc_entry *e = &ix->ent[i];
        if (e->type != ARC_T_HARDLINK || (selective && !want_ent[i])) continue;
        const arc_entry *own = &ix->ent[e->link_to];
        char *dst = join_dst(dstroot, e->name);
        if (dst == NULL) {
            (*failed)++;
            continue;
        }
        int rc = 1;
        if (!selective) {
            char *odst = join_dst(dstroot, own->name);
            if (odst != NULL) {
                omnic_fs_remove_force(dst); /* 全量提取与文件同语义：覆盖 */
                rc = (omnic_fs_make_hardlink(odst, dst) == OMNIC_OK) ? 0 : 1;
                free(odst);
            }
        } else {
            uint8_t *raw = NULL;
            if (decode_block(f, &ix->blk[own->block_id], &raw) == 0) {
                /* 数据取自目标条目（大小/偏移/校验），元数据用本条目自己的 */
                arc_entry tmp = *own;
                tmp.mtime = e->mtime;
                tmp.ctime = e->ctime;
                tmp.mode = e->mode;
                tmp.attrs = e->attrs;
                rc = write_entry(dst, raw + own->in_off, &tmp, force);
                free(raw);
            }
        }
        if (rc == 0) {
            (*extracted)++;
        } else {
            (*failed)++;
            if (!err_set) {
                omnic_set_error("cannot create hard link: %s", dst);
                err_set = 1;
            }
        }
        free(dst);
    }
    /* 符号链接最后建 */
    size_t as_junction = 0;
    for (size_t k = 0; k < links->n; k++) {
        const arc_entry *e = &ix->ent[links->ent[k]];
        char *dst = join_dst(dstroot, e->name);
        if (dst == NULL) {
            (*failed)++;
            continue;
        }
        int rc = 1, unprivileged = 0;
        if (selective && !force && arc_file_exists(dst)) {
            omnic_set_error("'%s' already exists — use -f to overwrite", dst);
            err_set = 1;
        } else {
            omnic_fs_remove_force(dst); /* 全量提取与文件同语义：覆盖（含既存的目录链接） */
            const char *t = links->target[k];
            int is_dir = (e->flags & ARC_EF_LINK_DIR) != 0;
            if ((e->flags & ARC_EF_LINK_JUNCTION) != 0) {
                if (target_is_absolute(t)) /* 联接的目标必须是绝对路径 */
                    rc = (omnic_fs_make_junction(dst, t) == OMNIC_OK) ? 0 : 1;
            } else {
                rc = (omnic_fs_make_symlink(dst, t, is_dir, &unprivileged) == OMNIC_OK) ? 0 : 1;
                if (rc != 0 && unprivileged && is_dir && target_is_absolute(t) &&
                    omnic_fs_make_junction(dst, t) == OMNIC_OK) {
                    rc = 0; /* 没有建符号链接的权限：目标为绝对路径的目录链接退而成目录联接 */
                    as_junction++;
                }
            }
        }
        if (rc == 0) {
            (*extracted)++;
        } else {
            (*failed)++;
            if (!err_set) {
                if (unprivileged)
                    omnic_set_error("cannot create symbolic link (Windows: enable Developer Mode "
                                    "or run as administrator): %s",
                                    dst);
                else
                    omnic_set_error("cannot create symbolic link: %s", dst);
                err_set = 1;
            }
        }
        free(dst);
    }
    if (!quiet && as_junction > 0)
        fprintf(stderr, "omnic: %zu directory symlinks created as junctions (no symlink privilege)\n",
                as_junction);
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

    /* 解码池（B）状态：置于所有 goto sys_fail_ix 之前，异常路径停机安全 */
    dec_ctx dec;
    memset(&dec, 0, sizeof(dec));
    arc_thread_t dec_tid[ARC_NWRITER_MAX];
    int dec_spawned = 0, dec_on = 0, dec_halt = 0;
    size_t dec_seq = 0, dec_reaped = 0;
    arc_linkq links; /* 符号链接目标队列（异常路径也要释放） */
    memset(&links, 0, sizeof(links));

    FILE *f = NULL;
    arc_index ix;
    omnic_status ost = arc_open_index(archive_path, &f, &ix);
    if (ost != OMNIC_OK) return ost;

    /* --file 名匹配（精确/目录前缀/glob 三种）：零命中即用法错误（可操作） */
    unsigned char *want_ent = NULL, *want_blk = NULL;
    size_t *blk_cnt = NULL, *blk_off = NULL, *ent_ord = NULL;
    if (n_only > 0) {
        want_ent = (unsigned char *)calloc(ix.nent ? ix.nent : 1, 1);
        want_blk = (unsigned char *)calloc(ix.nblk ? ix.nblk : 1, 1);
        if (want_ent == NULL || want_blk == NULL) goto sys_fail_ix;
        size_t missing = 0;
        for (size_t k = 0; k < n_only; k++) {
            size_t hit = 0;
            for (size_t i = 0; i < ix.nent; i++) {
                const arc_entry *e = &ix.ent[i];
                if (e->type == ARC_T_DIR) continue; /* 目录条目不可单选；文件、符号链接、硬链接可以 */
                if (!name_matches(only[k], strlen(only[k]), e->name)) continue;
                want_ent[i] = 1;
                if (ent_has_data(e)) want_blk[e->block_id] = 1;
                hit++;
            }
            if (hit == 0) {
                fprintf(stderr, "omnic: --file not in archive: %s\n", only[k]);
                missing++;
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
    /* 分桶（D8）：有数据的条目按 block_id 计数排序，替代逐块 O(nent) 全表扫描；
     * 桶计数即坏块失败口径（原 blk_ents）——只数被选中的，坏块才不会连累同块的
     * 未选条目。njobs = 待写作业总数。 */
    blk_cnt = (size_t *)calloc(ix.nblk ? ix.nblk : 1, sizeof(size_t));
    blk_off = (size_t *)malloc((ix.nblk + 1) * sizeof(size_t));
    ent_ord = (size_t *)malloc((ix.nent ? ix.nent : 1) * sizeof(size_t));
    if (blk_cnt == NULL || blk_off == NULL || ent_ord == NULL) goto sys_fail_ix;
    size_t njobs = 0;
    for (size_t i = 0; i < ix.nent; i++)
        if (ent_has_data(&ix.ent[i]) && (want_ent == NULL || want_ent[i])) {
            blk_cnt[ix.ent[i].block_id]++;
            njobs++;
        }
    blk_off[0] = 0;
    for (size_t bi = 0; bi < ix.nblk; bi++) blk_off[bi + 1] = blk_off[bi] + blk_cnt[bi];
    {
        size_t *cur = (size_t *)calloc(ix.nblk ? ix.nblk : 1, sizeof(size_t));
        if (cur == NULL) goto sys_fail_ix;
        for (size_t i = 0; i < ix.nent; i++)
            if (ent_has_data(&ix.ent[i]) && (want_ent == NULL || want_ent[i]))
                ent_ord[blk_off[ix.ent[i].block_id] + cur[ix.ent[i].block_id]++] = i;
        free(cur);
    }

    if (!verify_only && want_ent == NULL && omnic_fs_mkdir_p(dstroot) != OMNIC_OK)
        goto sys_fail_ix;
    /* 目录条目先行（含空目录；仅全量提取）。单个目录建不出来只记失败、继续——
     * 其下文件在写盘阶段逐个落败；输出根目录建不出来才在上面就中止。
     * 目录的 mtime 一律不恢复：往目录里写文件本身就会刷新它，只有空目录保得住——
     * 半保半丢不如都不保（文件时间照常恢复）。 */
    size_t dirs_failed = 0;
    if (!verify_only && want_ent == NULL) {
        for (size_t i = 0; i < ix.nent; i++) {
            if (ix.ent[i].type != ARC_T_DIR) continue;
            char *dst = (char *)malloc(strlen(dstroot) + strlen(ix.ent[i].name) + 2);
            if (dst == NULL) goto sys_fail_ix;
            sprintf(dst, "%s/%s", dstroot, ix.ent[i].name);
            omnic_status st = omnic_fs_mkdir_p(dst);
            if (st != OMNIC_OK) {
                if (dirs_failed == 0) omnic_set_error("cannot create directory: %s", dst);
                dirs_failed++;
            }
            free(dst);
        }
    }

    /* 并行判定：单线程/小作业量走串行原路径（--file 单提取 8.7ms 不启池） */
    /* 写量/块量预算先行：并行写盘判据要用（条目数或总字节任一过线） */
    size_t n_decode_blks = 0;
    uint64_t dec_raw_sum = 0;
    for (size_t bi = 0; bi < ix.nblk; bi++)
        if (want_blk == NULL || want_blk[bi]) {
            n_decode_blks++;
            dec_raw_sum += ix.blk[bi].raw_size;
        }
    int writers = ARC_NWRITER_DEFAULT;
    if (o != NULL && o->threads == 1) writers = 1;
    else if (o != NULL && o->threads > 1) writers = o->threads;
    if (writers > ARC_NWRITER_MAX) writers = ARC_NWRITER_MAX;
    int force = (o != NULL && (o->flags & OMNIC_F_FORCE) != 0);
    /* 全量提取本来就是覆盖语义：既存的只读文件也直接覆盖；--file 单选才要求 -f */
    int wforce = force || want_ent == NULL;
    arc_pool pool;
    int parallel = (!verify_only && writers > 1 && njobs >= 2 &&
                    (njobs >= ARC_PAR_MIN_JOBS || dec_raw_sum >= ARC_PAR_MIN_BYTES) &&
                    pool_init(&pool, &ix, dstroot, writers, wforce) == 0);
    unsigned char *went = NULL;
    if (parallel) {
        went = (unsigned char *)calloc(ix.nent ? ix.nent : 1, 1);
        if (went == NULL) { /* OOM：停池退回串行 */
            pool_shutdown(&pool);
            free(pool.result);
            parallel = 0;
        }
    }

    /* 解码池（B）：≥2 个待解块才有并行价值（单块无重叠窗口）；--file 单命中
     * 通常单块，自然落回串行。res 环容量 = workers+1 > 在途上限 workers。 */
    int dec_ok = (writers > 1 && n_decode_blks >= 2);
    if (dec_ok) {
        (void)omnic_crc32(0, "", 0); /* 预热分表：worker 可能先于主线程首调 */
        dec.path = archive_path;
        dec.res_cap = (size_t)writers + 1;
        dec.res = (dec_result *)calloc(dec.res_cap, sizeof(dec_result));
        if (dec.res != NULL) {
            arc_mutex_init(&dec.lock);
            arc_cond_init(&dec.job_cv);
            arc_cond_init(&dec.res_cv);
            for (; dec_spawned < writers; dec_spawned++)
                if (arc_thread_create(&dec_tid[dec_spawned], dec_worker, &dec) != 0) break;
            if (dec_spawned == 0) { /* 一线程未起：退串行 */
                free(dec.res);
                dec.res = NULL;
            } else {
                dec_on = 1;
            }
        }
    }

    /* 逐块解压切条目；坏块跳过隔离（AR-ERR-001，T2 模板）。
     * 解码池路径（B）：交错单循环——每轮先把窗口补满（在途 ≤ dec_spawned），
     * 再收割最旧回执走原条目逻辑：保序收割 = 块序/坏块/条目口径与串行逐项
     * 一致。派发中断（作业 OOM）后余块经 dec_reaped == dec_seq 分支串行兜底。 */
    size_t extracted = 0, failed = 0, blocks_ok = 0;
    if (dec_on) {
        size_t sub = 0; /* 派发游标：与处理游标 bi 在同一过滤序列上推进 */
        for (size_t bi = 0; bi < ix.nblk; bi++) {
            if (want_blk != NULL && !want_blk[bi]) continue;
            while (dec_halt == 0 && dec_seq - dec_reaped < (size_t)dec_spawned &&
                   sub < ix.nblk) {
                if (want_blk != NULL && !want_blk[sub]) { sub++; continue; }
                dec_job *j = (dec_job *)malloc(sizeof(dec_job));
                if (j == NULL) { dec_halt = 1; break; }
                j->b = &ix.blk[sub];
                j->seq = dec_seq++;
                if (dec_submit(&dec, j) != 0) { dec_halt = 1; break; }
                sub++;
            }
            const arc_block *b = &ix.blk[bi];
            int slot = parallel ? pool_acquire_slot(&pool) : -1;
            uint8_t *raw = NULL; /* 本块原始字节（store 直读 / 压缩解压产物） */
            int drc;
            if (dec_reaped < dec_seq) {
                dec_result *r = dec_reap(&dec, dec_reaped++);
                raw = r->raw;
                drc = r->drc;
                r->raw = NULL; /* 所有权移交 */
            } else {
                drc = decode_block(f, b, &raw); /* 池停后的串行兜底 */
            }
            if (drc != 0) {
                if (drc == 1) report_bad_block(&ix, bi, b->fmt, quiet);
                else if (drc == 2) omnic_set_error("block %zu: no decoder for format %u", bi, b->fmt);
                else omnic_set_error("out of memory");
                failed += blk_cnt[bi]; /* 坏块的条目全计失败：extracted N/M 口径 */
                if (parallel) pool_close_slot(&pool, slot);
                continue;
            }
            if (parallel) pool.slot[slot].buf = raw;
            blocks_ok++;
            for (size_t k = blk_off[bi]; k < blk_off[bi + 1]; k++) {
                size_t i = ent_ord[k];
                arc_entry *e = &ix.ent[i];
                if (want_ent != NULL && !want_ent[i]) continue;
                if (e->size > b->raw_size || e->in_off > b->raw_size - e->size ||
                    omnic_crc32(0, raw + e->in_off, (size_t)e->size) != e->crc) {
                    if (!quiet)
                        fprintf(stderr, "omnic: entry corrupted — skipped: %s\n", e->name);
                    failed++;
                    continue;
                }
                if (verify_only) {
                    rep->out_bytes += e->size;
                    extracted++;
                    continue;
                }
                if (e->type == ARC_T_SYMLINK) { /* 目标入队；文件全部落盘后才建链接 */
                    if (linkq_push(&links, i, e, raw) != 0) failed++;
                    continue;
                }
                if (want_ent != NULL) {
                    size_t dl = strlen(dstroot) + strlen(e->name) + 2;
                    char *dst = (char *)malloc(dl);
                    if (dst == NULL) {
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
                    if (dst == NULL) { free(raw); goto sys_fail_ix; }
                    sprintf(dst, "%s/%s", dstroot, e->name);
                    int rc = write_entry(dst, raw + e->in_off, e, wforce);
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
        /* 循环每轮收一块，退出时在途必已清空（dec_reaped == dec_seq） */
        dec_pool_shutdown(&dec, dec_tid, dec_spawned);
    } else {
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
                omnic_crc32(0, raw + e->in_off, (size_t)e->size) != e->crc) {
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
            if (e->type == ARC_T_SYMLINK) { /* 目标入队；文件全部落盘后才建链接 */
                if (linkq_push(&links, i, e, raw) != 0) failed++;
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
                int rc = write_entry(dst, raw + e->in_off, e, wforce);
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
    } /* 串行路径结束（else 分支闭合） */
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
    /* 硬链接 / 符号链接 / 目录权限：文件全部落盘之后 */
    if (!verify_only)
        finish_links(&ix, dstroot, want_ent, force, &links, f, &extracted, &failed, quiet);
    linkq_free(&links);
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
            printf("verified %zu/%zu entries, %zu blocks OK\n", extracted, extracted + failed,
                   blocks_ok);
    } else if ((failed > 0 || dirs_failed > 0) && !quiet) {
        if (dirs_failed > 0)
            fprintf(stderr, "omnic: %zu directories could not be created under %s\n", dirs_failed,
                    dstroot);
        fprintf(stderr, "omnic: extracted %zu/%zu entries (%zu failed)\n", extracted,
                extracted + failed, failed);
    }
    return (failed || dirs_failed) ? OMNIC_E_DATA : OMNIC_OK;

sys_fail_ix:
    if (dec_spawned > 0) dec_pool_shutdown(&dec, dec_tid, dec_spawned);
    linkq_free(&links);
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

/* --list：只读索引不读 Data 区（§5.2）；ratio 为该条目所在块级压缩率。
 * 符号链接的目标串在数据块里，列表不解码，只标出它是链接。 */
omnic_status omnic_arc_list(const char *archive_path, const omnic_options *o) {
    if (archive_path == NULL) return OMNIC_E_USAGE;
    FILE *f = NULL;
    arc_index ix;
    omnic_status st = arc_open_index(archive_path, &f, &ix);
    if (st != OMNIC_OK) return st;
    if (!(o != NULL && (o->flags & OMNIC_F_QUIET))) {
        size_t files = 0, dirs = 0, links = 0;
        for (size_t i = 0; i < ix.nent; i++) {
            const arc_entry *e = &ix.ent[i];
            if (e->type == ARC_T_DIR) {
                printf("%s\t-\tdir\n", e->name);
                dirs++;
                continue;
            }
            if (e->type == ARC_T_SYMLINK || e->type == ARC_T_HARDLINK) {
                /* 硬链接条目自身不带数据：显示它指向的文件的大小 */
                unsigned long long sz = (e->type == ARC_T_HARDLINK) ? ix.ent[e->link_to].size
                                                                    : e->size;
                printf("%s\t%llu\t%s\n", e->name, sz,
                       e->type == ARC_T_SYMLINK ? "symlink" : "hardlink");
                links++;
                continue;
            }
            const arc_block *b = &ix.blk[e->block_id];
            printf("%s\t%llu\t%s\t%.1f%%\n", e->name, (unsigned long long)e->size,
                   b->fmt == 0 ? "store" : omnic_fmt_str((omnic_fmt_id)b->fmt),
                   b->raw_size ? 100.0 * (double)b->comp_size / (double)b->raw_size : 0.0);
            files++;
        }
        printf("%zu files, %zu dirs, %zu blocks\n", files, dirs, ix.nblk);
        if (links > 0) printf("%zu links\n", links);
    }
    arc_index_mem_free(&ix);
    fclose(f);
    return OMNIC_OK;
}

#endif /* OMNIC_CODEC_ZSTD */
