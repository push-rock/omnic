/* archive：.omnic v1 实现（ARCHIVE_SPEC §3/§4）。
 * pack 单遍写出：Header → Data（块流式落盘，块/条目双层 CRC 边写边算）→
 * Index(zstd) → 回填 Header n_blocks → Footer。
 * unpack：Header/Footer 双端校验 → 索引 CRC + 结构校验 → 逐块解压切条目，
 * 坏块跳过隔离（AR-ERR-001）。CRC-32 自带（懒初始化表），不绑可选 codec。
 * A3 范围：store 块 only；压缩块与决策分桶在 A4 接入（fmt!=0 当前为桩）。 */
#include "core/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/util.h"

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
            if (ix->blk[i].comp_size == 0 || ix->blk[i].raw_size == 0 ||
                ix->blk[i].raw_size > (1u << 30)) /* D14 炸弹预检 */
                return OMNIC_E_DATA;
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
    uint64_t block_max = (uint64_t)OMNIC_ARC_BLOCK_MIB * 1024u * 1024u;

    uint8_t hdr[ARC_HDR];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, ARC_MAGIC_H, 8);
    put16(hdr + 8, OMNIC_ARC_VERSION);
    put16(hdr + 10, OMNIC_ARC_F_MTIME | OMNIC_ARC_F_INDEX_ZSTD);
    put32(hdr + 12, 0); /* n_blocks 回填于尾部 */
    put32(hdr + 16, (uint32_t)n);
    put64(hdr + 20, data_off);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto io_fail;

    size_t blk_cap = 64;
    ix.blk = (arc_block *)malloc(blk_cap * sizeof(arc_block));
    if (ix.blk == NULL) goto oom;
    double t0 = omnic_now_sec();
    uint32_t blk_crc = 0;
    int blk_open = 0;
    for (size_t i = 0; i < n; i++) {
        if (ix.ent[i].type == 1) continue;
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
        if (!blk_open ||
            (ix.blk[ix.nblk - 1].raw_size > 0 &&
             (ix.blk[ix.nblk - 1].raw_size + fs[i].size > block_max ||
              fs[i].size > block_max))) {
            if (ix.nblk == blk_cap) {
                blk_cap *= 2;
                arc_block *b = (arc_block *)realloc(ix.blk, blk_cap * sizeof(arc_block));
                if (b == NULL) {
                    fclose(in);
                    goto oom;
                }
                ix.blk = b;
            }
            memset(&ix.blk[ix.nblk], 0, sizeof(arc_block));
            ix.blk[ix.nblk].fmt = 0; /* store */
            ix.blk[ix.nblk].offset = arc_pos - data_off;
            ix.nblk++;
            blk_crc = 0;
            blk_open = 1;
        }
        arc_block *b = &ix.blk[ix.nblk - 1];
        ix.ent[i].block_id = (uint32_t)(ix.nblk - 1);
        ix.ent[i].in_off = b->raw_size;
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
            b->raw_size += r;
            b->comp_size = b->raw_size;
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
        rep->in_bytes += fs[i].size;
    }
    if (blk_open) {
        arc_block *b = &ix.blk[ix.nblk - 1];
        b->crc = blk_crc;
    }
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
    arc_index_mem_free(&ix);
    free(fs);
    if (f != NULL) fclose(f);
    return OMNIC_E_SYSTEM;
}

/* ---- unpack ---- */
omnic_status omnic_unpack(const char *archive_path, const char *dest_dir,
                          const omnic_options *o, omnic_report *rep) {
    (void)o;
    if (archive_path == NULL) return OMNIC_E_USAGE;
    omnic_report tmp;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    const char *dstroot = (dest_dir != NULL) ? dest_dir : ".";

    FILE *f = omnic_fs_fopen(archive_path, "rb");
    if (f == NULL) {
        omnic_set_error("cannot open archive: %s", archive_path);
        return OMNIC_E_SYSTEM;
    }
    /* Header 验（magic/version）；Footer 锚定 + 交叉校验 */
    uint8_t hdr[ARC_HDR], ftr[ARC_FTR];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, ARC_MAGIC_H, 8) != 0) {
        omnic_set_error("not a valid .omnic archive (bad header magic)");
        goto data_fail;
    }
    if (get16(hdr + 8) != OMNIC_ARC_VERSION) {
        omnic_set_error("unsupported .omnic version %u", get16(hdr + 8));
        goto data_fail;
    }
    if (fseek(f, -(long)sizeof(ftr), SEEK_END) != 0 ||
        fread(ftr, 1, sizeof(ftr), f) != sizeof(ftr) || memcmp(ftr, ARC_MAGIC_F, 8) != 0) {
        omnic_set_error("not a valid .omnic archive (bad footer magic)");
        goto data_fail;
    }
    uint64_t index_off = get64(ftr + 8), index_comp = get64(ftr + 16);
    uint32_t index_crc = get32(ftr + 24);
    uint32_t nent = get16(ftr + 28), nblk = get16(ftr + 30);
    if (get32(hdr + 16) != nent || get32(hdr + 12) != nblk) {
        omnic_set_error("archive header/footer mismatch — corrupt archive");
        goto data_fail;
    }
    if (index_off < ARC_HDR || index_comp == 0 || index_comp > (1u << 30)) goto data_fail;

    uint8_t *zbuf = (uint8_t *)malloc((size_t)index_comp);
    if (zbuf == NULL) goto sys_fail;
    if (arc_seek(f, index_off) != 0 || fread(zbuf, 1, (size_t)index_comp, f) != (size_t)index_comp) {
        free(zbuf);
        goto data_fail;
    }
    if (arc_crc32(0, zbuf, (size_t)index_comp) != index_crc) {
        free(zbuf);
        omnic_set_error("archive index corrupted — cannot list or extract");
        goto data_fail;
    }
    uint64_t plain_cap = ZSTD_getFrameContentSize(zbuf, (size_t)index_comp);
    if (plain_cap == 0 || plain_cap == ZSTD_CONTENTSIZE_ERROR || plain_cap == ZSTD_CONTENTSIZE_UNKNOWN ||
        plain_cap > (64u << 20)) {
        free(zbuf);
        goto data_fail;
    }
    uint8_t *plain = (uint8_t *)malloc((size_t)plain_cap);
    if (plain == NULL) {
        free(zbuf);
        goto sys_fail;
    }
    size_t plain_n = ZSTD_decompress(plain, (size_t)plain_cap, zbuf, (size_t)index_comp);
    free(zbuf);
    if (ZSTD_isError(plain_n)) {
        free(plain);
        goto data_fail;
    }
    arc_index ix;
    omnic_status pst = arc_index_parse(plain, plain_n, nent, nblk, 1, &ix);
    free(plain);
    if (pst == OMNIC_E_SYSTEM) {
        goto sys_fail;
    }
    if (pst != OMNIC_OK) {
        if (omnic_last_error()[0] == '\0') omnic_set_error("corrupt archive index");
        goto data_fail;
    }

    if (omnic_fs_mkdir_p(dstroot) != OMNIC_OK) goto sys_fail;
    /* 目录条目先行（含空目录） */
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

    /* 逐块解 store 段（压缩块为 A4 桩）；坏块跳过隔离 */
    size_t extracted = 0, failed = 0;
    uint8_t *blk_mem = NULL;
    uint64_t blk_cap = 0;
    for (size_t bi = 0; bi < ix.nblk; bi++) {
        const arc_block *b = &ix.blk[bi];
        if (b->fmt != 0) {
            omnic_set_error("compressed block (fmt %u) requires newer build", b->fmt);
            failed++;
            continue;
        }
        if (blk_cap < b->raw_size) {
            uint8_t *m = (uint8_t *)realloc(blk_mem, (size_t)b->raw_size);
            if (m == NULL) {
                free(blk_mem);
                goto sys_fail_ix;
            }
            blk_mem = m;
            blk_cap = b->raw_size;
        }
        if (arc_seek(f, ARC_HDR + b->offset) != 0 ||
            fread(blk_mem, 1, (size_t)b->raw_size, f) != (size_t)b->raw_size ||
            arc_crc32(0, blk_mem, (size_t)b->raw_size) != b->crc) {
            if (!(o != NULL && (o->flags & OMNIC_F_QUIET)))
                fprintf(stderr, "omnic: block %zu (fmt %u) corrupted — skipped\n", bi, b->fmt);
            failed++;
            continue;
        }
        for (size_t i = 0; i < ix.nent; i++) {
            arc_entry *e = &ix.ent[i];
            if (e->type != 0 || e->block_id != (uint32_t)bi) continue;
            size_t dl = strlen(dstroot) + strlen(e->name) + 2;
            char *dst = (char *)malloc(dl);
            if (dst == NULL) {
                free(blk_mem);
                goto sys_fail_ix;
            }
            sprintf(dst, "%s/%s", dstroot, e->name);
            /* 父目录兜底（条目序外缺失时） */
            char *slash = strrchr(dst, '/');
            if (slash != NULL) {
                *slash = '\0';
                if (omnic_fs_mkdir_p(dst) != OMNIC_OK) {
                    *slash = '/';
                    free(dst);
                    failed++;
                    continue;
                }
                *slash = '/';
            }
            FILE *out = omnic_fs_fopen(dst, "wb");
            if (out == NULL) {
                omnic_set_error("cannot create: %s", dst);
                free(dst);
                failed++;
                continue;
            }
            size_t wsz = fwrite(blk_mem + e->in_off, 1, (size_t)e->size, out);
            fclose(out);
            if (wsz != (size_t)e->size) {
                free(dst);
                failed++;
                continue;
            }
            omnic_fs_set_mtime(dst, e->mtime);
            free(dst);
            rep->out_bytes += e->size;
            extracted++;
        }
    }
    free(blk_mem);
    arc_index_mem_free(&ix);
    fclose(f);
    rep->in_bytes = rep->out_bytes;
    if (failed > 0 && !(o != NULL && (o->flags & OMNIC_F_QUIET)))
        fprintf(stderr, "omnic: extracted %zu/%zu files (%zu failed)\n", extracted,
                extracted + failed, failed);
    return failed ? OMNIC_E_DATA : OMNIC_OK;

data_fail:
    fclose(f);
    return OMNIC_E_DATA;
sys_fail:
    fclose(f);
    return OMNIC_E_SYSTEM;
sys_fail_ix:
    arc_index_mem_free(&ix);
    fclose(f);
    return OMNIC_E_SYSTEM;
}

#endif /* OMNIC_CODEC_ZSTD */
