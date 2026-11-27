/* 归档回归（ARCHIVE_SPEC §9 矩阵）：四策略 roundtrip、提取形态、损坏隔离、
 * 畸形拒绝。索引注入用 zstd 重写（与 pack 同级 3）。 */
/* _DEFAULT_SOURCE 须先于任何系统头：-std=c11 严格模式下 dup/dup2/fileno 需显式开启 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#endif

#include <zstd.h>

#include "core/archive.h"
#include "core/fs.h"
#include "core/util.h"
#include "omnic.h"

#ifndef OMNIC_CODEC_ZSTD
int main(void) { return 0; } /* 归档功能依赖 zstd（构建开关关闭时空跑） */
#else

static int g_fail = 0;
static int g_run = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_run++;                                                             \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("FAIL %s:%d: %s  [err: %s]\n", __FILE__, __LINE__, #cond, \
                   omnic_last_error());                                      \
        }                                                                    \
    } while (0)

/* 索引明文布局（与 archive.c 序列化对齐）：v2 条目定长 34B（type/flags/block_id/in_off/
 * size/crc/mtime），可选字段按 flags 跟在后面，次序固定。 */
#define T_BLK_ROW 32
#define T_ENT_FIX 34
#define T_EF_CTIME 0x04u
#define T_EF_MODE 0x08u
#define T_EF_ATTRS 0x10u
#define T_EF_LINKTO 0x40u

/* 一条记录定长部分的长度（含可选字段）；row 指向 name 之后的 type 字节 */
static size_t ent_row_len(const uint8_t *row) {
    uint8_t f = row[1];
    size_t len = T_ENT_FIX;
    if (f & T_EF_LINKTO) len += 4;
    if (f & T_EF_CTIME) len += 8;
    if (f & T_EF_MODE) len += 2;
    if (f & T_EF_ATTRS) len += 2;
    return len;
}

static const char *TREE = "omnic_arc_tree";
static const char *ARC = "omnic_arc_t.omnic";

static unsigned rng_state = 987654321u;
static unsigned rng(void) { return (rng_state = rng_state * 1103515245u + 12345u); }

static void wfile(const char *path, const void *data, size_t n) {
    FILE *f = omnic_fs_fopen(path, "wb");
    if (f == NULL) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

/* 可压缩文本：词表随机句 */
static void fill_text(char *buf, size_t n) {
    static const char *words[] = {"archive", "block", "codec", "bucket", "entropy",
                                  "decision", "solid", "index", "verify", "omnic"};
    size_t w = 0;
    while (w + 16 < n) {
        const char *t = words[rng() % 10];
        size_t l = strlen(t);
        memcpy(buf + w, t, l);
        w += l;
        buf[w++] = ' ';
    }
    while (w < n) buf[w++] = '.';
}

static void make_tree(void) {
    omnic_fs_mkdir_p(TREE);
    char p[512];
    static char buf[256 * 1024];

    fill_text(buf, 1024);
    snprintf(p, sizeof(p), "%s/a.txt", TREE);
    wfile(p, buf, 1024);
    fill_text(buf, 2048);
    snprintf(p, sizeof(p), "%s/b.txt", TREE);
    wfile(p, buf, 2048);

    /* 重复内容（跨文件冗余） */
    fill_text(buf, 512);
    snprintf(p, sizeof(p), "%s/twin1.txt", TREE);
    wfile(p, buf, 512);
    snprintf(p, sizeof(p), "%s/twin2.txt", TREE);
    wfile(p, buf, 512);

    /* 空文件 / 高熵（skip→store） */
    snprintf(p, sizeof(p), "%s/empty.bin", TREE);
    wfile(p, "", 0);
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (char)rng();
    snprintf(p, sizeof(p), "%s/rand.bin", TREE);
    wfile(p, buf, 32768);

    /* 中文名 */
    snprintf(p, sizeof(p), "%s/中文目录", TREE);
    omnic_fs_mkdir_p(p);
    fill_text(buf, 300);
    snprintf(p, sizeof(p), "%s/中文目录/中文文件.txt", TREE);
    wfile(p, buf, 300);

    /* 16 级深路径 */
    char deep[512];
    snprintf(deep, sizeof(deep), "%s", TREE);
    for (int i = 0; i < 16; i++) {
        strcat(deep, "/d");
        omnic_fs_mkdir_p(deep);
    }
    fill_text(buf, 100);
    strcat(deep, "/deep.txt");
    wfile(deep, buf, 100);

    /* 空目录 */
    snprintf(p, sizeof(p), "%s/sub_empty", TREE);
    omnic_fs_mkdir_p(p);
}

static void rm_tree(const char *path) {
    char cmd[1024];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rd /s /q \"%s\" 2>nul", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
    system(cmd);
}

static int cmp_fs_ent(const void *a, const void *b) {
    return strcmp(((const omnic_fs_entry *)a)->rel, ((const omnic_fs_entry *)b)->rel);
}

/* 两树文件集 + 内容 + mtime(±2s) 一致（walk 序在 *nix 上不保证，先排序） */
static int tree_equal(const char *a, const char *b) {
    size_t na = 0, nb = 0, sk = 0;
    omnic_fs_entry *fa = omnic_fs_walk(a, &na, &sk);
    omnic_fs_entry *fb = omnic_fs_walk(b, &nb, &sk);
    if (fa == NULL || fb == NULL) {
        omnic_fs_entries_free(fa, na);
        omnic_fs_entries_free(fb, nb);
        return 0;
    }
    qsort(fa, na, sizeof(*fa), cmp_fs_ent);
    qsort(fb, nb, sizeof(*fb), cmp_fs_ent);
    int ok = (na == nb);
    if (ok) {
        for (size_t i = 0; i < na && ok; i++) {
            if (strcmp(fa[i].rel, fb[i].rel) != 0 || fa[i].is_dir != fb[i].is_dir ||
                fa[i].link_kind != fb[i].link_kind || fa[i].size != fb[i].size) {
                ok = 0;
                break;
            }
            if (fa[i].link_kind != 0) { /* 链接：比种类与目标，不跟随 */
                if (fa[i].link_target == NULL || fb[i].link_target == NULL ||
                    strcmp(fa[i].link_target, fb[i].link_target) != 0)
                    ok = 0;
                continue;
            }
            if (fa[i].is_dir) continue;
            char pa[1024], pb[1024];
            snprintf(pa, sizeof(pa), "%s/%s", a, fa[i].rel);
            snprintf(pb, sizeof(pb), "%s/%s", b, fb[i].rel);
            FILE *h1 = omnic_fs_fopen(pa, "rb");
            FILE *h2 = omnic_fs_fopen(pb, "rb");
            if (h1 == NULL || h2 == NULL) {
                if (h1) fclose(h1);
                if (h2) fclose(h2);
                ok = 0;
                break;
            }
            char b1[64 * 1024], b2[64 * 1024];
            size_t r1, r2;
            do {
                r1 = fread(b1, 1, sizeof(b1), h1);
                r2 = fread(b2, 1, sizeof(b2), h2);
                if (r1 != r2 || memcmp(b1, b2, r1) != 0) ok = 0;
            } while (ok && r1 == sizeof(b1));
            fclose(h1);
            fclose(h2);
            if (ok) {
                long long d = fa[i].mtime - fb[i].mtime;
                if (d < -2 || d > 2) ok = 0; /* mtime 保持（秒级容差） */
            }
        }
    }
    omnic_fs_entries_free(fa, na);
    omnic_fs_entries_free(fb, nb);
    return ok;
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = omnic_fs_fopen(a, "rb"), *fb = omnic_fs_fopen(b, "rb");
    if (fa == NULL || fb == NULL) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    char ba[64 * 1024], bb[64 * 1024];
    size_t na, nb;
    do {
        na = fread(ba, 1, sizeof(ba), fa);
        nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) {
            fclose(fa);
            fclose(fb);
            return 0;
        }
    } while (na == sizeof(ba));
    fclose(fa);
    fclose(fb);
    return 1;
}

static omnic_options mkopts(unsigned flags, int block_mib) {
    omnic_options o;
    memset(&o, 0, sizeof(o));
    o.profile = OMNIC_PROFILE_BALANCED;
    o.flags = flags;
    o.block_mib = block_mib;
    return o;
}

static omnic_report g_rep;

static omnic_status pack_tree(const char *root, const char *arc, omnic_options o) {
    memset(&g_rep, 0, sizeof(g_rep));
    return omnic_pack(root, arc, &o, &g_rep);
}

static omnic_status unpack_arc(const char *arc, const char *dir, omnic_options o) {
    memset(&g_rep, 0, sizeof(g_rep));
    return omnic_unpack(arc, dir, &o, &g_rep);
}

/* ---- 索引注入（损坏/畸形共用） ---- */

static uint8_t *load_index_plain(const char *arc, size_t *out_n, uint64_t *index_off,
                                 uint32_t *nent, uint32_t *nblk) {
    FILE *f = omnic_fs_fopen(arc, "rb");
    if (f == NULL) return NULL;
    uint8_t ftr[32];
    fseek(f, -32, SEEK_END);
    if (fread(ftr, 1, 32, f) != 32 || memcmp(ftr, "OMNICEND", 8) != 0) {
        fclose(f);
        return NULL;
    }
    uint64_t io = 0, ic = 0;
    for (int i = 0; i < 8; i++) io |= (uint64_t)ftr[8 + i] << (8 * i);  /* 小端（与 put64 一致） */
    for (int i = 0; i < 8; i++) ic |= (uint64_t)ftr[16 + i] << (8 * i);
    /* 计数：v2 在头部（32 位），尾部那 4 字节保留为 0；v1 才在尾部（16 位） */
    uint8_t hdr[32];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 32, f) != 32) {
        fclose(f);
        return NULL;
    }
    if ((hdr[8] | (hdr[9] << 8)) >= 2) {
        *nent = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) | ((uint32_t)hdr[18] << 16) |
                ((uint32_t)hdr[19] << 24);
        *nblk = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) | ((uint32_t)hdr[14] << 16) |
                ((uint32_t)hdr[15] << 24);
    } else {
        *nent = (uint32_t)ftr[28] | ((uint32_t)ftr[29] << 8);
        *nblk = (uint32_t)ftr[30] | ((uint32_t)ftr[31] << 8);
    }
    uint8_t *zbuf = (uint8_t *)malloc((size_t)ic);
    if (zbuf == NULL || fseek(f, (long)io, SEEK_SET) != 0 ||
        fread(zbuf, 1, (size_t)ic, f) != (size_t)ic) {
        free(zbuf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    uint64_t cap = ZSTD_getFrameContentSize(zbuf, (size_t)ic);
    if (cap == 0 || cap > (64u << 20)) {
        free(zbuf);
        return NULL;
    }
    uint8_t *plain = (uint8_t *)malloc((size_t)cap);
    size_t n = ZSTD_decompress(plain, (size_t)cap, zbuf, (size_t)ic);
    free(zbuf);
    if (ZSTD_isError(n)) {
        free(plain);
        return NULL;
    }
    *out_n = n;
    *index_off = io;
    return plain;
}

/* 改后回写：truncate 到 index_off → zstd/3 压缩 → 新索引 + 新 Footer */
static int save_index_plain(const char *arc, uint8_t *plain, size_t n, uint64_t index_off) {
    size_t zcap = ZSTD_compressBound(n);
    uint8_t *zbuf = (uint8_t *)malloc(zcap);
    if (zbuf == NULL) return -1;
    size_t zn = ZSTD_compress(zbuf, zcap, plain, n, 3);
    if (ZSTD_isError(zn)) {
        free(zbuf);
        return -1;
    }
    FILE *f = omnic_fs_fopen(arc, "r+b");
    if (f == NULL) {
        free(zbuf);
        return -1;
    }
    /* CRC-32/IEEE（与归档同款多项式），流式 */
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < zn; i++) {
        crc ^= zbuf[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc ^= 0xFFFFFFFFu;
    fseek(f, (long)index_off, SEEK_SET);
    fwrite(zbuf, 1, zn, f);
    uint8_t ftr[32];
    memset(ftr, 0, sizeof(ftr));
    memcpy(ftr, "OMNICEND", 8);
    for (int i = 0; i < 8; i++) ftr[8 + i] = (uint8_t)(index_off >> (8 * i));   /* 小端 */
    for (int i = 0; i < 8; i++) ftr[16 + i] = (uint8_t)(zn >> (8 * i));
    for (int i = 0; i < 4; i++) ftr[24 + i] = (uint8_t)(crc >> (8 * i));
    /* 尾部 28..32：v2 保留为 0（计数以头部为准） */
    fwrite(ftr, 1, 32, f);
    /* "r+" 不截断：重压索引比原短时尾部残留旧字节，SEEK_END 读 Footer 会落空 */
#if defined(_WIN32)
    _chsize(fileno(f), (long)(index_off + zn + 32));
#else
    ftruncate(fileno(f), (off_t)(index_off + zn + 32));
#endif
    fclose(f);
    free(zbuf);
    return 0;
}

/* 按名定位条目行偏移：条目序 = walk 序，ext4/APFS 的 readdir 序与 NTFS 字典序
 * 不同且目录条目(type=1)混杂其间——注入用例禁止按序数定位条目 */
static long find_ent(const uint8_t *plain, uint32_t nblk, uint32_t nent, const char *name) {
    size_t off = (size_t)nblk * T_BLK_ROW;
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < nent; i++) {
        uint32_t nl = plain[off] | ((uint32_t)plain[off + 1] << 8);
        if (nl == nlen && memcmp(plain + off + 2, name, nl) == 0) return (long)off;
        off += 2 + nl + ent_row_len(plain + off + 2 + nl);
    }
    return -1;
}

static void flip_byte(const char *path, long off) {
    FILE *f = omnic_fs_fopen(path, "r+b");
    if (f == NULL) return;
    fseek(f, off, SEEK_SET);
    int c = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(c ^ 0xFF, f);
    fclose(f);
}

static long file_size(const char *path) {
    FILE *f = omnic_fs_fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* ---- 用例 ---- */

static void test_roundtrip(void) {
    make_tree();
    struct {
        const char *name;
        unsigned flags;
    } cases[] = {
        {"solid", 0},           {"per-file", OMNIC_F_PER_FILE},
        {"no-skip", OMNIC_F_NO_SKIP}, {"per-file+no-skip", OMNIC_F_PER_FILE | OMNIC_F_NO_SKIP},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char out[512];
        snprintf(out, sizeof(out), "omnic_arc_out_%s", cases[c].name);
        rm_tree(out);
        CHECK(pack_tree(TREE, ARC, mkopts(cases[c].flags | OMNIC_F_QUIET, 0)) == OMNIC_OK);
        CHECK(unpack_arc(ARC, out, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
        CHECK(tree_equal(TREE, out));
        rm_tree(out);
    }
    remove(ARC);
    rm_tree(TREE);
}

static void test_roundtrip_edges(void) {
    /* 空目录树 */
    omnic_fs_mkdir_p("omnic_arc_e1");
    omnic_fs_mkdir_p("omnic_arc_e1/x/y");
    CHECK(pack_tree("omnic_arc_e1", ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    rm_tree("omnic_arc_e2");
    CHECK(unpack_arc(ARC, "omnic_arc_e2", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_e1", "omnic_arc_e2"));
    rm_tree("omnic_arc_e1");
    rm_tree("omnic_arc_e2");

    /* 单文件树（无目录条目） */
    omnic_fs_mkdir_p("omnic_arc_e3");
    char txt[256];
    fill_text(txt, 200);
    wfile("omnic_arc_e3/only.txt", txt, 200);
    CHECK(pack_tree("omnic_arc_e3", ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    rm_tree("omnic_arc_e4");
    CHECK(unpack_arc(ARC, "omnic_arc_e4", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_e3", "omnic_arc_e4"));
    rm_tree("omnic_arc_e3");
    rm_tree("omnic_arc_e4");
    remove(ARC);
}

static void test_roundtrip_big(void) {
    /* >64 MiB 高熵单文件：skip→store，超限独立成块（AR-PKG-002 不撕裂） */
    omnic_fs_mkdir_p("omnic_arc_big");
    FILE *f = omnic_fs_fopen("omnic_arc_big/huge.bin", "wb");
    CHECK(f != NULL);
    static char buf[256 * 1024];
    for (size_t blk = 0; blk < (66u << 20) / sizeof(buf); blk++) {
        for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (char)rng();
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    CHECK(pack_tree("omnic_arc_big", ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    rm_tree("omnic_arc_big_out");
    CHECK(unpack_arc(ARC, "omnic_arc_big_out", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_big", "omnic_arc_big_out"));
    rm_tree("omnic_arc_big");
    rm_tree("omnic_arc_big_out");
    remove(ARC);
}

/* 少而大的 solid 形态：条目 <32 但写量 ≥8MiB，写盘池按字节门启用（原本只认
 * 条目数，Silesia 形态曾因此退化为串行写）。池化与串行解包均须与源树一致。 */
static void test_fewbig_pool(void) {
    const char *root = "omnic_arc_fewbig";
    rm_tree(root);
    omnic_fs_mkdir_p(root);
    static char buf[256 * 1024];
    for (int fi = 0; fi < 3; fi++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/big%d.txt", root, fi);
        FILE *f = omnic_fs_fopen(p, "wb");
        CHECK(f != NULL);
        if (f == NULL) return;
        for (size_t blk = 0; blk < (4u << 20) / sizeof(buf); blk++) {
            fill_text(buf, sizeof(buf));
            fwrite(buf, 1, sizeof(buf), f);
        }
        fclose(f);
    }
    CHECK(pack_tree(root, ARC, mkopts(OMNIC_F_QUIET, 2)) == OMNIC_OK);

    omnic_options o8 = mkopts(OMNIC_F_QUIET, 2);
    o8.threads = 8;
    rm_tree("omnic_arc_fewbig_p8");
    CHECK(unpack_arc(ARC, "omnic_arc_fewbig_p8", o8) == OMNIC_OK);
    CHECK(tree_equal(root, "omnic_arc_fewbig_p8"));

    omnic_options o1 = mkopts(OMNIC_F_QUIET, 2);
    o1.threads = 1;
    rm_tree("omnic_arc_fewbig_p1");
    CHECK(unpack_arc(ARC, "omnic_arc_fewbig_p1", o1) == OMNIC_OK);
    CHECK(tree_equal(root, "omnic_arc_fewbig_p1"));

    rm_tree("omnic_arc_fewbig_p8");
    rm_tree("omnic_arc_fewbig_p1");
    rm_tree(root);
    remove(ARC);
}

static void test_extract(void) {
    make_tree();
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);

    /* --file 单 */
    rm_tree("omnic_arc_x1");
    const char *names1[] = {"a.txt"};
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack_files(ARC, "omnic_arc_x1", names1, 1, &o, &g_rep) == OMNIC_OK);
    {
        FILE *h = omnic_fs_fopen("omnic_arc_x1/a.txt", "rb");
        CHECK(h != NULL);
        if (h != NULL) {
            char b[1024];
            size_t r = fread(b, 1, 200, h); /* 前 200B 非零即内容非空 */
            fclose(h);
            CHECK(r == 200);
        }
    }

    /* --file 多（同块合并） */
    rm_tree("omnic_arc_x2");
    const char *names2[] = {"a.txt", "b.txt", "twin1.txt"};
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack_files(ARC, "omnic_arc_x2", names2, 3, &o, &g_rep) == OMNIC_OK);
    CHECK(omnic_fs_fopen("omnic_arc_x2/b.txt", "rb") != NULL);

    /* --file 不存在名 → 用法错误 */
    const char *names3[] = {"no/such.txt"};
    rm_tree("omnic_arc_x3");
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack_files(ARC, "omnic_arc_x3", names3, 1, &o, &g_rep) == OMNIC_E_USAGE);

    /* T5：目标已存在未 -f 拒绝；-f 后成功 */
    const char *names4[] = {"a.txt"};
    rm_tree("omnic_arc_x4");
    omnic_fs_mkdir_p("omnic_arc_x4");
    wfile("omnic_arc_x4/a.txt", "old", 3);
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack_files(ARC, "omnic_arc_x4", names4, 1, &o, &g_rep) != OMNIC_OK);
    omnic_options of = mkopts(OMNIC_F_QUIET | OMNIC_F_FORCE, 0);
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack_files(ARC, "omnic_arc_x4", names4, 1, &of, &g_rep) == OMNIC_OK);

    /* --verify：完好归档全过 */
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_arc_verify(ARC, &o, &g_rep) == OMNIC_OK);
    CHECK(g_rep.out_bytes > 0);

    /* --list：stdout 捕获后断言行数（fd 级备份恢复，跨平台且 CI 无 tty 也安全） */
    {
        const char *lf = "omnic_arc_list.txt";
        int saved = dup(1);
        FILE *h = omnic_fs_fopen(lf, "wb");
        CHECK(h != NULL);
        if (h != NULL) {
            dup2(fileno(h), 1);
            /* 全零复合字面量的初始化会被 GCC 16.2 -O2/-O3 消除，static const 免疫 */
            static const omnic_options lo = {.profile = OMNIC_PROFILE_BALANCED,
                                             .codec = NULL, .level = 0, .threads = 0,
                                             .flags = 0u, .output_dir = NULL,
                                             .block_mib = 0};
            omnic_arc_list(ARC, &lo); /* 非 quiet 才有输出 */
            fflush(stdout);
            dup2(saved, 1);
            close(saved);
            fclose(h);
            h = omnic_fs_fopen(lf, "rb");
            CHECK(h != NULL);
            if (h != NULL) {
                char line[512];
                int nl = 0;
                while (fgets(line, sizeof(line), h) != NULL) nl++;
                fclose(h);
                CHECK(nl >= 8); /* 9 文件/目录条目 + 汇总行 */
            }
        }
        remove(lf);
    }

    rm_tree("omnic_arc_x1");
    rm_tree("omnic_arc_x2");
    rm_tree("omnic_arc_x3");
    rm_tree("omnic_arc_x4");
    remove(ARC);
    rm_tree(TREE);
}

static void test_corruption(void) {
    make_tree();
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);

    /* 块 CRC：Data 区末块翻转（首块是 store 桶——最大桶排最前）→ store 条目完好 + E_DATA */
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        if (pl != NULL) {
            free(pl);
            flip_byte(ARC, (long)io - 10); /* Data 尾（索引前）→ 最后一块 */
        }
    }
    rm_tree("omnic_arc_c1");
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    memset(&g_rep, 0, sizeof(g_rep));
    omnic_status st = omnic_unpack(ARC, "omnic_arc_c1", &o, &g_rep);
    CHECK(st == OMNIC_E_DATA); /* 坏块隔离：退出码 2 */
    /* 至少 store 块条目（rand.bin）完好解出（高熵文件必在 store 块） */
    CHECK(omnic_fs_fopen("omnic_arc_c1/rand.bin", "rb") != NULL);
    rm_tree("omnic_arc_c1");

    /* 索引 CRC：索引区翻字节 → 整体拒绝 */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        if (pl != NULL) {
            free(pl);
            flip_byte(ARC, (long)io + 2);
        }
    }
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_c2", &o, &g_rep) == OMNIC_E_DATA);

    /* 尾部截断（7/8 处）→ 拒绝 */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        long sz = file_size(ARC);
        CHECK(sz > 0);
        if (sz > 0) {
            FILE *h = omnic_fs_fopen(ARC, "rb");
            uint8_t *all = (uint8_t *)malloc((size_t)sz);
            fread(all, 1, (size_t)sz, h);
            fclose(h);
            h = omnic_fs_fopen(ARC, "wb");
            fwrite(all, 1, (size_t)(sz * 7 / 8), h);
            fclose(h);
            free(all);
        }
    }
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_c3", &o, &g_rep) == OMNIC_E_DATA);

    /* 条目 CRC（索引重写注入）：该条目失败、其余完好、汇总口径 */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        if (pl != NULL) {
            long eo = find_ent(pl, nblk, nent, "b.txt");
            CHECK(eo >= 0);
            if (eo >= 0) {
                uint32_t nl = (uint32_t)pl[eo] | ((uint32_t)pl[eo + 1] << 8);
                pl[eo + 2 + nl + 22] ^= 0xFF; /* 条目行内 crc 字段（+22） */
            }
            CHECK(save_index_plain(ARC, pl, n, io) == 0);
            free(pl);
        }
    }
    rm_tree("omnic_arc_c4");
    memset(&g_rep, 0, sizeof(g_rep));
    st = omnic_unpack(ARC, "omnic_arc_c4", &o, &g_rep);
    CHECK(st == OMNIC_E_DATA); /* 1 条目失败 */
    CHECK(omnic_fs_fopen("omnic_arc_c4/a.txt", "rb") != NULL); /* 其余完好 */
    rm_tree("omnic_arc_c4");

    remove(ARC);
    rm_tree(TREE);
    rm_tree("omnic_arc_c2");
    rm_tree("omnic_arc_c3");
}

static void test_malformed(void) {
    make_tree();
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);

    /* 头/尾 magic 破坏 */
    flip_byte(ARC, 1);
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_m1", &o, &g_rep) == OMNIC_E_DATA);
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    flip_byte(ARC, file_size(ARC) - 32); /* Footer 魔数首字节 */
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_m2", &o, &g_rep) == OMNIC_E_DATA);

    /* 头部的条目计数与索引内容对不上（v2 起计数以头部 32 位为准）：n_entries 最低位翻转 */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        FILE *f = omnic_fs_fopen(ARC, "r+b");
        CHECK(f != NULL);
        if (f != NULL) {
            fseek(f, 16, SEEK_SET);
            int c = fgetc(f);
            fseek(f, 16, SEEK_SET);
            fputc(c ^ 0x01, f); /* 计数 ±1：多了读不够，少了索引读不完 */
            fclose(f);
        }
    }
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_m3", &o, &g_rep) == OMNIC_E_DATA);

    /* 索引重写注入：../ 穿越 / name_len 巨值 / 重复名 */
    struct {
        const char *name;
        int mode; /* 0=escape, 1=huge len, 2=dup */
    } inj[] = {{"escape", 0}, {"huge", 1}, {"dup", 2}};
    for (size_t c = 0; c < 3; c++) {
        CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        if (pl != NULL) {
            long eo = find_ent(pl, nblk, nent, "b.txt");
            CHECK(eo >= 0);
            if (inj[c].mode == 0) {
                if (eo >= 0) memcpy(pl + eo + 2, "../xx", 5); /* 同长覆盖成逃逸路径 */
            } else if (inj[c].mode == 1) {
                if (eo >= 0) {
                    pl[eo] = 0xFF; /* name_len=65535 溢出 */
                    pl[eo + 1] = 0xFF;
                }
            } else {
                if (eo >= 0) memcpy(pl + eo + 2, "a.txt", 5); /* b.txt 名改成 a.txt */
            }
            CHECK(save_index_plain(ARC, pl, n, io) == 0);
            free(pl);
        }
        char out[64];
        snprintf(out, sizeof(out), "omnic_arc_m_%s", inj[c].name);
        memset(&g_rep, 0, sizeof(g_rep));
        omnic_status st = omnic_unpack(ARC, out, &o, &g_rep);
        CHECK(st == OMNIC_E_DATA); /* 全部拒绝不崩 */
        rm_tree(out);
    }

    remove(ARC);
    rm_tree(TREE);
    rm_tree("omnic_arc_m1");
    rm_tree("omnic_arc_m2");
    rm_tree("omnic_arc_m3");
}

static void test_bounds(void) {
    make_tree();

    /* T1：输出在源树内 → 拒绝 */
    char inside[512];
    snprintf(inside, sizeof(inside), "%s/inside.omnic", TREE);
    omnic_status st = pack_tree(TREE, inside, mkopts(OMNIC_F_QUIET, 0));
    CHECK(st == OMNIC_E_USAGE);
    remove(inside);

    /* --block-size=1 MiB：solid 切多块，roundtrip 仍全等（块边界 1..1024） */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 1)) == OMNIC_OK);
    rm_tree("omnic_arc_bs1");
    CHECK(unpack_arc(ARC, "omnic_arc_bs1", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(tree_equal(TREE, "omnic_arc_bs1"));
    rm_tree("omnic_arc_bs1");

    remove(ARC);
    rm_tree(TREE);
}

/* ---- v0.3.1 并行写盘（DESIGN-parallel-unpack）---- */
static const char *TREE2 = "omnic_arc_tree2";
static const char *ARC2 = "omnic_arc_t2.omnic";

static void make_tree_big(void) {
    rm_tree(TREE2);
    omnic_fs_mkdir_p(TREE2);
    static char buf[8 * 1024];
    char p[512];
    for (int d = 0; d < 3; d++) { /* 子目录先建：wfile 不建父目录 */
        snprintf(p, sizeof(p), "%s/d%d", TREE2, d);
        omnic_fs_mkdir_p(p);
    }
    /* 120 文件跨 3 目录：超 ARC_PAR_MIN_JOBS(32) 强制并行路径 */
    for (int i = 0; i < 120; i++) {
        size_t n = 64 + (size_t)(i % 7) * 512;
        fill_text(buf, n);
        snprintf(p, sizeof(p), "%s/d%d/f%03d.txt", TREE2, i % 3, i);
        wfile(p, buf, n);
    }
    snprintf(p, sizeof(p), "%s/empty.bin", TREE2); /* 空文件 */
    wfile(p, "", 0);
    { /* 32KB 高熵（同 make_tree 口径：熵采样下必入 store 块） */
        static char rbuf[32 * 1024];
        for (size_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = (char)rng();
        snprintf(p, sizeof(p), "%s/rand.bin", TREE2);
        wfile(p, rbuf, sizeof(rbuf));
    }
}

static void test_parallel(void) {
    make_tree_big();
    CHECK(pack_tree(TREE2, ARC2, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);

    omnic_options o8 = mkopts(OMNIC_F_QUIET, 0);
    o8.threads = 8;
    omnic_options o1 = mkopts(OMNIC_F_QUIET, 0);
    o1.threads = 1;

    /* P1：-T8 与 -T1 树等价（文件集/内容/mtime），且与源树一致 */
    rm_tree("omnic_arc_p8");
    rm_tree("omnic_arc_p1");
    CHECK(unpack_arc(ARC2, "omnic_arc_p8", o8) == OMNIC_OK);
    CHECK(unpack_arc(ARC2, "omnic_arc_p1", o1) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_p8", "omnic_arc_p1"));
    CHECK(tree_equal("omnic_arc_p8", TREE2));
    rm_tree("omnic_arc_p8");
    rm_tree("omnic_arc_p1");

    /* P2：默认 threads=0 → 8 writers，等价 */
    rm_tree("omnic_arc_pd");
    CHECK(unpack_arc(ARC2, "omnic_arc_pd", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_pd", TREE2));
    rm_tree("omnic_arc_pd");

    /* P3：坏块 × 并行：数据区首块（offset 0）翻转 → 该块文件成员全灭、其他块
     * 成员存活、退出码 2（隔离语义）。块归属从索引直读——不猜分类（LCG 低
     * 字节"随机"数据实测入压缩桶，只有 0 字节文件按规则必入 store 桶）。 */
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC2, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        char in_b0[512], out_b0[512];
        int got_in = 0, got_out = 0;
        long flip_off = -1;
        if (pl != NULL) {
            size_t off = (size_t)nblk * T_BLK_ROW;
            for (uint32_t i = 0; i < nent && !(got_in && got_out); i++) {
                uint32_t nl = (uint32_t)pl[off] | ((uint32_t)pl[off + 1] << 8);
                if (pl[off + 2 + nl] == 0) { /* 文件条目 */
                    uint32_t bid = (uint32_t)pl[off + 4 + nl] | ((uint32_t)pl[off + 5 + nl] << 8);
                    if (bid == 0 && !got_in) {
                        memcpy(in_b0, pl + off + 2, nl);
                        in_b0[nl] = '\0';
                        got_in = 1;
                    } else if (bid != 0 && !got_out) {
                        memcpy(out_b0, pl + off + 2, nl);
                        out_b0[nl] = '\0';
                        got_out = 1;
                    }
                }
                off += 2 + nl + ent_row_len(pl + off + 2 + nl);
            }
            /* 块 0 行：fmt@0 comp@12 raw@20；存储域 = store?raw:comp，翻其中点 */
            uint64_t comp = 0, raw = 0;
            for (int b = 0; b < 8; b++) comp |= (uint64_t)pl[b + 12] << (8 * b);
            for (int b = 0; b < 8; b++) raw |= (uint64_t)pl[b + 20] << (8 * b);
            uint64_t stored = (pl[0] == 0) ? raw : comp;
            if (stored > 0) flip_off = (long)(28 + stored / 2); /* 28 = ARC_HDR */
            free(pl);
        }
        CHECK(got_in);
        if (nblk > 1) CHECK(got_out);
        CHECK(flip_off > 0);
        if (flip_off > 0) flip_byte(ARC2, flip_off);
        rm_tree("omnic_arc_pc");
        CHECK(unpack_arc(ARC2, "omnic_arc_pc", o8) == OMNIC_E_DATA);
        if (got_in) {
            char dst[600];
            snprintf(dst, sizeof(dst), "omnic_arc_pc/%s", in_b0);
            CHECK(omnic_fs_fopen(dst, "rb") == NULL); /* 坏块成员缺失 */
        }
        if (nblk > 1 && got_out) {
            char dst[600];
            snprintf(dst, sizeof(dst), "omnic_arc_pc/%s", out_b0);
            CHECK(omnic_fs_fopen(dst, "rb") != NULL); /* 好块成员存活 */
        }
        rm_tree("omnic_arc_pc");
    }

    /* P4：条目 CRC 注入 × 并行：单条目失败、其余完好 */
    CHECK(pack_tree(TREE2, ARC2, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(ARC2, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        if (pl != NULL) {
            long eo = find_ent(pl, nblk, nent, "d0/f000.txt");
            CHECK(eo >= 0);
            if (eo >= 0) {
                uint32_t nl = (uint32_t)pl[eo] | ((uint32_t)pl[eo + 1] << 8);
                pl[eo + 2 + nl + 22] ^= 0xFF; /* 条目行内 crc 字段 */
            }
            CHECK(save_index_plain(ARC2, pl, n, io) == 0);
            free(pl);
        }
    }
    rm_tree("omnic_arc_pe");
    CHECK(unpack_arc(ARC2, "omnic_arc_pe", o8) == OMNIC_E_DATA);
    CHECK(omnic_fs_fopen("omnic_arc_pe/d1/f001.txt", "rb") != NULL);
    CHECK(omnic_fs_fopen("omnic_arc_pe/rand.bin", "rb") != NULL);
    rm_tree("omnic_arc_pe");

    /* P5：--file 批量(≥32) 走并行路径，批量抽取正确（需干净归档：P4 有注入） */
    CHECK(pack_tree(TREE2, ARC2, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        static char nbuf[40][16];
        static const char *names[40];
        for (int i = 0; i < 40; i++) {
            snprintf(nbuf[i], sizeof(nbuf[i]), "d%d/f%03d.txt", i % 3, i);
            names[i] = nbuf[i];
        }
        rm_tree("omnic_arc_pf");
        omnic_options of = mkopts(OMNIC_F_QUIET, 0);
        of.threads = 8;
        memset(&g_rep, 0, sizeof(g_rep));
        CHECK(omnic_unpack_files(ARC2, "omnic_arc_pf", names, 40, &of, &g_rep) == OMNIC_OK);
        CHECK(omnic_fs_fopen("omnic_arc_pf/d0/f000.txt", "rb") != NULL);
        CHECK(omnic_fs_fopen("omnic_arc_pf/d0/f039.txt", "rb") != NULL);
        CHECK(omnic_fs_fopen("omnic_arc_pf/d0/f001.txt", "rb") == NULL); /* 非目标不落盘 */
        CHECK(omnic_fs_fopen("omnic_arc_pf/rand.bin", "rb") == NULL);
        rm_tree("omnic_arc_pf");
    }

    remove(ARC2);
    rm_tree(TREE2);
}

/* ---- T13 pack 确定性：分类池串行/并行产物逐字节相同（README 承诺的不变量）。
 * 桶压缩不吃 threads，两档唯一差异是分类路径；121 文件超 ARC_PAR_MIN_JOBS(32)
 * 强制并行。 ---- */
static void test_pack_determinism(void) {
    make_tree_big();

    omnic_options o1 = mkopts(OMNIC_F_QUIET, 0);
    o1.threads = 1;
    CHECK(pack_tree(TREE2, "omnic_arc_det1.omnic", o1) == OMNIC_OK);

    omnic_options o8 = mkopts(OMNIC_F_QUIET, 0);
    o8.threads = 8;
    CHECK(pack_tree(TREE2, "omnic_arc_det8.omnic", o8) == OMNIC_OK);

    CHECK(files_equal("omnic_arc_det1.omnic", "omnic_arc_det8.omnic"));

    rm_tree(TREE2);
    remove("omnic_arc_det1.omnic");
    remove("omnic_arc_det8.omnic");
}

/* ---- T12 块压缩池：多块乱序/回执环复用/probe 双候选/store 排水的确定性门禁。
 * 树形：96×128KiB 文本（best_ratio→xz 桶 14 块）+ font.ttf 6MiB 中熵
 * （best_ratio 计划 zstd/19 → 桶首块 probe 对 xz/9；单文件超限独立成块）+ rand/empty（store）。
 * block_mib=1 → 池序号 0..15（14 块 + probe + 1 字体块）：环 cap=9 下 probe 落在
 * seq 14（槽 5 复用点），窗口回收、乱序完成、跨桶重叠、store 前排水、探针窥视
 * 全部在途。两 profile 各自 -T1/-T8 逐字节相同 + -T8 归档 roundtrip 等价。 ---- */
static void test_pack_parallel_blocks(void) {
    static const char *T3 = "omnic_arc_tree3";
    rm_tree(T3);
    omnic_fs_mkdir_p(T3);
    static char buf[128 * 1024];
    char p[512];
    for (int i = 0; i < 96; i++) {
        fill_text(buf, sizeof(buf));
        snprintf(p, sizeof(p), "%s/t%02d.txt", T3, i % 100);
        wfile(p, buf, sizeof(buf));
    }
    { /* 6MiB 中熵：best_ratio 计划 zstd/19（≥4MiB 触发 probe），balanced 走 zstd/9 */
        static char mbuf[6 * 1024 * 1024];
        for (size_t i = 0; i < sizeof(mbuf); i++) mbuf[i] = (char)(rng() % 47);
        snprintf(p, sizeof(p), "%s/font.ttf", T3);
        wfile(p, mbuf, sizeof(mbuf));
    }
    { /* store 桶（与压缩桶混排，盖住 store 前排水路径） */
        static char rbuf[64 * 1024];
        for (size_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = (char)rng();
        snprintf(p, sizeof(p), "%s/rand.bin", T3);
        wfile(p, rbuf, sizeof(rbuf));
        snprintf(p, sizeof(p), "%s/empty.bin", T3);
        wfile(p, "", 0);
    }

    for (int best = 0; best <= 1; best++) {
        omnic_options o1 = mkopts(OMNIC_F_QUIET, 1);
        o1.threads = 1;
        o1.profile = best ? OMNIC_PROFILE_BEST_RATIO : OMNIC_PROFILE_BALANCED;
        omnic_options o8 = o1;
        o8.threads = 8;
        const char *a1 = best ? "omnic_arc_pb_b1.omnic" : "omnic_arc_pb_l1.omnic";
        const char *a8 = best ? "omnic_arc_pb_b8.omnic" : "omnic_arc_pb_l8.omnic";
        CHECK(pack_tree(T3, a1, o1) == OMNIC_OK);
        CHECK(pack_tree(T3, a8, o8) == OMNIC_OK);
        CHECK(files_equal(a1, a8)); /* 池保序回执：-T1 ≡ -T8 逐字节 */
        rm_tree("omnic_arc_pb_rt");
        CHECK(unpack_arc(a8, "omnic_arc_pb_rt", o8) == OMNIC_OK);
        CHECK(tree_equal("omnic_arc_pb_rt", T3));
        rm_tree("omnic_arc_pb_rt");
        remove(a1);
        remove(a8);
    }
    rm_tree(T3);
}

/* B 门禁：解码池保序回执（每文件一块 × 66 块跨 8 worker）。回执错位会把 A 块
 * 的原始字节配给 B 块的条目——条目 CRC 即红、树即不等。中段坏块在池路径下
 * 验证隔离语义（受击成员缺失、前后邻完好、退出码 2）；verify 的 -T8/-T1
 * out_bytes 口径一致。 */
static void test_decode_pool(void) {
    static const char *T4 = "omnic_arc_tree4";
    static const char *A4 = "omnic_arc_dp.omnic";
    rm_tree(T4);
    omnic_fs_mkdir_p(T4);
    static char buf[64 * 1024];
    char p[512];
    for (int i = 0; i < 64; i++) {
        fill_text(buf, sizeof(buf));
        buf[0] = (char)('a' + i % 26); /* 内容随块序可区分：块交叉必炸 CRC */
        buf[1] = (char)('A' + i / 26);
        snprintf(p, sizeof(p), "%s/dp%03d.txt", T4, i);
        wfile(p, buf, sizeof(buf));
    }
    { /* 高熵 + 空：store/零尺寸块混进解码流（decode_block 全返回码路径） */
        static char rbuf[64 * 1024];
        for (size_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = (char)rng();
        snprintf(p, sizeof(p), "%s/rand.bin", T4);
        wfile(p, rbuf, sizeof(rbuf));
        snprintf(p, sizeof(p), "%s/empty.bin", T4);
        wfile(p, "", 0);
    }

    omnic_options o8 = mkopts(OMNIC_F_QUIET | OMNIC_F_PER_FILE, 0);
    o8.threads = 8;
    omnic_options o1 = o8;
    o1.threads = 1;
    CHECK(pack_tree(T4, A4, o8) == OMNIC_OK);

    rm_tree("omnic_arc_dp8");
    CHECK(unpack_arc(A4, "omnic_arc_dp8", o8) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_dp8", T4));
    rm_tree("omnic_arc_dp1");
    CHECK(unpack_arc(A4, "omnic_arc_dp1", o1) == OMNIC_OK);
    CHECK(tree_equal("omnic_arc_dp1", T4));
    rm_tree("omnic_arc_dp8");
    rm_tree("omnic_arc_dp1");

    /* verify 池路径：-T8 与 -T1 校验字节口径一致 */
    omnic_report r8, r1;
    memset(&r8, 0, sizeof(r8));
    memset(&r1, 0, sizeof(r1));
    CHECK(omnic_arc_verify(A4, &o8, &r8) == OMNIC_OK);
    CHECK(omnic_arc_verify(A4, &o1, &r1) == OMNIC_OK);
    CHECK(r8.out_bytes == r1.out_bytes);
    CHECK(r8.out_bytes > 0);

    /* 中段坏块 × 池：从索引取中段块行（offset@4/comp@12/raw@20），翻转存储
     * 域中点；受击块的成员缺失，前后邻块成员存活（隔离语义在保序收割下不变） */
    {
        size_t n;
        uint64_t io;
        uint32_t nent, nblk;
        uint8_t *pl = load_index_plain(A4, &n, &io, &nent, &nblk);
        CHECK(pl != NULL);
        long flip_off = -1;
        uint32_t hit = 0, pre = 0, post = 0;
        char in_name[512] = {0}, pre_name[512] = {0}, post_name[512] = {0};
        int got_in = 0, got_pre = 0, got_post = 0;
        if (pl != NULL) {
            CHECK(nblk >= 8); /* ≥66 块：解码池必开 */
            /* 中段起找首个存储域 >4 的块（跳过零尺寸空块），前后邻取成员名 */
            for (uint32_t cand = nblk / 2; cand + 1 < nblk; cand++) {
                const uint8_t *row = pl + (size_t)cand * T_BLK_ROW;
                uint64_t boff = 0, comp = 0, raw = 0;
                for (int b = 0; b < 8; b++) boff |= (uint64_t)row[b + 4] << (8 * b);
                for (int b = 0; b < 8; b++) comp |= (uint64_t)row[b + 12] << (8 * b);
                for (int b = 0; b < 8; b++) raw |= (uint64_t)row[b + 20] << (8 * b);
                uint64_t stored = (row[0] == 0) ? raw : comp;
                if (stored <= 4) continue;
                flip_off = (long)(28 + boff + stored / 2);
                hit = cand;
                pre = cand - 1;
                post = cand + 1;
                break;
            }
            if (flip_off > 0) {
                size_t off = (size_t)nblk * T_BLK_ROW;
                for (uint32_t i = 0; i < nent && !(got_in && got_pre && got_post); i++) {
                    uint32_t nl = (uint32_t)pl[off] | ((uint32_t)pl[off + 1] << 8);
                    if (pl[off + 2 + nl] == 0) { /* 文件条目 */
                        uint32_t bid =
                            (uint32_t)pl[off + 4 + nl] | ((uint32_t)pl[off + 5 + nl] << 8);
                        char *dst = NULL;
                        int want = 0;
                        if (bid == hit && !got_in) {
                            dst = in_name;
                            want = got_in = 1;
                        } else if (bid == pre && !got_pre) {
                            dst = pre_name;
                            want = got_pre = 1;
                        } else if (bid == post && !got_post) {
                            dst = post_name;
                            want = got_post = 1;
                        }
                        if (want) {
                            memcpy(dst, pl + off + 2, nl);
                            dst[nl] = '\0';
                        }
                    }
                    off += 2 + nl + ent_row_len(pl + off + 2 + nl);
                }
                CHECK(got_in);
                CHECK(got_pre);
                CHECK(got_post);
            }
            free(pl);
        }
        CHECK(flip_off > 0);
        if (flip_off > 0) {
            flip_byte(A4, flip_off);
            rm_tree("omnic_arc_dpc");
            CHECK(unpack_arc(A4, "omnic_arc_dpc", o8) == OMNIC_E_DATA);
            char dst[600];
            snprintf(dst, sizeof(dst), "omnic_arc_dpc/%s", in_name);
            CHECK(omnic_fs_fopen(dst, "rb") == NULL); /* 受击成员缺失 */
            snprintf(dst, sizeof(dst), "omnic_arc_dpc/%s", pre_name);
            CHECK(omnic_fs_fopen(dst, "rb") != NULL); /* 前邻存活 */
            snprintf(dst, sizeof(dst), "omnic_arc_dpc/%s", post_name);
            CHECK(omnic_fs_fopen(dst, "rb") != NULL); /* 后邻存活 */
            rm_tree("omnic_arc_dpc");
        }
    }
    remove(A4);
    rm_tree(T4);
}

/* ---- 失败的 pack 不毁旧归档 / 跳过条目上报 / 深路径 / 目录建不出来不中止 ---- */

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = omnic_fs_fopen(path, "rb");
    if (f == NULL) return NULL;
    long sz = -1;
    unsigned char *b = NULL;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz >= 0) {
        rewind(f);
        b = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
        if (b != NULL && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    if (b != NULL) *n = (size_t)sz;
    return b;
}

/* 让 path 读不了（pack 中途失败的注入）。返回 1=已生效，0=本环境做不到（如 root）。 */
static int deny_read(const char *path, int on) {
#ifdef _WIN32
    static HANDLE held = INVALID_HANDLE_VALUE;
    if (on) {
        wchar_t w[2048];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048);
        held = CreateFileW(w, GENERIC_READ, 0 /* 独占：谁也打不开 */, NULL, OPEN_EXISTING, 0, NULL);
        return held != INVALID_HANDLE_VALUE;
    }
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    held = INVALID_HANDLE_VALUE;
    return 1;
#else
    static mode_t orig = (mode_t)-1;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (on) {
        orig = st.st_mode & 07777;
        if (chmod(path, 0) != 0) return 0;
        if (access(path, R_OK) == 0) { /* root：读得了，注入无效 */
            chmod(path, orig);
            orig = (mode_t)-1;
            return 0;
        }
        return 1;
    }
    return orig == (mode_t)-1 || chmod(path, orig) == 0;
#endif
}

/* 一条链接类条目：Windows 用目录联接（不需特权），POSIX 用符号链接。返回 1=建成了 */
static int make_link(const char *tree) {
#ifdef _WIN32
    char cwd[1024], cmd[4096];
    if (_getcwd(cwd, sizeof(cwd)) == NULL) return 0;
    snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\\%s\\link\" \"%s\\%s\\real\" >nul 2>&1", cwd, tree,
             cwd, tree);
    return system(cmd) == 0;
#else
    char link[1024];
    snprintf(link, sizeof(link), "%s/link", tree);
    return symlink("real", link) == 0;
#endif
}

/* 一条特殊文件（命名管道）。Windows 造不出，返回 0 */
static int make_fifo(const char *tree) {
#ifndef _WIN32
    char p[1024];
    snprintf(p, sizeof(p), "%s/pipe", tree);
    return mkfifo(p, 0600) == 0;
#else
    (void)tree;
    return 0;
#endif
}

/* rm -rf / rd /s 处理不了超过 260 字符的路径：用 omnic 自己的接口删，先子后父 */
static void rm_tree_deep(const char *root) {
    size_t n = 0;
    omnic_fs_skip_report sk;
    omnic_fs_entry *v = omnic_fs_walk_report(root, &n, &sk);
    if (v != NULL) {
        for (size_t k = n; k-- > 0;) { /* walk 先父后子：倒序即先子后父 */
            char p[OMNIC_FS_PATH_MAX + 1100]; /* root + '/' + rel(rel 最长 4095) */
            snprintf(p, sizeof(p), "%s/%s", root, v[k].rel);
            if (v[k].is_dir)
                omnic_fs_rmdir(p);
            else
                omnic_fs_remove_force(p);
        }
        omnic_fs_entries_free(v, n);
    }
    omnic_fs_rmdir(root);
}

/* 存在性检查（打开即关：留着的句柄会让随后的清理删不掉文件） */
static int file_exists(const char *path) {
    FILE *f = omnic_fs_fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

static void test_pack_atomic_publish(void) {
    const char *T = "omnic_arc_ap", *A = "omnic_arc_ap.omnic";
    rm_tree(T);
    omnic_fs_mkdir_p(T);
    char txt[256];
    fill_text(txt, 200);
    wfile("omnic_arc_ap/a.txt", txt, 200);
    wfile("omnic_arc_ap/b.txt", txt, 200);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);

    size_t good_n = 0;
    unsigned char *good = slurp(A, &good_n);
    CHECK(good != NULL && good_n > 0);

    /* pack 中途失败：输出路径上必须还是上一份好归档，且不留临时文件 */
    if (deny_read("omnic_arc_ap/b.txt", 1)) {
        CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) != OMNIC_OK);
        size_t now_n = 0;
        unsigned char *now = slurp(A, &now_n);
        CHECK(now != NULL && now_n == good_n && (good == NULL || memcmp(now, good, good_n) == 0));
        free(now);
        CHECK(file_exists("omnic_arc_ap.omnic.tmp") == 0);
        deny_read("omnic_arc_ap/b.txt", 0);
    } else {
        printf("note: cannot make a file unreadable here; failure-injection checks skipped\n");
    }

    /* 恢复后重打包：新归档整体替换旧的 */
    fill_text(txt, 120);
    wfile("omnic_arc_ap/a.txt", txt, 120);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(file_exists("omnic_arc_ap.omnic.tmp") == 0);
    rm_tree("omnic_arc_ap_out");
    CHECK(unpack_arc(A, "omnic_arc_ap_out", mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(files_equal("omnic_arc_ap/a.txt", "omnic_arc_ap_out/a.txt"));
    CHECK(files_equal("omnic_arc_ap/b.txt", "omnic_arc_ap_out/b.txt"));
    free(good);
    rm_tree(T);
    rm_tree("omnic_arc_ap_out");
    remove(A);
}

static void test_pack_reports_skipped(void) {
    const char *T = "omnic_arc_sk", *A = "omnic_arc_sk.omnic";
    rm_tree(T);
    omnic_fs_mkdir_p("omnic_arc_sk/real");
    char txt[256];
    fill_text(txt, 100);
    wfile("omnic_arc_sk/a.txt", txt, 100);
    wfile("omnic_arc_sk/real/t.txt", txt, 100);
    int link_ok = make_link(T), fifo_ok = make_fifo(T);
    printf("note: link entry created: %d, fifo created: %d\n", link_ok, fifo_ok);

    size_t n = 0;
    omnic_fs_skip_report sk;
    omnic_fs_entry *v = omnic_fs_walk_report(T, &n, &sk);
    CHECK(v != NULL && n == 3 + (size_t)link_ok); /* 链接收录；只有特殊文件不收录 */
    CHECK(sk.total == (size_t)fifo_ok);
    CHECK(sk.special == (size_t)fifo_ok);
    CHECK(sk.n_first == sk.total);
    free(v);

    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    CHECK(pack_tree(T, A, o) == OMNIC_OK); /* 默认：照常打包，跳过的如实上报 */
    CHECK(omnic_arc_verify(A, &o, &g_rep) == OMNIC_OK);
    remove(A);
    omnic_options strict = mkopts(OMNIC_F_QUIET | OMNIC_F_STRICT, 0);
    CHECK(pack_tree(T, A, strict) == (fifo_ok ? OMNIC_E_DATA : OMNIC_OK)); /* 有跳过才失败 */
    if (fifo_ok) {
        CHECK(file_exists(A) == 0); /* 且不留下任何输出 */
        CHECK(file_exists("omnic_arc_sk.omnic.tmp") == 0);
    } else {
        remove(A);
    }
    /* 没有被跳过条目的树，--strict 不受影响 */
    CHECK(pack_tree("omnic_arc_sk/real", A, strict) == OMNIC_OK);
    remove(A);
#ifdef _WIN32
    omnic_fs_rmdir("omnic_arc_sk/link"); /* 只删联接本身，不动目标 */
#else
    omnic_fs_remove("omnic_arc_sk/link");
    omnic_fs_remove("omnic_arc_sk/pipe");
#endif
    rm_tree(T);
}

static void test_long_path_roundtrip(void) {
    /* 路径总长超过 MAX_PATH(260)：Windows 上不带 \\?\ 前缀的 Win32 调用到这里就失效 */
    const char *T = "omnic_arc_lp", *O = "omnic_arc_lp_out", *A = "omnic_arc_lp.omnic";
    static char deep[OMNIC_FS_PATH_MAX], out[OMNIC_FS_PATH_MAX + 1100]; /* O + deep 去掉前缀 */
    rm_tree_deep(T);
    rm_tree_deep(O);
    snprintf(deep, sizeof(deep), "%s", T);
    for (int i = 0; i < 14; i++)
        strncat(deep, "/lvl_xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(deep) - strlen(deep) - 1);
    omnic_fs_mkdir_p(deep);
    char txt[64];
    fill_text(txt, 40);
    strncat(deep, "/deep.txt", sizeof(deep) - strlen(deep) - 1);
    wfile(deep, txt, 40);
    CHECK(strlen(deep) > 300);

    size_t n = 0, sk = 0;
    omnic_fs_entry *v = omnic_fs_walk(T, &n, &sk);
    CHECK(v != NULL && n == 15 && sk == 0); /* 14 级目录 + 1 个文件，一个不落 */
    free(v);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(unpack_arc(A, O, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    snprintf(out, sizeof(out), "%s%s", O, deep + strlen(T));
    CHECK(files_equal(deep, out));
    rm_tree_deep(T);
    rm_tree_deep(O);
    remove(A);
}

static void test_unpack_dir_failure_continues(void) {
    const char *T = "omnic_arc_df", *O = "omnic_arc_df_out", *A = "omnic_arc_df.omnic";
    rm_tree(T);
    rm_tree(O);
    omnic_fs_mkdir_p("omnic_arc_df/sub/inner");
    char txt[256];
    fill_text(txt, 100);
    wfile("omnic_arc_df/a.txt", txt, 100);
    wfile("omnic_arc_df/sub/inner/x.txt", txt, 100);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    /* 输出位置上 sub 是个普通文件：目录 sub/inner 建不出来 */
    omnic_fs_mkdir_p(O);
    wfile("omnic_arc_df_out/sub", "x", 1);
    CHECK(unpack_arc(A, O, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_E_DATA);
    CHECK(file_exists("omnic_arc_df_out/a.txt"));     /* 其余条目照常解出 */
    CHECK(file_exists("omnic_arc_df_out/sub/inner/x.txt") == 0);
    rm_tree(T);
    rm_tree(O);
    remove(A);
}

/* 设置目录的 mtime（测试用；解包代码自己不再对目录设时间） */
static void set_dir_time(const char *path, long long t) {
#ifdef _WIN32
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)t * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, &ft, &ft);
    CloseHandle(h);
#else
    struct utimbuf tb;
    tb.actime = (time_t)t;
    tb.modtime = (time_t)t;
    utime(path, &tb);
#endif
}

/* 目录的修改时间一律不恢复（往目录里写文件会刷新它，只有空目录保得住——半保半丢不如都不保） */
static void test_dir_mtime_not_restored(void) {
    const char *T = "omnic_arc_dm", *O = "omnic_arc_dm_out", *A = "omnic_arc_dm.omnic";
    rm_tree(T);
    rm_tree(O);
    omnic_fs_mkdir_p("omnic_arc_dm/d");
    omnic_fs_mkdir_p("omnic_arc_dm/empty");
    char txt[64];
    fill_text(txt, 30);
    wfile("omnic_arc_dm/d/x.txt", txt, 30);
    set_dir_time("omnic_arc_dm/d", 1000000000LL); /* 2001-09-09 */
    set_dir_time("omnic_arc_dm/empty", 1000000000LL);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(unpack_arc(A, O, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    long long now = (long long)time(NULL), td = 0, te = 0;
    size_t n = 0;
    omnic_fs_skip_report sk;
    omnic_fs_entry *v = omnic_fs_walk_report(O, &n, &sk);
    if (v != NULL)
        for (size_t i = 0; i < n; i++) {
            if (strcmp(v[i].rel, "d") == 0) td = v[i].mtime;
            if (strcmp(v[i].rel, "empty") == 0) te = v[i].mtime;
        }
    free(v);
    CHECK(td > now - 3600); /* 两个目录都是解包时刻 */
    CHECK(te > now - 3600);
    rm_tree(T);
    rm_tree(O);
    remove(A);
}

/* ---- v2：链接 / 硬链接 / 权限与属性 / 创建时间 / 32 位计数 / v1 归档可读 / 畸形输入不崩 ---- */

/* 取一个路径对应条目的元数据：借遍历接口读父目录（跨平台，不另写系统调用） */
typedef struct {
    int is_dir, link_kind, has_mode;
    char target[1024];
    unsigned attrs, mode;
    long long mtime, ctime;
    unsigned long long size;
} t_meta;

static int stat_meta(const char *path, t_meta *m, unsigned want) {
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s", path);
    char *base = strrchr(dir, '/');
    if (base == NULL) return -1;
    *base++ = '\0';
    size_t n = 0;
    omnic_fs_skip_report sk;
    omnic_fs_entry *v = omnic_fs_walk_ex(dir, &n, &sk, want);
    int found = 0;
    memset(m, 0, sizeof(*m));
    if (v != NULL)
        for (size_t i = 0; i < n && !found; i++) {
            if (strcmp(v[i].rel, base) != 0) continue;
            m->is_dir = v[i].is_dir;
            m->link_kind = v[i].link_kind;
            if (v[i].link_target != NULL) snprintf(m->target, sizeof(m->target), "%s", v[i].link_target);
            m->attrs = v[i].attrs;
            m->mode = v[i].mode;
            m->has_mode = v[i].has_mode;
            m->mtime = v[i].mtime;
            m->ctime = v[i].ctime;
            m->size = v[i].size;
            found = 1;
        }
    omnic_fs_entries_free(v, n);
    return found ? 0 : -1;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 文件/目录的创建时间（测试用；仅 Windows 有可移植的设置接口） */
static void set_creation_time(const char *path, long long t) {
#ifdef _WIN32
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)t * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, &ft, NULL, NULL);
    CloseHandle(h);
#else
    (void)path;
    (void)t;
#endif
}

static void test_v2_links(void) {
    const char *T = "omnic_arc_lk", *O = "omnic_arc_lk_out", *A = "omnic_arc_lk.omnic";
    const char *X = "omnic_arc_lk_x";
    rm_tree(T);
    rm_tree(O);
    rm_tree(X);
    omnic_fs_mkdir_p("omnic_arc_lk/sub");
    char txt[256];
    fill_text(txt, 200);
    wfile("omnic_arc_lk/a.txt", txt, 200);
    wfile("omnic_arc_lk/sub/x.txt", txt, 150);
    int hl_ok = (omnic_fs_make_hardlink("omnic_arc_lk/a.txt", "omnic_arc_lk/hard.txt") == OMNIC_OK);
    /* 目录链接：Windows 用目录联接（不需特权，目标须为绝对路径），POSIX 用符号链接。
     * 文件符号链接两边都尽力建：Windows 普通权限下建不了，相关检查随之略过。 */
    int jn_ok = 0, sl_ok = 0;
#ifdef _WIN32
    char cwd[1024], tgt[2048];
    if (_getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(tgt, sizeof(tgt), "%s\\%s\\sub", cwd, T);
        jn_ok = (omnic_fs_make_junction("omnic_arc_lk/dlink", tgt) == OMNIC_OK);
    }
    int unpriv = 0;
    sl_ok = (omnic_fs_make_symlink("omnic_arc_lk/slink", "a.txt", 0, &unpriv) == OMNIC_OK);
#else
    jn_ok = (omnic_fs_make_symlink("omnic_arc_lk/dlink", "sub", 1, NULL) == OMNIC_OK);
    sl_ok = (omnic_fs_make_symlink("omnic_arc_lk/slink", "a.txt", 0, NULL) == OMNIC_OK);
#endif
    printf("note: hard link: %d, directory link: %d, file symlink: %d\n", hl_ok, jn_ok, sl_ok);
    CHECK(hl_ok == 1);
    CHECK(jn_ok == 1);

    size_t n = 0;
    omnic_fs_skip_report sk;
    omnic_fs_entry *v = omnic_fs_walk_report(T, &n, &sk);
    CHECK(v != NULL && sk.total == 0);             /* 链接是条目，不再是"跳过" */
    CHECK(n == 4 + (size_t)jn_ok + (size_t)sl_ok); /* a.txt, sub, sub/x.txt, hard.txt + 链接 */
    omnic_fs_entries_free(v, n);

    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    CHECK(pack_tree(T, A, o) == OMNIC_OK);
    CHECK(omnic_arc_verify(A, &o, &g_rep) == OMNIC_OK);
    CHECK(unpack_arc(A, O, o) == OMNIC_OK);
    CHECK(tree_equal(T, O)); /* 内容、链接种类与目标 */

    /* 硬链接是真的：同一文件身份；改一处，另一处跟着变 */
    unsigned long long d1 = 0, i1 = 0, d2 = 0, i2 = 0;
    CHECK(omnic_fs_file_id("omnic_arc_lk_out/a.txt", &d1, &i1) == OMNIC_OK);
    CHECK(omnic_fs_file_id("omnic_arc_lk_out/hard.txt", &d2, &i2) == OMNIC_OK);
    CHECK(d1 == d2 && i1 == i2 && !(d1 == 0 && i1 == 0));
    wfile("omnic_arc_lk_out/hard.txt", "changed!", 8);
    /* 读文件本身核对（目录枚举给出的副本信息在 NTFS 上可能滞后） */
    size_t hn = 0;
    unsigned char *hb = slurp("omnic_arc_lk_out/a.txt", &hn);
    CHECK(hb != NULL && hn == 8); /* 经 hard.txt 改的，a.txt 跟着变 */
    free(hb);
    t_meta m;

    /* --file 单选：硬链接的目标条目不在场，就地物化成独立副本；符号链接照建 */
    const char *names[2] = {"hard.txt", "slink"};
    CHECK(omnic_unpack_files(A, X, names, sl_ok ? 2u : 1u, &o, &g_rep) == OMNIC_OK);
    CHECK(files_equal("omnic_arc_lk/a.txt", "omnic_arc_lk_x/hard.txt"));
    CHECK(omnic_fs_file_id("omnic_arc_lk_x/hard.txt", &d2, &i2) == OMNIC_OK);
    CHECK(!(d1 == d2 && i1 == i2)); /* 独立副本，不与原输出目录里的文件结成硬链接 */
    CHECK(file_exists("omnic_arc_lk_x/a.txt") == 0); /* 没有顺手多解出目标条目 */
    if (sl_ok) {
        CHECK(stat_meta("omnic_arc_lk_x/slink", &m, 0) == 0 && m.link_kind == OMNIC_FS_LINK_SYMLINK &&
              strcmp(m.target, "a.txt") == 0);
    }

    /* 再解一次（全量提取的覆盖语义）：既存的链接被替换，仍然成功 */
    CHECK(unpack_arc(A, O, o) == OMNIC_OK);
    CHECK(tree_equal(T, O));

    /* 清理：先删链接本身——目录联接若被顺着走进去，删掉的是目标里的内容 */
    omnic_fs_remove_force("omnic_arc_lk/dlink");
    omnic_fs_remove_force("omnic_arc_lk/slink");
    omnic_fs_remove_force("omnic_arc_lk_out/dlink");
    omnic_fs_remove_force("omnic_arc_lk_out/slink");
    omnic_fs_remove_force("omnic_arc_lk_x/slink");
    rm_tree(T);
    rm_tree(O);
    rm_tree(X);
    remove(A);
}

static void test_v2_meta(void) {
    const char *T = "omnic_arc_md", *O = "omnic_arc_md_out", *A = "omnic_arc_md.omnic";
    rm_tree_deep(T);
    rm_tree_deep(O);
    omnic_fs_mkdir_p("omnic_arc_md/dir");
    char txt[128];
    fill_text(txt, 60);
    wfile("omnic_arc_md/plain.txt", txt, 60);
    wfile("omnic_arc_md/ro.txt", txt, 60);
    wfile("omnic_arc_md/x.sh", "#!/bin/sh\necho hi\n", 18);
    omnic_fs_meta m;
    memset(&m, 0, sizeof(m));
#ifdef _WIN32
    m.attrs = OMNIC_FS_ATTR_READONLY | OMNIC_FS_ATTR_HIDDEN;
    CHECK(omnic_fs_set_meta("omnic_arc_md/ro.txt", &m) == OMNIC_OK);
    m.attrs = OMNIC_FS_ATTR_SYSTEM;
    CHECK(omnic_fs_set_meta("omnic_arc_md/x.sh", &m) == OMNIC_OK);
    m.attrs = OMNIC_FS_ATTR_HIDDEN;
    CHECK(omnic_fs_set_meta("omnic_arc_md/dir", &m) == OMNIC_OK);
#else
    m.mode = 0600;
    CHECK(omnic_fs_set_meta("omnic_arc_md/ro.txt", &m) == OMNIC_OK);
    m.mode = 0755;
    CHECK(omnic_fs_set_meta("omnic_arc_md/x.sh", &m) == OMNIC_OK);
    m.mode = 0750;
    CHECK(omnic_fs_set_meta("omnic_arc_md/dir", &m) == OMNIC_OK);
#endif
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    CHECK(pack_tree(T, A, o) == OMNIC_OK);
    CHECK(unpack_arc(A, O, o) == OMNIC_OK);
    t_meta ro, xs, dir, pl;
    CHECK(stat_meta("omnic_arc_md_out/ro.txt", &ro, 0) == 0);
    CHECK(stat_meta("omnic_arc_md_out/x.sh", &xs, 0) == 0);
    CHECK(stat_meta("omnic_arc_md_out/dir", &dir, 0) == 0);
    CHECK(stat_meta("omnic_arc_md_out/plain.txt", &pl, 0) == 0);
#ifdef _WIN32
    CHECK((ro.attrs & (OMNIC_FS_ATTR_READONLY | OMNIC_FS_ATTR_HIDDEN)) ==
          (OMNIC_FS_ATTR_READONLY | OMNIC_FS_ATTR_HIDDEN));
    CHECK((xs.attrs & OMNIC_FS_ATTR_SYSTEM) != 0);
    CHECK((dir.attrs & OMNIC_FS_ATTR_HIDDEN) != 0); /* 目录的属性在文件全部写完后设 */
    CHECK(pl.attrs == 0);
#else
    CHECK(ro.has_mode && ro.mode == 0600); /* 私密文件保持私密 */
    CHECK(xs.has_mode && xs.mode == 0755); /* 可执行文件解出来就能运行 */
    CHECK(dir.has_mode && dir.mode == 0750);
    CHECK(pl.has_mode && pl.mode == 0644);
#endif
    CHECK(files_equal("omnic_arc_md/x.sh", "omnic_arc_md_out/x.sh"));
    /* 全量提取再解一次：只读的既存文件也照常覆盖（覆盖语义不因为保了只读属性而变） */
    CHECK(unpack_arc(A, O, o) == OMNIC_OK);
    CHECK(files_equal("omnic_arc_md/ro.txt", "omnic_arc_md_out/ro.txt"));
    /* --file 单选到已存在的文件：不带 -f 拒绝；带 -f 覆盖只读文件并重新保上只读 */
    const char *names[1] = {"ro.txt"};
    CHECK(omnic_unpack_files(A, O, names, 1, &o, &g_rep) != OMNIC_OK);
    omnic_options of = mkopts(OMNIC_F_QUIET | OMNIC_F_FORCE, 0);
    CHECK(omnic_unpack_files(A, O, names, 1, &of, &g_rep) == OMNIC_OK);
    CHECK(stat_meta("omnic_arc_md_out/ro.txt", &ro, 0) == 0);
#ifdef _WIN32
    CHECK((ro.attrs & OMNIC_FS_ATTR_READONLY) != 0);
#else
    CHECK(ro.has_mode && ro.mode == 0600);
#endif
    rm_tree_deep(T);
    rm_tree_deep(O);
    remove(A);
}

static void test_v2_ctime(void) {
    const char *T = "omnic_arc_ct", *O = "omnic_arc_ct_out", *A = "omnic_arc_ct.omnic";
    rm_tree(T);
    rm_tree(O);
    omnic_fs_mkdir_p(T);
    char txt[64];
    fill_text(txt, 30);
    wfile("omnic_arc_ct/a.txt", txt, 30);
    const long long old = 1000000000LL; /* 2001-09-09 */
    set_creation_time("omnic_arc_ct/a.txt", old);
    long long now = (long long)time(NULL);
    t_meta e;

    /* 默认不存创建时间：解出来是新建文件的时间 */
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(unpack_arc(A, O, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(stat_meta("omnic_arc_ct_out/a.txt", &e, OMNIC_FS_WANT_CTIME) == 0);
    CHECK(e.ctime > now - 3600);
    /* --ctime：存下来；Windows 上原样恢复 */
    rm_tree(O);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET | OMNIC_F_CTIME, 0)) == OMNIC_OK);
    CHECK(unpack_arc(A, O, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    CHECK(stat_meta("omnic_arc_ct_out/a.txt", &e, OMNIC_FS_WANT_CTIME) == 0);
#ifdef _WIN32
    CHECK(e.ctime >= old - 2 && e.ctime <= old + 2);
#else
    CHECK(e.ctime > now - 3600); /* POSIX：创建时间只存不恢复（没有可移植的设置接口） */
#endif
    rm_tree(T);
    rm_tree(O);
    remove(A);
}

static void test_v2_layout(void) {
    /* 头部版本 2；计数在头部（32 位）；尾部那 4 个字节保留为 0 */
    const char *T = "omnic_arc_ly", *A = "omnic_arc_ly.omnic";
    rm_tree(T);
    omnic_fs_mkdir_p("omnic_arc_ly/d");
    char txt[64];
    fill_text(txt, 30);
    wfile("omnic_arc_ly/f.txt", txt, 30);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    size_t n = 0;
    unsigned char *b = slurp(A, &n);
    CHECK(b != NULL && n > 64);
    if (b != NULL) {
        CHECK(b[8] == OMNIC_ARC_VERSION && b[9] == 0);
        CHECK(rd32(b + 16) == 2);  /* 目录 + 文件 */
        CHECK(rd32(b + 12) == 1);  /* 一个块 */
        CHECK(rd32(b + 20) == 32); /* data_off */
        CHECK(memcmp(b + n - 32, "OMNICEND", 8) == 0);
        CHECK(b[n - 4] == 0 && b[n - 3] == 0 && b[n - 2] == 0 && b[n - 1] == 0);
        free(b);
    }
    rm_tree(T);
    remove(A);
}

static void test_v1_archive_readable(void) {
    /* 由 v1 写入端（提交 c064007 的二进制）打出的归档：压缩块 + store 块 + 子目录 + 空文件 + 中文名。
     * v2 的读取端必须原样解出。从构建目录或仓库根运行都能找到样本。 */
    const char *cand[2] = {"../tests/fixtures/v1_sample.omnic", "tests/fixtures/v1_sample.omnic"};
    const char *F = NULL;
    for (int i = 0; i < 2 && F == NULL; i++)
        if (file_exists(cand[i])) F = cand[i];
    CHECK(F != NULL);
    if (F == NULL) return;
    size_t n = 0;
    unsigned char *b = slurp(F, &n);
    CHECK(b != NULL && n == 1349);
    CHECK(b != NULL && b[8] == 1 && b[9] == 0); /* 版本 1 */
    free(b);
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    const char *O = "omnic_arc_v1_out";
    rm_tree(O);
    CHECK(omnic_arc_verify(F, &o, &g_rep) == OMNIC_OK);
    CHECK(unpack_arc(F, O, o) == OMNIC_OK);
    t_meta a, e;
    CHECK(stat_meta("omnic_arc_v1_out/a.txt", &a, 0) == 0 && a.size > 300 && a.is_dir == 0);
    CHECK(stat_meta("omnic_arc_v1_out/empty.bin", &e, 0) == 0 && e.size == 0);
    CHECK(stat_meta("omnic_arc_v1_out/rand.bin", &e, 0) == 0 && e.size == 1024);
    CHECK(stat_meta("omnic_arc_v1_out/sub", &e, 0) == 0 && e.is_dir == 1);
    CHECK(file_exists("omnic_arc_v1_out/sub/中文.txt") == 1);
    /* 文件时间：打包前设成了 2020-01-02 03:04:05（本地时间） */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 120;
    tm.tm_mon = 0;
    tm.tm_mday = 2;
    tm.tm_hour = 3;
    tm.tm_min = 4;
    tm.tm_sec = 5;
    long long want = (long long)mktime(&tm);
    CHECK(a.mtime >= want - 2 && a.mtime <= want + 2);
    CHECK(omnic_arc_list(F, &o) == OMNIC_OK);
    rm_tree(O);
}

static void test_index_fuzz(void) {
    /* 逐比特翻转一个小归档：任何一处损坏都不得让读取端崩溃；能通过校验的只允许是
     * 规范里声明"保留、读取端忽略"的字节（头部 24..32、尾部最后 4 字节）。 */
    const char *T = "omnic_arc_fz", *A = "omnic_arc_fz.omnic";
    rm_tree(T);
    omnic_fs_mkdir_p(T);
    char txt[64];
    fill_text(txt, 40);
    wfile("omnic_arc_fz/f.txt", txt, 40);
    CHECK(pack_tree(T, A, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    size_t n = 0;
    unsigned char *good = slurp(A, &n);
    unsigned char *buf = (unsigned char *)malloc(n);
    CHECK(good != NULL && buf != NULL && n > 64);
    omnic_options o = mkopts(OMNIC_F_QUIET, 0);
    size_t flips = 0, accepted = 0, bad_accept = 0;
    for (size_t i = 0; good != NULL && buf != NULL && i < n; i++) {
        for (int k = 0; k < 8; k++) {
            memcpy(buf, good, n);
            buf[i] ^= (unsigned char)(1u << k);
            FILE *f = fopen(A, "wb"); /* 短 ASCII 路径：CRT fopen 足够 */
            if (f == NULL) break;
            fwrite(buf, 1, n, f);
            fclose(f);
            flips++;
            memset(&g_rep, 0, sizeof(g_rep));
            if (omnic_arc_verify(A, &o, &g_rep) == OMNIC_OK) {
                accepted++;
                if (!((i >= 24 && i < 32) || i + 4 >= n)) bad_accept++;
            }
        }
    }
    printf("note: fuzz: %zu single-bit flips, %zu accepted, %zu outside reserved bytes\n", flips,
           accepted, bad_accept);
    CHECK(flips == n * 8);
    CHECK(bad_accept == 0);
    free(buf);
    free(good);
    rm_tree(T);
    remove(A);
}

int main(void) {
    test_roundtrip();
    test_roundtrip_edges();
    test_roundtrip_big();
    test_extract();
    test_corruption();
    test_malformed();
    test_bounds();
    test_parallel();
    test_fewbig_pool();
    test_pack_determinism();
    test_pack_parallel_blocks();
    test_decode_pool();
    test_pack_atomic_publish();
    test_pack_reports_skipped();
    test_long_path_roundtrip();
    test_unpack_dir_failure_continues();
    test_dir_mtime_not_restored();
    test_v2_links();
    test_v2_meta();
    test_v2_ctime();
    test_v2_layout();
    test_v1_archive_readable();
    test_index_fuzz();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* OMNIC_CODEC_ZSTD */
