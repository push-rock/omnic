/* 核心回归（TST-CODEC/FMT/DEC/POL/NEG 的一期子集）：
 * 决策表、探测、熵、每 codec roundtrip、跳过路径、截断检测。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec/codec.h"
#include "core/registry.h"
#include "core/util.h"
#include "detect/entropy.h"
#include "detect/feature.h"
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
                           "omnic_test_src.txt.gz",
                           "omnic_test_src.txt.br",
                           "omnic_test_src.txt.snappy",
                           "omnic_test_src.txt.lzfse",
                           "omnic_test_src.txt.zlib",
                           "omnic_test_src.txt.zz",
                           "omnic_test_src.txt.lzma",
                           DST_FMT,
                           "omnic_test_dst",
                           "omnic_test_dst.gz",
                           "omnic_test_dst.br",
                           "omnic_test_dst.snappy",
                           "omnic_test_dst.lzfse",
                           "omnic_test_dst.zlib",
                           "omnic_test_dst.zz",
                           "omnic_test_dst.lzma",
                           "omnic_test_rand.png",
                           "omnic_test_rand.png.zst",
                           "omnic_test_rand",
                           "omnic_test_rand.zst",
                           "omnic_test_bigz",
                           "omnic_test_bigr",
                           "omnic_test_trunc.zst",
                           "omnic_test_trunc.gz",
                           "omnic_test_trunc.br",
                           "omnic_test_trunc.snappy",
                           "omnic_test_trunc.lzfse",
                           "omnic_test_trunc.zlib",
                           "omnic_test_trunc.zz",
                           "omnic_test_trunc.lzma",
                           "omnic_test_trunc.lzvn",
                           "omnic_test_lzvn.lzvn",
                           "omnic_test_lzvn",
                           "omnic_test_lzvn_ref",
                           "omnic_test_lz.lz",
                           "omnic_test_lz",
                           "omnic_test_corpus.lz",
                           "omnic_test_corpus",
                           "omnic_test_bz.bz2",
                           "omnic_test_bz",
                           "omnic_test_z.Z",
                           "omnic_test_z",
                           "omnic_test_trunc",
                           "omnic_test_empty",
                           "omnic_test_mm1.gz",
                           "omnic_test_mm2.gz",
                           "omnic_test_mm.gz",
                           "omnic_test_mm",
                           "omnic_test_mm_ref",
                           "omnic_test_crc.snappy",
                           "omnic_test_crc",
                           NULL};
    for (int i = 0; files[i] != NULL; i++) remove(files[i]);
}

static unsigned rng_state = 12345;
static unsigned rng(void) { return (rng_state = rng_state * 1103515245u + 12345u); }

/* 3MB 可压缩"文本"：重复词表 + 噪声 */
static void make_src(void) {
    /* 1024 个合成词的随机句：词表足够大，dup16 落自然语言带 <0.03
     * （feature_probe 实测校准），γ 细分为 NTEXT */
    FILE *f = fopen(SRC, "wb");
    char buf[4096];
    size_t total = 0;
    while (total < 3u * 1024u * 1024u) {
        size_t n = 0;
        int lines = 8;
        for (int i = 0; i < lines; i++) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "w%u ", (unsigned)((rng() >> 22) % 1024u));
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

static void make_zeros(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    unsigned char b[65536] = {0};
    while (n > 0) {
        size_t c = n < sizeof(b) ? n : sizeof(b);
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

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"), *b = fopen(dst, "wb");
    if (a == NULL || b == NULL) {
        if (a) fclose(a);
        if (b) fclose(b);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 0;
}

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
    /* 二期细类表（PHASE2_DECIDER_DESIGN §3.4）：γ 细分后进表的 8 类；
     * 粗类 TEXT/MEDIA_RAW 只作为细分中间态，无表行；STRUCTURED 细分撤销 */
    static const omnic_class klasses[] = {
        OMNIC_CLS_NTEXT,             OMNIC_CLS_CODE,      OMNIC_CLS_EXEC, OMNIC_CLS_STRUCTURED,
        OMNIC_CLS_GRAPHIC,           OMNIC_CLS_PHOTO,     OMNIC_CLS_ALREADY_COMPRESSED,
        OMNIC_CLS_RANDOM,            OMNIC_CLS_UNKNOWN};
    for (size_t i = 0; i < sizeof(klasses) / sizeof(klasses[0]); i++) {
        for (int p = 0; p < (int)OMNIC_PROFILE_COUNT; p++) {
            const omnic_policy_entry *e = omnic_policy_lookup(klasses[i], (omnic_profile)p);
            CHECK(e != NULL);
            if (e != NULL) CHECK(e->codec != NULL && e->codec[0] != '\0');
        }
    }
    const omnic_policy_entry *e = omnic_policy_lookup(OMNIC_CLS_NTEXT, OMNIC_PROFILE_BEST_RATIO);
    CHECK(e != NULL && strcmp(e->codec, "xz") == 0);
    e = omnic_policy_lookup(OMNIC_CLS_CODE, OMNIC_PROFILE_BEST_RATIO);
    CHECK(e != NULL && strcmp(e->codec, "xz") == 0);
    e = omnic_policy_lookup(OMNIC_CLS_PHOTO, OMNIC_PROFILE_BEST_RATIO);
    CHECK(e != NULL && strcmp(e->codec, "xz") == 0);
    e = omnic_policy_lookup(OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BALANCED);
    CHECK(omnic_policy_is_skip(e));
    e = omnic_policy_lookup(OMNIC_CLS_NTEXT, OMNIC_PROFILE_FAST_COMPRESS);
    CHECK(e != NULL && strcmp(e->codec, "lz4") == 0);
}

/* ---- 特征层与细分类（PHASE2_DECIDER_DESIGN §3.3/3.4） ---- */
static void test_feature(void) {
    omnic_features g;
    uint8_t *buf = (uint8_t *)malloc(OMNIC_SAMPLE_SEG);
    CHECK(buf != NULL);
    if (buf == NULL) return;

    /* 全零：熵 0、零率 1、窗口重复 1（退化段复用头段） */
    memset(buf, 0, OMNIC_SAMPLE_SEG);
    omnic_feature_extract(buf, OMNIC_SAMPLE_SEG, NULL, 0, NULL, 0, &g);
    CHECK(g.zero_rate > 0.99 && g.entropy_h < 0.01);
    CHECK(g.dup_rate > 0.95);

    /* 高熵随机：无任何窗口重复 */
    for (size_t i = 0; i < OMNIC_SAMPLE_SEG; i++) buf[i] = (uint8_t)(rng() >> 24);
    omnic_feature_extract(buf, OMNIC_SAMPLE_SEG, NULL, 0, NULL, 0, &g);
    CHECK(g.entropy_h > 7.9 && g.entropy_delta < 0.05);
    CHECK(g.dup_rate < 0.05 && g.dup_rate_16 < 0.01);

    /* 周期性块（窗口大小整数倍）：4KiB 窗口全同 */
    for (size_t i = 0; i < OMNIC_SAMPLE_SEG; i++) buf[i] = (uint8_t)(i % 4096);
    omnic_feature_extract(buf, OMNIC_SAMPLE_SEG, NULL, 0, NULL, 0, &g);
    CHECK(g.dup_rate > 0.95);

    /* γ 判据（PHASE2 §3.4 实测校准）：CODE/NTEXT、GRAPHIC/PHOTO、其余不细分 */
    omnic_features t = {0};
    t.printable = 0.95;
    t.dup_rate_16 = 0.6;
    CHECK(omnic_refine_class(OMNIC_CLS_TEXT, &t) == OMNIC_CLS_CODE);
    t.dup_rate_16 = 0.02; /* 实测自然语言带：dickens 0.001 / enwik8 0.022 */
    CHECK(omnic_refine_class(OMNIC_CLS_TEXT, &t) == OMNIC_CLS_NTEXT);
    t.printable = 0.6; /* 不可打印文本不进 CODE */
    t.dup_rate_16 = 0.6;
    CHECK(omnic_refine_class(OMNIC_CLS_TEXT, &t) == OMNIC_CLS_NTEXT);
    omnic_features mv = {0};
    mv.dup_rate = 0.5;
    CHECK(omnic_refine_class(OMNIC_CLS_MEDIA_RAW, &mv) == OMNIC_CLS_GRAPHIC);
    mv.dup_rate = 0.01;
    CHECK(omnic_refine_class(OMNIC_CLS_MEDIA_RAW, &mv) == OMNIC_CLS_PHOTO);
    omnic_features sd = {0};
    sd.entropy_delta = 2.0;
    CHECK(omnic_refine_class(OMNIC_CLS_STRUCTURED, &sd) == OMNIC_CLS_STRUCTURED);
    CHECK(omnic_refine_class(OMNIC_CLS_EXEC, &sd) == OMNIC_CLS_EXEC);
    free(buf);
}

/* FC 档候选试压（PHASE2 §3.5）：≥32MiB、reason 含实测数字、roundtrip 完整。
 * SRC 自然文本 ×11（≥32MiB）：自然文本上 zstd-3 的 ρ 稳定优于 lz4
 * （一期实测），验证预算内选优方向。 */
static void test_trial(void) {
    omnic_options o = def_opts();
    omnic_report rep;
    const char *path = "omnic_test_trial.tar";
    const char *ref = "omnic_test_trial_ref.tar";
    rm("omnic_test_trial.tar.zst");
    rm("omnic_test_trial.tar.lz4"); /* 上次运行的残留（试压胜者不定） */
    rm(path);
    rm(ref);

    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    for (int i = 0; i < 11; i++) { /* 3MiB × 11 = 33MiB */
        FILE *s = fopen(SRC, "rb");
        CHECK(s != NULL);
        if (s == NULL) {
            fclose(f);
            return;
        }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), s)) > 0) fwrite(buf, 1, n, f);
        fclose(s);
    }
    fclose(f);
    CHECK(copy_file(path, ref) == 0);

    o.profile = OMNIC_PROFILE_FAST_COMPRESS;
    CHECK(omnic_compress(path, &o, &rep) == OMNIC_OK);
    CHECK(!rep.skipped);
    CHECK(strstr(rep.reason, "trial 256KiB") != NULL);
    CHECK(strcmp(rep.codec, "zstd") == 0); /* 自然文本 zstd-3 ρ 胜 lz4（预算内选优） */

    o.flags |= OMNIC_F_FORCE; /* def_opts 的 KEEP 保留了源文件，解压需覆盖 */
    CHECK(omnic_decompress("omnic_test_trial.tar.zst", &o, &rep) == OMNIC_OK);
    CHECK(files_equal(path, ref));
    rm("omnic_test_trial.tar.zst");
    rm(path);
    rm(ref);
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

/* 完整流程：compress -> 解压到独立文件，比较内容（避免与 SRC 同名冲突）。
 * 产物改名 omnic_test_dst.<ext>：无 magic 的 codec（brotli/raw-deflate 族）
 * 解压识别依赖后缀兜底，不能统一改成 .tmp。 */
static void test_roundtrip_full(const char *name) {
    const omnic_codec_ops *ops = omnic_codec_find(name);
    CHECK(ops != NULL);
    if (ops == NULL) return;

    omnic_options o = def_opts();
    o.codec = name;

    omnic_report rep;
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *out = with_ext(SRC, ops->native_ext);
    char dst2[64];
    snprintf(dst2, sizeof dst2, "omnic_test_dst.%s", ops->native_ext);

    CHECK(rename(out, dst2) == 0);
    CHECK(omnic_decompress(dst2, &o, &rep) == OMNIC_OK);
    CHECK(files_equal(SRC, "omnic_test_dst"));
    rm("omnic_test_dst");
    rm(dst2);
    rm(out);
    free(out);
}

/* 大文件 roundtrip：可压缩内容 >4MB 时压缩产物 <4MB（单输入块）而解压跨输出
 * flush——brotli 解码循环曾在该形态把输出满暂停误判为截断；随机 5MB 则令产物
 * 超过输入块大小，覆盖解压多块喂入。 */
static void test_big_roundtrip(const char *name) {
    const omnic_codec_ops *ops = omnic_codec_find(name);
    CHECK(ops != NULL);
    if (ops == NULL) return;

    const char *srcs[2] = {"omnic_test_bigz", "omnic_test_bigr"};
    omnic_options o = def_opts();
    o.codec = name;
    o.flags |= OMNIC_F_FORCE; /* 随机数据默认跳过 */

    for (int i = 0; i < 2; i++) {
        omnic_report rep;
        CHECK(omnic_compress(srcs[i], &o, &rep) == OMNIC_OK);
        char *out = with_ext(srcs[i], ops->native_ext);
        char dst2[64];
        snprintf(dst2, sizeof dst2, "omnic_test_dst.%s", ops->native_ext);
        CHECK(rename(out, dst2) == 0);
        CHECK(omnic_decompress(dst2, &o, &rep) == OMNIC_OK);
        CHECK(files_equal(srcs[i], "omnic_test_dst"));
        rm("omnic_test_dst");
        rm(dst2);
        free(out);
    }
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
    CHECK(rep.klass == OMNIC_CLS_NTEXT && rep.entropy < 6.0);
}

/* 截断检测（SYS-DEC-003 子集）：截去尾部 10 字节——必然落在最后一个
 * chunk/帧内部。统一截 10 字节是为了避开 snappy framing 的 chunk 边界截断
 * 形成合法短流（解压会成功前缀部分而非报错）。 */
static void test_truncated(const char *name) {
    omnic_options o = def_opts();
    o.codec = name;
    omnic_report rep;
    const omnic_codec_ops *ops = omnic_codec_find(name);
    CHECK(ops != NULL);
    if (ops == NULL) return;
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *z = with_ext(SRC, ops->native_ext);
    FILE *f = fopen(z, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    size_t keep = (size_t)(sz > 16 ? sz - 10 : sz / 2);
    f = fopen(z, "rb");
    unsigned char *part = malloc(keep);
    CHECK(fread(part, 1, keep, f) == keep);
    fclose(f);
    char tname[64];
    snprintf(tname, sizeof tname, "omnic_test_trunc.%s", ops->native_ext);
    f = fopen(tname, "wb");
    fwrite(part, 1, keep, f);
    fclose(f);
    free(part);
    o.flags = 0;
    CHECK(omnic_decompress(tname, &o, &rep) == OMNIC_E_DATA);
    rm(tname);
    rm("omnic_test_trunc");
    rm(z);
    free(z);
}

/* 空文件是经典边界：全部 codec 走一遍空输入 roundtrip */
static void test_empty_roundtrip(void) {
    static const char *names[] = {"zstd",     "lz4",  "xz",       "zlib",  "zlib-ng",
                                  "libdeflate", "brotli", "snappy", "lzfse",
                                  "zlib-stream", "raw-deflate", "lzma", NULL};
    FILE *f = fopen("omnic_test_empty", "wb");
    CHECK(f != NULL);
    if (f != NULL) fclose(f);
    for (int i = 0; names[i] != NULL; i++) {
        const omnic_codec_ops *ops = omnic_codec_find(names[i]);
        CHECK(ops != NULL);
        if (ops == NULL) continue;
        omnic_options o = def_opts();
        o.codec = names[i];
        omnic_report rep;
        char out[64], dst[64];
        snprintf(out, sizeof out, "omnic_test_empty.%s", ops->native_ext);
        snprintf(dst, sizeof dst, "omnic_empty_%d.%s", i, ops->native_ext);
        CHECK(omnic_compress("omnic_test_empty", &o, &rep) == OMNIC_OK);
        CHECK(rename(out, dst) == 0);
        o.flags = 0;
        CHECK(omnic_decompress(dst, &o, &rep) == OMNIC_OK);
        snprintf(dst, sizeof dst, "omnic_empty_%d", i); /* 解压输出名 */
        CHECK(files_equal("omnic_test_empty", dst));
        rm(dst);
        snprintf(dst, sizeof dst, "omnic_empty_%d.%s", i, ops->native_ext);
        rm(dst);
    }
    rm("omnic_test_empty");
}

/* 多 member gzip（cat a.gz b.gz）：解压须串接两个流的内容（inflateReset 路径） */
static void test_gzip_multimember(void) {
    const omnic_codec_ops *ops = omnic_codec_find("zlib");
    CHECK(ops != NULL);
    if (ops == NULL) return;
    omnic_options o = def_opts();
    o.codec = "zlib";
    omnic_report rep;

    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *z = with_ext(SRC, ops->native_ext);
    CHECK(rename(z, "omnic_test_mm1.gz") == 0);
    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    CHECK(rename(z, "omnic_test_mm2.gz") == 0);

    FILE *a = fopen("omnic_test_mm1.gz", "rb");
    FILE *b = fopen("omnic_test_mm2.gz", "rb");
    FILE *c = fopen("omnic_test_mm.gz", "wb");
    FILE *s = fopen(SRC, "rb");
    FILE *r = fopen("omnic_test_mm_ref", "wb");
    CHECK(a != NULL && b != NULL && c != NULL && s != NULL && r != NULL);
    if (a && b && c && s && r) {
        int ch;
        while ((ch = fgetc(a)) != EOF) fputc(ch, c);
        while ((ch = fgetc(b)) != EOF) fputc(ch, c);
        while ((ch = fgetc(s)) != EOF) fputc(ch, r);
        fseek(s, 0, SEEK_SET);
        while ((ch = fgetc(s)) != EOF) fputc(ch, r);
    }
    if (a) fclose(a);
    if (b) fclose(b);
    if (c) fclose(c);
    if (s) fclose(s);
    if (r) fclose(r);

    o.flags = 0;
    CHECK(omnic_decompress("omnic_test_mm.gz", &o, &rep) == OMNIC_OK);
    CHECK(files_equal("omnic_test_mm_ref", "omnic_test_mm"));
    rm("omnic_test_mm1.gz");
    rm("omnic_test_mm2.gz");
    rm("omnic_test_mm.gz");
    rm("omnic_test_mm");
    rm("omnic_test_mm_ref");
    rm(z);
    free(z);
}

/* snappy framing 的 CRC 路径：翻转首个 chunk 的存储 CRC（数据本身完好），
 * 必须由 framing 层校验拦下而非依赖 snappy 解码失败 */
static void test_snappy_crc(void) {
    const omnic_codec_ops *ops = omnic_codec_find("snappy");
    CHECK(ops != NULL);
    if (ops == NULL) return;
    omnic_options o = def_opts();
    o.codec = "snappy";
    omnic_report rep;

    CHECK(omnic_compress(SRC, &o, &rep) == OMNIC_OK);
    char *z = with_ext(SRC, ops->native_ext);
    CHECK(rename(z, "omnic_test_crc.snappy") == 0);
    FILE *f = fopen("omnic_test_crc.snappy", "r+b");
    CHECK(f != NULL);
    if (f != NULL) {
        int c;
        fseek(f, 14, SEEK_SET); /* magic 10 + chunk 头 4 之后 = 首个 chunk 的 CRC 区 */
        c = fgetc(f);
        fseek(f, 14, SEEK_SET);
        fputc(c ^ 0xFF, f);
        fclose(f);
        o.flags = 0;
        CHECK(omnic_decompress("omnic_test_crc.snappy", &o, &rep) == OMNIC_E_DATA);
    }
    rm("omnic_test_crc.snappy");
    rm("omnic_test_crc");
    rm(z);
    free(z);
}

/* ---- lzvn 解码路径（需求 4.4）----
 * 上游无公开工具产出该格式，测试手工构造首块为 bvxn 的 lzfse 流：
 * 块头为 uint32 小端（bvxn 魔数 + n_raw + n_payload），lzvn 载荷 + bvx$ 终止块，
 * 载荷由 lzfse 库内的 lzvn 编码器产出（自带流结束标记），声明见下。 */
size_t lzvn_encode_scratch_size(void);
size_t lzvn_encode_buffer(void *dst, size_t dst_size, const void *src, size_t src_size,
                          void *work);

static void put_le32(FILE *f, uint32_t v) {
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

static void test_lzvn_decode(void) {
    enum { N = 60000 };
    uint8_t *buf = (uint8_t *)malloc(N);
    size_t cap = N + N / 16 + 64;
    uint8_t *pay = (uint8_t *)malloc(cap);
    void *work = malloc(lzvn_encode_scratch_size());
    CHECK(buf != NULL && pay != NULL && work != NULL);
    if (buf == NULL || pay == NULL || work == NULL) goto done;
    for (size_t i = 0; i < N; i++)
        buf[i] = (i % 9 == 0) ? (uint8_t)(i >> 4) : (uint8_t)('a' + (int)(i % 26));
    size_t plen = lzvn_encode_buffer(pay, cap, buf, N, work);
    CHECK(plen > 0 && plen < N); /* 可压缩内容必须真的被压缩 */
    if (plen == 0 || plen >= N) goto done;

    {
        FILE *f = fopen("omnic_test_lzvn.lzvn", "wb");
        FILE *r = fopen("omnic_test_lzvn_ref", "wb");
        CHECK(f != NULL && r != NULL);
        if (f != NULL && r != NULL) {
            put_le32(f, 0x6E787662u); /* "bvxn" */
            put_le32(f, (uint32_t)N);
            put_le32(f, (uint32_t)plen);
            fwrite(pay, 1, plen, f);
            put_le32(f, 0x24787662u); /* "bvx$" */
            fwrite(buf, 1, N, r);
        }
        if (f) fclose(f);
        if (r) fclose(r);
    }
    {
        omnic_options o = def_opts();
        omnic_report rep;
        CHECK(omnic_decompress("omnic_test_lzvn.lzvn", &o, &rep) == OMNIC_OK);
        CHECK(files_equal("omnic_test_lzvn", "omnic_test_lzvn_ref"));
        rm("omnic_test_lzvn"); /* 后续小节重用同一输出名 */
    }

    /* 截断 10 字节：咬掉 bvx$ 终止块 + 载荷尾，bvxn 块校验必须报 E_DATA */
    {
        FILE *f = fopen("omnic_test_lzvn.lzvn", "rb");
        CHECK(f != NULL);
        if (f != NULL) {
            unsigned char all[16 + 65536];
            size_t total = fread(all, 1, sizeof(all), f);
            fclose(f);
            CHECK(total == 12 + plen + 4);
            if (total == 12 + plen + 4) {
                FILE *t = fopen("omnic_test_trunc.lzvn", "wb");
                CHECK(t != NULL);
                if (t != NULL) {
                    fwrite(all, 1, total - 10, t);
                    fclose(t);
                }
            }
        }
        omnic_options o = def_opts();
        omnic_report rep;
        CHECK(omnic_decompress("omnic_test_trunc.lzvn", &o, &rep) == OMNIC_E_DATA);
        rm("omnic_test_trunc.lzvn");
        rm("omnic_test_trunc");
    }

    /* 1 字节边界：库解码器在 n_raw=0 时提前返回（dst_len==0 早退被块校验
     * 判错），零字节 bvzn 块无法表示；1 字节是可解码的最小流 */
    {
        size_t eplen = lzvn_encode_buffer(pay, cap, buf, 1, work);
        CHECK(eplen > 0 && eplen <= 16);
        if (eplen > 0) {
            FILE *f = fopen("omnic_test_lzvn.lzvn", "wb");
            CHECK(f != NULL);
            if (f != NULL) {
                put_le32(f, 0x6E787662u);
                put_le32(f, 1);
                put_le32(f, (uint32_t)eplen);
                fwrite(pay, 1, eplen, f);
                put_le32(f, 0x24787662u);
                fclose(f);
            }
            omnic_options o = def_opts();
            omnic_report rep;
            CHECK(omnic_decompress("omnic_test_lzvn.lzvn", &o, &rep) == OMNIC_OK);
            {
                FILE *chk = fopen("omnic_test_lzvn", "rb"); /* 输出恰 1 字节 */
                CHECK(chk != NULL);
                if (chk != NULL) {
                    int c = fgetc(chk);
                    CHECK(c == (int)buf[0] && fgetc(chk) == EOF);
                    fclose(chk);
                }
            }
        }
    }
    rm("omnic_test_lzvn.lzvn");
    rm("omnic_test_lzvn");
    rm("omnic_test_lzvn_ref");
done:
    free(buf);
    free(pay);
    free(work);
}

/* lzip（需求 4.4）：内嵌 blob 验证内容与截断（blob = "omnic lzip embedded
 * test payload\n" x100 的 .lz v1 封装，dict 2^16）；外加 vendored xz 自带
 * 官方 .lz 语料全量回归（good 8 个全过 / bad+unsupported 10 个全拒，
 * 语义与 xz -dc 对齐）。语料不存在时跳过该段（防非常规构建布局）。 */
static void test_lzip_decode(void) {
    static const uint8_t blob[] = {
        76, 90, 73, 80, 1, 16, 0, 55, 155, 74, 29, 94, 104, 211, 11, 58, 125, 185, 252, 246,
        74, 56, 162, 230, 115, 234, 185, 188, 175, 91, 252, 244, 135, 251, 126, 45, 108, 152,
        201, 63, 0, 39, 136, 253, 224, 215, 86, 152, 102, 72, 229, 90, 216, 72, 39, 148, 171,
        44, 190, 234, 239, 8, 255, 254, 158, 4, 0, 45, 207, 166, 69, 228, 12, 0, 0, 0, 0, 0,
        0, 87, 0, 0, 0, 0, 0, 0, 0};
    static const char pattern[] = "omnic lzip embedded test payload\n"; /* 33 字节 */
    enum { PAYLOAD = 3300 };

    FILE *f = fopen("omnic_test_lz.lz", "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fwrite(blob, 1, sizeof(blob), f);
        fclose(f);
    }
    {
        omnic_options o = def_opts();
        omnic_report rep;
        CHECK(omnic_decompress("omnic_test_lz.lz", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_lz", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            uint8_t *out = (uint8_t *)malloc(PAYLOAD);
            CHECK(out != NULL);
            if (out != NULL) {
                int ok = (fread(out, 1, PAYLOAD, r) == PAYLOAD);
                for (int i = 0; ok && i < 100; i++)
                    if (memcmp(out + i * 33, pattern, 33) != 0) ok = 0;
                CHECK(ok && fgetc(r) == EOF);
                free(out);
            }
            fclose(r);
        }
    }
    rm("omnic_test_lz"); /* 截断用例重用同一输出名 */
    /* 截断 10 字节：咬掉 trailer 的 CRC/尺寸域，必须 E_DATA */
    f = fopen("omnic_test_lz.lz", "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fwrite(blob, 1, sizeof(blob) - 10, f);
        fclose(f);
    }
    {
        omnic_options o = def_opts();
        omnic_report rep;
        CHECK(omnic_decompress("omnic_test_lz.lz", &o, &rep) == OMNIC_E_DATA);
    }
    rm("omnic_test_lz.lz");
    rm("omnic_test_lz");

    static const char *good[] = {"good-1-v0.lz",
                                 "good-1-v0-trailing-1.lz",
                                 "good-1-v1.lz",
                                 "good-1-v1-trailing-1.lz",
                                 "good-1-v1-trailing-2.lz",
                                 "good-2-v0-v1.lz",
                                 "good-2-v1-v0.lz",
                                 "good-2-v1-v1.lz",
                                 NULL};
    static const char *bad[] = {"bad-1-v0-uncomp-size.lz", "bad-1-v1-crc32.lz", "bad-1-v1-dict-1.lz",
                               "bad-1-v1-dict-2.lz",      "bad-1-v1-magic-1.lz", "bad-1-v1-magic-2.lz",
                               "bad-1-v1-member-size.lz", "bad-1-v1-trailing-magic.lz",
                               "bad-1-v1-uncomp-size.lz", "unsupported-1-v234.lz", NULL};
    const char *dir = "../third_party/xz/tests/files/";
    char path[256];
    snprintf(path, sizeof path, "%s%s", dir, good[0]);
    FILE *probe = fopen(path, "rb");
    if (probe == NULL) {
        printf("note: lzip corpus not found, skip corpus checks\n");
        return;
    }
    fclose(probe);
    /* 语料先拷到工作目录再解压，避免输出落进 third_party */
    for (int i = 0; good[i] != NULL; i++) {
        omnic_options o = def_opts();
        omnic_report rep;
        snprintf(path, sizeof path, "%s%s", dir, good[i]);
        CHECK(copy_file(path, "omnic_test_corpus.lz") == 0);
        CHECK(omnic_decompress("omnic_test_corpus.lz", &o, &rep) == OMNIC_OK);
        rm("omnic_test_corpus");
    }
    /* 两 member 内容拼接：good-2-v1-v1 = "Hello\n" + "World!\n" */
    {
        omnic_options o = def_opts();
        omnic_report rep;
        snprintf(path, sizeof path, "%sgood-2-v1-v1.lz", dir);
        CHECK(copy_file(path, "omnic_test_corpus.lz") == 0);
        CHECK(omnic_decompress("omnic_test_corpus.lz", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_corpus", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            char buf[16];
            size_t n = fread(buf, 1, sizeof(buf), r);
            CHECK(n == 13 && memcmp(buf, "Hello\nWorld!\n", 13) == 0);
            fclose(r);
        }
        rm("omnic_test_corpus");
    }
    for (int i = 0; bad[i] != NULL; i++) {
        omnic_options o = def_opts();
        omnic_report rep;
        snprintf(path, sizeof path, "%s%s", dir, bad[i]);
        CHECK(copy_file(path, "omnic_test_corpus.lz") == 0);
        CHECK(omnic_decompress("omnic_test_corpus.lz", &o, &rep) == OMNIC_E_DATA);
        rm("omnic_test_corpus");
    }
    rm("omnic_test_corpus.lz");
}

/* bzip2（需求 4.4）：嵌入流由 Python bz2（libbzip2 参考实现）产出——
 * 小流内容校验、串接流（cat 两流）、5MB 零填充跨 4MB 输出块、截断拒收。 */
static void test_bzip2_decode(void) {
    static const uint8_t blob_a[] = {
        66, 90, 104, 57, 49, 65, 89, 38, 83, 89, 147, 29, 173, 234, 0, 10, 139, 217, 128, 0, 16,
        64, 0, 16, 0, 62, 39, 204, 48, 48, 0, 216, 5, 52, 200, 196, 196, 196, 41, 166, 70, 38, 38,
        32, 82, 170, 6, 70, 158, 154, 155, 83, 97, 22, 194, 47, 34, 47, 194, 47, 2, 44, 136, 178,
        34, 236, 34, 228, 69, 236, 69, 129, 23, 2, 44, 4, 91, 132, 88, 17, 122, 17, 124, 17, 100,
        69, 161, 22, 132, 91, 136, 176, 34, 250, 34, 232, 69, 208, 139, 145, 23, 2, 45, 132, 90,
        17, 119, 17, 100, 69, 200, 139, 248, 187, 146, 41, 194, 132, 132, 152, 237, 111, 80};
    static const uint8_t blob_z[] = {
        66, 90, 104, 57, 49, 65, 89, 38, 83, 89, 174, 205, 220, 147, 0, 40, 40, 68, 0, 192, 0, 0, 4,
        0, 8, 32, 0, 48, 204, 5, 41, 166, 9, 2, 108, 66, 64, 158, 46, 228, 138, 112, 161, 33, 93,
        155, 185, 38};
    static const char pattern[] = "omnic bzip2 embedded test payload\n"; /* 34 字节 */
    enum { A_PAYLOAD = 6800, Z_PAYLOAD = 5 * 1024 * 1024 };
    omnic_options o = def_opts();
    omnic_report rep;

    /* 单流内容 */
    {
        FILE *f = fopen("omnic_test_bz.bz2", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(blob_a, 1, sizeof(blob_a), f);
            fclose(f);
        }
        CHECK(omnic_decompress("omnic_test_bz.bz2", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_bz", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            uint8_t *out = (uint8_t *)malloc(A_PAYLOAD);
            CHECK(out != NULL);
            if (out != NULL) {
                int ok = (fread(out, 1, A_PAYLOAD, r) == A_PAYLOAD);
                for (int i = 0; ok && i < 200; i++)
                    if (memcmp(out + i * 34, pattern, 34) != 0) ok = 0;
                CHECK(ok && fgetc(r) == EOF);
                free(out);
            }
            fclose(r);
        }
        rm("omnic_test_bz");
    }
    /* 串接流：同一小流两份 */
    {
        FILE *f = fopen("omnic_test_bz.bz2", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(blob_a, 1, sizeof(blob_a), f);
            fwrite(blob_a, 1, sizeof(blob_a), f);
            fclose(f);
        }
        CHECK(omnic_decompress("omnic_test_bz.bz2", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_bz", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            uint8_t *out = (uint8_t *)malloc(2 * A_PAYLOAD);
            CHECK(out != NULL);
            if (out != NULL) {
                int ok = (fread(out, 1, 2 * A_PAYLOAD, r) == 2 * A_PAYLOAD);
                for (int i = 0; ok && i < 400; i++)
                    if (memcmp(out + i * 34, pattern, 34) != 0) ok = 0;
                CHECK(ok && fgetc(r) == EOF);
                free(out);
            }
            fclose(r);
        }
        rm("omnic_test_bz");
    }
    /* 5MB 零填充：解码输出跨 4MB flush 边界 */
    {
        FILE *f = fopen("omnic_test_bz.bz2", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(blob_z, 1, sizeof(blob_z), f);
            fclose(f);
        }
        CHECK(omnic_decompress("omnic_test_bz.bz2", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_bz", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            size_t left = Z_PAYLOAD;
            int ok = 1;
            uint8_t *buf = (uint8_t *)malloc(65536);
            CHECK(buf != NULL);
            if (buf != NULL) {
                while (left > 0) {
                    size_t c = left < 65536 ? left : 65536;
                    if (fread(buf, 1, c, r) != c) {
                        ok = 0;
                        break;
                    }
                    for (size_t i = 0; i < c; i++)
                        if (buf[i] != 0) {
                            ok = 0;
                            break;
                        }
                    left -= c;
                }
                CHECK(ok && fgetc(r) == EOF);
                free(buf);
            }
            fclose(r);
        }
        rm("omnic_test_bz");
    }
    /* 截断 10 字节：拒收 */
    {
        FILE *f = fopen("omnic_test_bz.bz2", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(blob_a, 1, sizeof(blob_a) - 10, f);
            fclose(f);
        }
        CHECK(omnic_decompress("omnic_test_bz.bz2", &o, &rep) == OMNIC_E_DATA);
    }
    rm("omnic_test_bz.bz2");
    rm("omnic_test_bz");
}

/* compress(.Z)（需求 4.4）：fixtures 由 ncompress 5.0 生成（见 tests/files/
 * README.md）。文本档验证内容与升宽路径；随机档验证表满 + CLEAR；零填充档
 * 验证升宽至 16 位与跨 4MB 输出。格式无校验和：截断只能解出前缀（上游
 * uncompress 同行为），测试如实断言该语义。 */
static void test_compressz_decode(void) {
    const char *dir = "../tests/files/";
    omnic_options o = def_opts();
    omnic_report rep;
    char path[256];

    snprintf(path, sizeof path, "%s%s", dir, "z_text.Z");
    FILE *probe = fopen(path, "rb");
    if (probe == NULL) {
        printf("note: .Z fixtures not found, skip\n");
        return;
    }
    fclose(probe);

    /* 文本档：期望内容 = 模式行 x 400（行号递增，C 侧重构） */
    enum { ZTEXT = 400 * 45 };
    char *expect = (char *)malloc(ZTEXT);
    CHECK(expect != NULL);
    if (expect != NULL) {
        int off = 0;
        for (int i = 0; i < 400; i++)
            off += sprintf(expect + off, "omnic compress .Z fixture payload line %05d\n", i);
        CHECK(off == ZTEXT);

        snprintf(path, sizeof path, "%s%s", dir, "z_text.Z");
        CHECK(copy_file(path, "omnic_test_z.Z") == 0);
        CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_OK);
        {
            FILE *r = fopen("omnic_test_z", "rb");
            CHECK(r != NULL);
            if (r != NULL) {
                char *got = (char *)malloc(ZTEXT);
                CHECK(got != NULL);
                if (got != NULL) {
                    CHECK(fread(got, 1, ZTEXT, r) == ZTEXT && memcmp(got, expect, ZTEXT) == 0);
                    CHECK(fgetc(r) == EOF);
                    free(got);
                }
                fclose(r);
            }
        }

        /* 截断 10 字节：无校验和格式只能解出前缀（omnic 返回 OK 属实然语义） */
        {
            FILE *src = fopen(path, "rb");
            CHECK(src != NULL);
            if (src != NULL) {
                uint8_t buf[8192];
                size_t n = fread(buf, 1, sizeof(buf), src);
                fclose(src);
                CHECK(n == 2126);
                FILE *t = fopen("omnic_test_z.Z", "wb");
                CHECK(t != NULL);
                if (t != NULL) {
                    fwrite(buf, 1, n - 10, t);
                    fclose(t);
                }
            }
            rm("omnic_test_z"); /* 重用输出名，先清残留 */
            CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_OK);
            FILE *r = fopen("omnic_test_z", "rb");
            CHECK(r != NULL);
            if (r != NULL) {
                char *got = (char *)malloc(ZTEXT);
                CHECK(got != NULL);
                if (got != NULL) {
                    size_t got_n = fread(got, 1, ZTEXT, r);
                    CHECK(got_n > 0 && got_n < ZTEXT && fgetc(r) == EOF);
                    CHECK(memcmp(got, expect, got_n) == 0); /* 严格前缀 */
                    free(got);
                }
                fclose(r);
            }
        }
        free(expect);
    }

    /* 随机档（-b 10）：表满 + CLEAR 路径 */
    {
        snprintf(path, sizeof path, "%s%s", dir, "z_rand10.Z");
        CHECK(copy_file(path, "omnic_test_z.Z") == 0);
        rm("omnic_test_z");
        CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_OK);
        snprintf(path, sizeof path, "%s%s", dir, "z_rand10.expected");
        CHECK(files_equal("omnic_test_z", path));
    }

    /* 零填充档（-b 16）：升宽至 16 位 + 输出跨 4MB flush 边界 */
    {
        snprintf(path, sizeof path, "%s%s", dir, "z_zeros.Z");
        CHECK(copy_file(path, "omnic_test_z.Z") == 0);
        rm("omnic_test_z");
        CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_OK);
        FILE *r = fopen("omnic_test_z", "rb");
        CHECK(r != NULL);
        if (r != NULL) {
            enum { ZBYTES = 5 * 1024 * 1024 };
            uint8_t *buf = (uint8_t *)malloc(65536);
            CHECK(buf != NULL);
            size_t left = ZBYTES;
            int ok = 1;
            if (buf != NULL) {
                while (left > 0) {
                    size_t c = left < 65536 ? left : 65536;
                    if (fread(buf, 1, c, r) != c) {
                        ok = 0;
                        break;
                    }
                    for (size_t i = 0; i < c; i++)
                        if (buf[i] != 0) {
                            ok = 0;
                            break;
                        }
                    left -= c;
                }
                CHECK(ok && fgetc(r) == EOF);
                free(buf);
            }
            fclose(r);
        }
    }

    /* 空 .Z（仅 3 字节头）-> 空输出；坏 flags（bits 5-6 置位）-> E_DATA */
    {
        static const uint8_t empty_z[] = {0x1F, 0x9D, 0x8C};
        FILE *f = fopen("omnic_test_z.Z", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(empty_z, 1, 3, f);
            fclose(f);
        }
        rm("omnic_test_z");
        CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_OK);
        {
            FILE *r = fopen("omnic_test_z", "rb");
            CHECK(r != NULL);
            if (r != NULL) {
                CHECK(fgetc(r) == EOF);
                fclose(r);
            }
        }
        rm("omnic_test_z");
        f = fopen("omnic_test_z.Z", "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            uint8_t bad = 0xEC;
            fwrite(empty_z, 1, 2, f);
            fwrite(&bad, 1, 1, f);
            fclose(f);
        }
        CHECK(omnic_decompress("omnic_test_z.Z", &o, &rep) == OMNIC_E_DATA);
    }
    rm("omnic_test_z.Z");
    rm("omnic_test_z");
}

/* DoD #6（SYS-CMP-002）：决策延迟——4MB 特征提取 + 分类查表 ≤ 50ms（不含磁盘 I/O）。
 * Windows CRT 的 clock() 粒度 ~15ms，单次几毫秒测不出，跑多轮取平均。 */
static void test_decision_latency(void) {
    enum { N = 4u * 1024u * 1024u, ROUNDS = 64 };
    uint8_t *buf = (uint8_t *)malloc(N);
    CHECK(buf != NULL);
    if (buf == NULL) return;
    for (size_t i = 0; i < N; i++)
        buf[i] = (uint8_t)(rng() >> 24); /* 全域字节，熵计算最重负载 */
    clock_t t0 = clock();
    omnic_class cls = OMNIC_CLS_UNKNOWN;
    const omnic_policy_entry *e = NULL;
    for (int i = 0; i < ROUNDS; i++) {
        double h = omnic_entropy(buf, N); /* 特征提取主体（api 层 UNKNOWN->RANDOM 依此细分） */
        cls = omnic_classify("noext", buf, N);
        e = omnic_policy_lookup(cls, OMNIC_PROFILE_BALANCED);
        (void)h;
    }
    clock_t t1 = clock();
    CHECK(e != NULL);
    double ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC / ROUNDS;
    printf("decision latency (4MB sample, %d rounds avg): %.2f ms\n", ROUNDS, ms);
    CHECK(ms < 50.0);
    free(buf);
}

int main(void) {
    cleanup_all();
    make_src();
    test_policy();
    test_feature();
    test_trial();
    test_detect();
    test_auto_and_skip();
    test_roundtrip_full("zstd");
    test_roundtrip_full("lz4");
    test_roundtrip_full("xz");
    test_roundtrip_full("zlib");
    test_roundtrip_full("zlib-ng");
    test_roundtrip_full("libdeflate");
    test_roundtrip_full("brotli");
    test_roundtrip_full("snappy");
    test_roundtrip_full("lzfse");
    test_roundtrip_full("zlib-stream");
    test_roundtrip_full("raw-deflate");
    test_roundtrip_full("lzma");
    test_truncated("zstd");
    test_truncated("zlib");
    test_truncated("brotli");
    test_truncated("snappy");
    test_truncated("lzfse");
    test_truncated("zlib-stream");
    test_truncated("raw-deflate");
    test_truncated("lzma");
    test_empty_roundtrip();
    test_gzip_multimember();
    test_snappy_crc();
    test_lzvn_decode();
    test_lzip_decode();
    test_bzip2_decode();
    test_compressz_decode();
    test_decision_latency();
    make_zeros("omnic_test_bigz", (4u << 20) + 700000); /* 解压跨 4MB 输出 flush */
    make_random("omnic_test_bigr", 5u << 20);           /* 产物超 4MB 输入块 */
    test_big_roundtrip("zstd");
    test_big_roundtrip("lz4");
    test_big_roundtrip("xz");
    test_big_roundtrip("zlib");
    test_big_roundtrip("zlib-ng");
    test_big_roundtrip("libdeflate");
    test_big_roundtrip("brotli");
    test_big_roundtrip("snappy");
    test_big_roundtrip("lzfse");
    test_big_roundtrip("zlib-stream");
    test_big_roundtrip("raw-deflate");
    test_big_roundtrip("lzma");
    rm(SRC);
    rm("omnic_test_rand.png");
    rm("omnic_test_rand.png.zst");
    rm("omnic_test_bigz");
    rm("omnic_test_bigr");
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
