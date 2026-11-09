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
#else
#include <unistd.h>
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

/* 索引明文布局（与 archive.c 序列化对齐） */
#define T_BLK_ROW 32
#define T_ENT_FIX 26

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
        free(fa);
        free(fb);
        return 0;
    }
    qsort(fa, na, sizeof(*fa), cmp_fs_ent);
    qsort(fb, nb, sizeof(*fb), cmp_fs_ent);
    int ok = (na == nb);
    if (ok) {
        for (size_t i = 0; i < na && ok; i++) {
            if (strcmp(fa[i].rel, fb[i].rel) != 0 || fa[i].is_dir != fb[i].is_dir ||
                fa[i].size != fb[i].size) {
                ok = 0;
                break;
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
    free(fa);
    free(fb);
    return ok;
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
    *nent = (uint32_t)ftr[28] | ((uint32_t)ftr[29] << 8);
    *nblk = (uint32_t)ftr[30] | ((uint32_t)ftr[31] << 8);
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
static int save_index_plain(const char *arc, uint8_t *plain, size_t n, uint64_t index_off,
                            uint32_t nent, uint32_t nblk) {
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
    ftr[28] = (uint8_t)(nent & 0xFF);
    ftr[29] = (uint8_t)(nent >> 8);
    ftr[30] = (uint8_t)(nblk & 0xFF);
    ftr[31] = (uint8_t)(nblk >> 8);
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
        off += 2 + nl + T_ENT_FIX + 8;
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
            CHECK(save_index_plain(ARC, pl, n, io, nent, nblk) == 0);
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
    flip_byte(ARC, file_size(ARC) - 2);
    memset(&g_rep, 0, sizeof(g_rep));
    CHECK(omnic_unpack(ARC, "omnic_arc_m2", &o, &g_rep) == OMNIC_E_DATA);

    /* Footer nent 与 Header 不一致：改 Footer nent 字段 */
    CHECK(pack_tree(TREE, ARC, mkopts(OMNIC_F_QUIET, 0)) == OMNIC_OK);
    {
        FILE *f = omnic_fs_fopen(ARC, "r+b");
        CHECK(f != NULL);
        if (f != NULL) {
            fseek(f, -4, SEEK_END); /* nent/nblk 在 Footer 尾 4B */
            uint8_t nb[4];
            fread(nb, 1, 4, f);
            fseek(f, -4, SEEK_END);
            fputc(nb[0] ^ 0x01, f); /* nent±1 制造交叉不一致 */
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
            CHECK(save_index_plain(ARC, pl, n, io, nent, nblk) == 0);
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
                off += 2 + nl + T_ENT_FIX + 8;
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
            CHECK(save_index_plain(ARC2, pl, n, io, nent, nblk) == 0);
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
        rm_tree("omnic_arc_pf");
    }

    remove(ARC2);
    rm_tree(TREE2);
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
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* OMNIC_CODEC_ZSTD */
