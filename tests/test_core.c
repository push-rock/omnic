/* 核心回归（TST-CODEC/FMT/DEC/POL/NEG 的一期子集）：
 * 决策表、探测、熵、每 codec roundtrip、跳过路径、截断检测。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "core/registry.h"
#include "core/util.h"
#include "detect/entropy.h"
#include "detect/filetype.h"
#include "detect/magic.h"
#include "omnic.h"
#include "policy/policy.h"

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

static const char *SRC = "omnic_test_src.txt";
static const char *DST_FMT = "omnic_test_dst.tmp";

/* 清理历史运行残留（失败中断时可能留下产物） */
static void cleanup_all(void) {
    const char *files[] = {SRC,
                           "omnic_test_src.txt.zst",
                           "omnic_test_src.txt.lz4",
                           "omnic_test_src.txt.xz",
                           DST_FMT,
                           "omnic_test_dst",
                           "omnic_test_rand.png",
                           "omnic_test_rand.png.zst",
                           "omnic_test_rand",
                           "omnic_test_rand.zst",
                           "omnic_test_trunc.zst",
                           "omnic_test_trunc",
                           NULL};
    for (int i = 0; files[i] != NULL; i++) remove(files[i]);
}

static unsigned rng_state = 12345;
static unsigned rng(void) { return (rng_state = rng_state * 1103515245u + 12345u); }

/* 3MB 可压缩"文本"：重复词表 + 噪声 */
static void make_src(void) {
    static const char *words[] = {"compression", "omnic", "codec", "policy", "stream",
                                  "entropy", "magic", "format", "block", "zstd"};
    FILE *f = fopen(SRC, "wb");
    char buf[4096];
    size_t total = 0;
    while (total < 3u * 1024u * 1024u) {
        size_t n = 0;
        int lines = 8;
        for (int i = 0; i < lines; i++) {
            const char *w = words[rng() % 10];
            size_t wl = strlen(w);
            memcpy(buf + n, w, wl);
            n += wl;
            buf[n++] = ' ';
            if (rng() % 7 == 0) buf[n++] = '\n';
        }
        buf[n++] = '\n';
        fwrite(buf, 1, n, f);
        total += n;
    }
    fclose(f);
}

static void make_random(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    unsigned char b[65536];
    while (n > 0) {
        size_t c = n < sizeof(b) ? n : sizeof(b);
        for (size_t i = 0; i < c; i++) b[i] = (unsigned char)(rng() >> 16);
        fwrite(b, 1, c, f);
        n -= c;
    }
    fclose(f);
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (fa == NULL || fb == NULL) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    char ba[65536], bb[65536];
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) {
            fclose(fa);
            fclose(fb);
            return 0;
        }
        if (na == 0) break;
    }
    fclose(fa);
    fclose(fb);
    return 1;
}

static void rm(const char *p) { remove(p); }

static omnic_options def_opts(void) {
    /* 测试统一保留源文件（压缩成功默认删源，假设 A8） */
    omnic_options o = {OMNIC_PROFILE_BALANCED, NULL, 0, 0, OMNIC_F_KEEP, NULL};
    return o;
}

/* 生成压缩输出路径：base + "." + ext */
static char *with_ext(const char *base, const char *ext) {
    size_t n = strlen(base) + strlen(ext) + 2;
    char *p = malloc(n);
    snprintf(p, n, "%s.%s", base, ext);
    return p;
}

static void test_policy(void) {
    for (int c = 0; c <= (int)OMNIC_CLS_RANDOM; c++) {
        for (int p = 0; p < (int)OMNIC_PROFILE_COUNT; p++) {
            const omnic_policy_entry *e = omnic_policy_lookup((omnic_class)c, (omnic_profile)p);
            CHECK(e != NULL);
            if (e != NULL) CHECK(e->codec != NULL && e->codec[0] != '\0');
        }
    }
    const omnic_policy_entry *e = omnic_policy_lookup(OMNIC_CLS_TEXT, OMNIC_PROFILE_BEST_RATIO);
    CHECK(e != NULL && strcmp(e->codec, "xz") == 0);
    e = omnic_policy_lookup(OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BALANCED);
    CHECK(omnic_policy_is_skip(e));
    e = omnic_policy_lookup(OMNIC_CLS_TEXT, OMNIC_PROFILE_FAST_COMPRESS);
    CHECK(e != NULL && strcmp(e->codec, "lz4") == 0);
}

static void test_detect(void) {
    CHECK(omnic_detect_magic((const uint8_t *)"\x28\xB5\x2F\xFD", 4) == OMNIC_FMT_ZSTD);
    CHECK(omnic_detect_magic((const uint8_t *)"\x1F\x8B\x08\x00", 4) == OMNIC_FMT_GZIP);
    CHECK(omnic_detect_magic((const uint8_t *)"\xFD\x37\x7A\x58\x5A\x00", 6) == OMNIC_FMT_XZ);
    CHECK(omnic_detect_magic((const uint8_t *)"\x42\x5A\x68\x31", 4) == OMNIC_FMT_BZIP2);
    CHECK(omnic_detect_magic((const uint8_t *)"\x04\x22\x4D\x18", 4) == OMNIC_FMT_LZ4);
    CHECK(omnic_detect_magic((const uint8_t *)"\x78\x9C", 2) == OMNIC_FMT_ZLIB);
    CHECK(omnic_detect_magic((const uint8_t *)"\x1F\x9D\x90", 3) == OMNIC_FMT_COMPRESS_Z);
    CHECK(omnic_detect_magic((const uint8_t *)"\x00\x01\x02\x03", 4) == OMNIC_FMT_UNKNOWN);
    CHECK(omnic_detect("x.br", (const uint8_t *)"garbage!", 8) == OMNIC_FMT_BROTLI);
    CHECK(omnic_detect("x.tar", (const uint8_t *)"garbage!", 8) == OMNIC_FMT_UNKNOWN);

    CHECK(omnic_classify_ext("a.json") == OMNIC_CLS_TEXT);
    CHECK(omnic_classify_ext("a.EXE") == OMNIC_CLS_EXEC);
    CHECK(omnic_classify_ext("a.png") == OMNIC_CLS_ALREADY_COMPRESSED);
    CHECK(omnic_classify_ext("a.bmp") == OMNIC_CLS_MEDIA_RAW);
    CHECK(omnic_classify_ext("a.sqlite") == OMNIC_CLS_STRUCTURED);
    CHECK(omnic_classify_ext("noext") == OMNIC_CLS_UNKNOWN);

    double h0 = omnic_entropy((const uint8_t *)"aaaa", 4);
    CHECK(h0 == 0.0);
    CHECK(omnic_entropy((const uint8_t *)"\x00", 1) == 0.0);
}

/* 完整流程：compress -> 解压到独立文件，比较内容（避免与 SRC 同名冲突） */
static void test_roundtrip_full(const char *name) {
    const omnic_codec_ops *ops = omnic_codec_find(name);
    CHECK(ops != NULL);
    if (ops == NULL) return;

    omnic_options o = def_opts();
    o.codec = name;

    omnic_report rep;
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *out = with_ext(SRC, ops->native_ext);

    /* 压缩产物改名避开 SRC 的扩展名，解压输出 = 去掉 .tmp 后缀 */
    CHECK(rename(out, DST_FMT) == 0);
    CHECK(omnic_decompress(DST_FMT, &o, &rep) == OMNIC_OK);
    CHECK(files_equal(SRC, "omnic_test_dst"));
    rm("omnic_test_dst");
    rm(DST_FMT);
    rm(out);
    free(out);
}

static void test_auto_and_skip(void) {
    omnic_options o = def_opts();
    omnic_report rep;

    /* 文本 + balanced -> zstd（决策表） */
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    CHECK(strcmp(rep.codec, "zstd") == 0);
    char *z = with_ext(SRC, "zst");
    {
        FILE *probe = fopen(z, "rb"); /* Windows：句柄未关时 remove 会失败，必须 fclose */
        CHECK(probe != NULL);
        if (probe != NULL) fclose(probe);
        rm(z);
    }
    free(z);

    /* 高熵 + .png -> 跳过 */
    make_random("omnic_test_rand.png", 1u << 20);
    CHECK(omnic_compress("omnic_test_rand.png", &o, &rep) == OMNIC_OK);
    CHECK(rep.skipped);
    /* --force 后照常压缩 */
    o.flags = OMNIC_F_FORCE | OMNIC_F_KEEP;
    CHECK(omnic_compress("omnic_test_rand.png", &o, &rep) == OMNIC_OK);
    CHECK(!rep.skipped);
    o.flags = 0;

    /* 无后缀高熵 -> RANDOM -> 跳过 */
    make_random("omnic_test_rand", 1u << 20);
    CHECK(omnic_compress("omnic_test_rand", &o, &rep) == OMNIC_OK);
    CHECK(rep.skipped);
    rm("omnic_test_rand");
    rm("omnic_test_rand.zst");

    /* analyze */
    CHECK(omnic_analyze(SRC, &rep) == OMNIC_OK);
    CHECK(rep.klass == OMNIC_CLS_TEXT && rep.entropy < 6.0);
}

static void test_truncated(void) {
    /* zstd 产物截断 -> E_DATA（SYS-DEC-003 子集） */
    omnic_options o = def_opts();
    o.codec = "zstd";
    omnic_report rep;
    if (omnic_codec_find("zstd") == NULL) return;
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *z = with_ext(SRC, "zst");
    /* 截一半 */
    FILE *f = fopen(z, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    FILE *g = fopen(z, "r+b");
    /* 覆盖尾部：保留前一半，截断 */
    size_t keep = (size_t)(sz / 2);
    /* Windows 下无 ftruncate 便捷方式：拷贝前半到新文件 */
    fclose(g);
    f = fopen(z, "rb");
    unsigned char *half = malloc(keep);
    fread(half, 1, keep, f);
    fclose(f);
    f = fopen("omnic_test_trunc.zst", "wb");
    fwrite(half, 1, keep, f);
    fclose(f);
    free(half);
    o.flags = 0;
    CHECK(omnic_decompress("omnic_test_trunc.zst", &o, &rep) == OMNIC_E_DATA);
    rm("omnic_test_trunc.zst");
    rm("omnic_test_trunc");
    rm(z);
    free(z);
}

int main(void) {
    cleanup_all();
    make_src();
    test_policy();
    test_detect();
    test_auto_and_skip();
    test_roundtrip_full("zstd");
    test_roundtrip_full("lz4");
    test_roundtrip_full("xz");
    test_truncated();
    rm(SRC);
    rm("omnic_test_rand.png");
    rm("omnic_test_rand.png.zst");
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
