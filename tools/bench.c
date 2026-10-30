/* omnic_bench（A7 / TST-PERF-001）：适配层吞吐损耗基准 + 压缩器性能数据。
 *
 * 口径：对每个可压缩 codec 在同一语料上测两条路径——
 *   base  = 同库直调（one-shot API，或与适配层相同的喂入边界），
 *           读源文件 -> 内存压缩 -> 写产物文件；
 *   adapt = omnic_codec_ops 流式全路径（reader/writer 分块抽象）。
 * 两条路径文件 I/O 对称，产物逐字节对拍，耗时差即"适配层损耗"。
 * snappy 的 framing/CRC 容器成本在基线中同样存在（口径对消）。
 * decode-only codec 无压缩产物可比，不参与本基准。
 *
 * 用法：omnic_bench [--size MiB] [--rounds N] [--csv FILE] [--check pct]
 *   --check：压缩与解压损耗全部 <= pct% 时退出 0，否则退出 1（CI 门禁用）。
 * 语料固定种子生成（混合文本/JSON/低熵/半随机/纯随机），结果可复现。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "core/blockio.h"
#include "core/util.h" /* omnic_now_sec：跨平台单调时钟（含 POSIX 功能宏处理） */

/* ---- 库头（按启用 codec 条件引入，与 adapter 相同的头） ---- */
#ifdef OMNIC_CODEC_ZSTD
#include <zstd.h>
#endif
#ifdef OMNIC_CODEC_LZ4
#include <lz4frame.h>
#endif
#ifdef OMNIC_CODEC_ZLIB
#include <zlib.h>
#endif
#ifdef OMNIC_CODEC_LIBDEFLATE
#include <libdeflate.h>
#endif
#ifdef OMNIC_CODEC_BROTLI
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif
#ifdef OMNIC_CODEC_XZ
#include <lzma.h>
#endif
#ifdef OMNIC_CODEC_SNAPPY
#include <snappy-c.h>
#endif
#ifdef OMNIC_CODEC_LZFSE
#include <lzfse.h>
#endif

#define SRC_FILE "bench_src.tmp"
#define OUT_A "bench_out_a.tmp" /* 适配层产物 */
#define OUT_B "bench_out_b.tmp" /* 基线产物 */
#define DEC_A "bench_dec_a.tmp" /* 适配层解压输出 */
#define DEC_B "bench_dec_b.tmp" /* 基线解压输出 */

/* ---- 小工具 ---- */

static void die(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p == NULL) die("out of memory (%zu bytes)", n);
    return p;
}

static uint8_t *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) die("open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) die("seek %s", path);
    long sz = ftell(f);
    if (sz < 0) die("tell %s", path);
    rewind(f);
    uint8_t *buf = (uint8_t *)xmalloc((size_t)sz);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("read %s", path);
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static void write_whole(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) die("create %s", path);
    if (n > 0 && fwrite(p, 1, n, f) != n) die("write %s", path);
    fclose(f);
}

static int files_identical(const char *a, const char *b) {
    size_t la, lb;
    uint8_t *pa = read_whole(a, &la);
    uint8_t *pb = read_whole(b, &lb);
    int same = (la == lb && (la == 0 || memcmp(pa, pb, la) == 0));
    free(pa);
    free(pb);
    return same;
}

/* ---- 语料（固定种子，混合类型覆盖不同压缩器强项） ---- */

static unsigned g_rng = 20261030u;
static unsigned rnd(void) { return g_rng = g_rng * 1103515245u + 12345u; }

static size_t gen_text(uint8_t *p, size_t cap) {
    static const char *words[] = {"compression", "omnic",  "codec", "policy", "stream", "entropy",
                                  "magic",      "format", "block", "zstd",   "benchmark", "ratio"};
    size_t w = 0;
    while (w + 16 < cap) {
        int n = 6 + (int)(rnd() % 8);
        for (int i = 0; i < n; i++) {
            const char *s = words[rnd() % 12];
            size_t l = strlen(s);
            if (w + l + 2 > cap) break;
            memcpy(p + w, s, l);
            w += l;
            p[w++] = (i == n - 1) ? '\n' : ' ';
        }
    }
    return w;
}

static size_t gen_json(uint8_t *p, size_t cap) {
    size_t w = 0;
    while (w + 96 < cap) {
        w += (size_t)sprintf((char *)p + w,
                             "{\"id\":%u,\"name\":\"item-%u\",\"tags\":[\"alpha\",\"beta\"],"
                             "\"price\":%u.%02u,\"stock\":%u}\n",
                             rnd() % 100000u, rnd() % 1000u, rnd() % 900u + 1u, rnd() % 100u,
                             rnd() % 5000u);
    }
    return w;
}

static size_t gen_lowent(uint8_t *p, size_t cap) {
    size_t w = 0;
    while (w + 8 < cap) {
        if (rnd() & 1) {
            memset(p + w, 0, 64);
            w += 64;
        } else {
            for (int i = 0; i < 64; i += 4) memcpy(p + w + i, "ABCD", 4);
            w += 64;
        }
    }
    return w < cap ? w : cap;
}

static size_t gen_semirand(uint8_t *p, size_t cap) {
    size_t w = 0;
    while (w + 4 <= cap) {
        uint8_t b = (uint8_t)(rnd() >> 24);
        memset(p + w, b, 4); /* 4 字节字典粒度：LZ 家族可压、熵仍高 */
        w += 4;
    }
    return w;
}

static void gen_corpus(uint8_t *p, size_t n) {
    size_t off = 0;
    size_t seg;
    seg = n * 40 / 100;
    off += gen_text(p + off, seg);
    seg = n * 20 / 100;
    off += gen_json(p + off, seg);
    seg = n * 15 / 100;
    off += gen_lowent(p + off, seg);
    seg = n * 15 / 100;
    off += gen_semirand(p + off, seg);
    while (off < n) p[off++] = (uint8_t)(rnd() >> 24); /* 尾段纯随机 */
}

/* ---- 各 codec 基线：内存 one-shot / 与适配层对齐的直调 ---- */

#ifdef OMNIC_CODEC_ZSTD
/* 流式喂入（4MB 边界与适配层一致）保证产物逐字节可对拍 */
static size_t base_zstd_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    if (c == NULL) die("zstd cctx");
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, lvl);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
    size_t produced = 0;
    for (size_t off = 0;; off += 4u * 1024 * 1024) {
        size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
        ZSTD_inBuffer ib = {s + off, chunk, 0};
        int last = (off + chunk >= n);
        for (;;) {
            ZSTD_outBuffer ob = {d + produced, cap - produced, 0};
            size_t r = ZSTD_compressStream2(c, &ob, &ib, last ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(r)) die("zstd compress: %s", ZSTD_getErrorName(r));
            produced += ob.pos;
            if (last && r == 0) goto done;
            if (ib.pos == ib.size) break;
        }
        if (last) break;
    }
done:
    ZSTD_freeCCtx(c);
    return produced;
}
static size_t base_zstd_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    if (ctx == NULL) die("zstd dctx");
    ZSTD_inBuffer ib = {s, n, 0};
    size_t produced = 0;
    for (;;) {
        ZSTD_outBuffer ob = {d + produced, cap - produced, 0};
        size_t r = ZSTD_decompressStream(ctx, &ob, &ib);
        if (ZSTD_isError(r)) die("zstd decompress");
        produced += ob.pos;
        if (r == 0) break;
        if (ib.pos == ib.size && ob.pos == 0) die("zstd dst");
    }
    ZSTD_freeDCtx(ctx);
    return produced;
}
#endif

#ifdef OMNIC_CODEC_LZ4
static size_t base_lz4_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    LZ4F_cctx *cctx;
    LZ4F_preferences_t pref;
    size_t produced = 0, w;
    memset(&pref, 0, sizeof(pref));
    pref.compressionLevel = lvl;
    pref.frameInfo.contentChecksumFlag = 1;
    pref.frameInfo.blockSizeID = LZ4F_max256KB;
    if (LZ4F_isError(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION))) die("lz4 cctx");
    w = LZ4F_compressBegin(cctx, d + produced, cap - produced, &pref);
    if (LZ4F_isError(w)) die("lz4 begin: %s", LZ4F_getErrorName(w));
    produced += w;
    for (size_t off = 0; off < n; off += 4u * 1024 * 1024) { /* 喂入边界对齐适配层 */
        size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
        w = LZ4F_compressUpdate(cctx, d + produced, cap - produced, s + off, chunk, NULL);
        if (LZ4F_isError(w)) die("lz4 compress: %s", LZ4F_getErrorName(w));
        produced += w;
    }
    w = LZ4F_compressEnd(cctx, d + produced, cap - produced, NULL);
    if (LZ4F_isError(w)) die("lz4 end: %s", LZ4F_getErrorName(w));
    produced += w;
    LZ4F_freeCompressionContext(cctx);
    return produced;
}
static size_t base_lz4_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    LZ4F_dctx *ctx;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) die("lz4 dctx");
    size_t consumed = 0, total = 0;
    while (consumed < n) {
        size_t in_step = n - consumed;
        size_t out_step = cap - total;
        size_t r = LZ4F_decompress(ctx, d + total, &out_step, s + consumed, &in_step, NULL);
        if (LZ4F_isError(r)) die("lz4 decompress: %s", LZ4F_getErrorName(r));
        total += out_step;
        consumed += in_step;
        if (r == 0) break; /* 帧正常结束 */
        if (in_step == 0 && out_step == 0) die("lz4 no progress");
    }
    LZ4F_freeDecompressionContext(ctx);
    return total;
}
#endif

#ifdef OMNIC_CODEC_ZLIB
/* deflate 家族：与适配层相同的 4MB 喂入边界（保证产物逐字节一致） */
static size_t base_deflate_c(const uint8_t *s, size_t n, int lvl, int wbits, uint8_t *d, size_t cap) {
    z_stream zs;
    uint8_t *ob = (uint8_t *)xmalloc(cap);
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, lvl, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) die("deflateInit2");
    size_t produced = 0;
    for (size_t off = 0;; off += 4u * 1024 * 1024) {
        size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
        zs.next_in = (Bytef *)(s + off);
        zs.avail_in = (uInt)chunk;
        int flush = (off + chunk >= n) ? Z_FINISH : Z_NO_FLUSH;
        for (;;) {
            zs.next_out = ob;
            zs.avail_out = (uInt)cap;
            int r = deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) die("deflate");
            size_t got = cap - zs.avail_out;
            memcpy(d + produced, ob, got);
            produced += got;
            if (r == Z_STREAM_END) goto done;
            if (zs.avail_out != 0) break;
        }
        if (flush == Z_FINISH) break;
    }
done:
    deflateEnd(&zs);
    free(ob);
    return produced;
}
static size_t base_inflate_d(const uint8_t *s, size_t n, int wbits, uint8_t *d, size_t cap) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, wbits) != Z_OK) die("inflateInit2");
    zs.next_in = (Bytef *)s;
    zs.avail_in = (uInt)n;
    size_t produced = 0;
    for (;;) {
        zs.next_out = d + produced;
        zs.avail_out = (uInt)(cap - produced);
        int r = inflate(&zs, Z_NO_FLUSH);
        produced = cap - zs.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) die("inflate");
        if (zs.avail_in == 0 && zs.avail_out != 0) die("inflate truncated");
    }
    inflateEnd(&zs);
    return produced;
}
/* 统一签名桥接（bench_codec 函数指针无 wbits 参数） */
static size_t bc_gzip(const uint8_t *s, size_t n, int l, uint8_t *d, size_t c) {
    return base_deflate_c(s, n, l, 15 + 16, d, c);
}
static size_t bd_gzip(const uint8_t *s, size_t n, uint8_t *d, size_t c) {
    return base_inflate_d(s, n, 15 + 16, d, c);
}
static size_t bc_zlibstream(const uint8_t *s, size_t n, int l, uint8_t *d, size_t c) {
    return base_deflate_c(s, n, l, 15, d, c);
}
static size_t bd_zlibstream(const uint8_t *s, size_t n, uint8_t *d, size_t c) {
    return base_inflate_d(s, n, 15, d, c);
}
static size_t bc_rawdeflate(const uint8_t *s, size_t n, int l, uint8_t *d, size_t c) {
    return base_deflate_c(s, n, l, -15, d, c);
}
static size_t bd_rawdeflate(const uint8_t *s, size_t n, uint8_t *d, size_t c) {
    return base_inflate_d(s, n, -15, d, c);
}
#endif

#ifdef OMNIC_CODEC_ZLIB_NG
/* zlib-ng 基线在 bench_zng.c（头文件冲突需独立编译单元） */
size_t bench_zng_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap);
size_t bench_zng_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap);
#endif

#ifdef OMNIC_CODEC_LIBDEFLATE
static size_t base_libdeflate_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(lvl);
    if (c == NULL) die("libdeflate alloc");
    size_t r = libdeflate_gzip_compress(c, s, n, d, cap);
    libdeflate_free_compressor(c);
    return r;
}
static size_t base_libdeflate_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    struct libdeflate_decompressor *dc = libdeflate_alloc_decompressor();
    if (dc == NULL) die("libdeflate dalloc");
    size_t out = 0;
    if (libdeflate_gzip_decompress(dc, s, n, d, cap, &out) != LIBDEFLATE_SUCCESS) die("libdeflate dec");
    libdeflate_free_decompressor(dc);
    return out;
}
#endif

#ifdef OMNIC_CODEC_BROTLI
static size_t base_brotli_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    BrotliEncoderState *st = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (st == NULL) die("brotli enc");
    BrotliEncoderSetParameter(st, BROTLI_PARAM_QUALITY, (uint32_t)lvl);
    BrotliEncoderSetParameter(st, BROTLI_PARAM_LGWIN, 22);
    size_t produced = 0;
    /* 节奏复刻适配层：数据块全部 PROCESS，EOF 后空输入 FINISH（metablock
     * 规划对收尾时机敏感，口径不对齐会导致产物差异与速度失真） */
    for (size_t off = 0; off < n; off += 4u * 1024 * 1024) {
        const uint8_t *next_in = s + off;
        size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
        size_t avail_in = chunk;
        do {
            uint8_t *next_out = d + produced;
            size_t avail_out = cap - produced;
            if (!BrotliEncoderCompressStream(st, BROTLI_OPERATION_PROCESS, &avail_in, &next_in,
                                             &avail_out, &next_out, NULL))
                die("brotli c");
            produced = cap - avail_out;
        } while (avail_in > 0 || BrotliEncoderHasMoreOutput(st));
    }
    for (;;) {
        size_t avail_in = 0;
        const uint8_t *next_in = s; /* 空输入也需有效指针（库内部解引用 next_in） */
        uint8_t *next_out = d + produced;
        size_t avail_out = cap - produced;
        if (!BrotliEncoderCompressStream(st, BROTLI_OPERATION_FINISH, &avail_in, &next_in,
                                         &avail_out, &next_out, NULL))
            die("brotli fin");
        produced = cap - avail_out;
        if (BrotliEncoderIsFinished(st)) break;
    }
    BrotliEncoderDestroyInstance(st);
    return produced;
}
static size_t base_brotli_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t r = cap;
    if (BrotliDecoderDecompress(n, s, &r, d) != BROTLI_DECODER_RESULT_SUCCESS) die("brotli d");
    return r;
}
#endif

#ifdef OMNIC_CODEC_XZ
/* 流式喂入（4MB 边界 + EOF 后 FINISH，与适配层同节奏）：one-shot buffer API
 * 的块组织与流式不同（产物不一致），口径必须对齐 */
static size_t xz_stream_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap,
                           bool alone) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (alone) {
        lzma_options_lzma opt;
        if (lzma_lzma_preset(&opt, (uint32_t)lvl) != LZMA_OK) die("lzma preset");
        if (lzma_alone_encoder(&strm, &opt) != LZMA_OK) die("lzma enc");
    } else {
        if (lzma_easy_encoder(&strm, (uint32_t)lvl, LZMA_CHECK_CRC64) != LZMA_OK) die("xz enc");
    }
    size_t produced = 0;
    size_t off = 0;
    bool fin = false;
    for (;;) {
        if (strm.avail_in == 0 && !fin) {
            size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
            if (chunk == 0) {
                fin = true;
            } else {
                strm.next_in = s + off;
                strm.avail_in = chunk;
                off += chunk;
            }
        }
        strm.next_out = d + produced;
        strm.avail_out = cap - produced;
        lzma_ret r = lzma_code(&strm, fin ? LZMA_FINISH : LZMA_RUN);
        produced = cap - strm.avail_out;
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) die("xz code");
        if (strm.avail_out == 0) die("xz cap");
    }
    lzma_end(&strm);
    return produced;
}
static size_t base_xz_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    return xz_stream_c(s, n, lvl, d, cap, false);
}
static size_t base_xz_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t in_pos = 0, out_pos = 0;
    uint64_t memlimit = UINT64_MAX;
    if (lzma_stream_buffer_decode(&memlimit, 0, NULL, s, &in_pos, n, d, &out_pos, cap) != LZMA_OK) die("xz dec");
    return out_pos;
}
static size_t base_lzma_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    return xz_stream_c(s, n, lvl, d, cap, true);
}
static size_t base_lzma_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) die("lzma dec");
    strm.next_in = s;
    strm.avail_in = n;
    size_t produced = 0;
    for (;;) {
        strm.next_out = d + produced;
        strm.avail_out = cap - produced;
        lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        produced = cap - strm.avail_out;
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) die("lzma dcode");
        if (strm.avail_out == 0) die("lzma dcap");
    }
    lzma_end(&strm);
    return produced;
}
#endif

#ifdef OMNIC_CODEC_SNAPPY
/* 基线含与适配层相同的 framing/CRC 容器成本（口径对消；CRC 实现与
 * ac_snappy.c 同款硬件指令优先，成本对齐才可比） */
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__SSE4_2__) || defined(_MSC_VER))
#include <nmmintrin.h>
static uint32_t b_crc32c(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {
        c = (uint32_t)_mm_crc32_u64(c, *(const uint64_t *)p);
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        c = (uint32_t)_mm_crc32_u32(c, *(const uint32_t *)p);
        p += 4;
        n -= 4;
    }
    while (n--) c = _mm_crc32_u8(c, *p++);
    return c ^ 0xFFFFFFFFu;
}
#else
static uint32_t b_crc32c(const uint8_t *p, size_t n) {
    static uint32_t tbl[256];
    static int ready = 0;
    if (!ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            tbl[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = tbl[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
#endif
static uint32_t b_mask(uint32_t c) { return ((c >> 15) | (c << 17)) + 0xa282ead8u; }
static void put_le24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}
static size_t base_snappy_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    size_t w = 0;
    memcpy(d, "\xff\x06\x00\x00sNaPpY", 10);
    w = 10;
    uint8_t *cb = (uint8_t *)xmalloc(snappy_max_compressed_length(65536));
    for (size_t off = 0; off < n; off += 65536) {
        size_t chunk = n - off > 65536 ? 65536 : n - off;
        uint32_t crc = b_mask(b_crc32c(s + off, chunk));
        size_t clen = snappy_max_compressed_length(65536);
        if (snappy_compress_with_level((const char *)(s + off), chunk, lvl, (char *)cb, &clen) != SNAPPY_OK)
            die("snappy c");
        const uint8_t *payload = cb;
        size_t plen = clen;
        uint8_t type = 0x00;
        if (clen >= chunk) { /* 无收益退化 uncompressed chunk，同适配层 */
            payload = s + off;
            plen = chunk;
            type = 0x01;
        }
        if (w + 8 + plen > cap) die("snappy cap");
        d[w] = type;
        put_le24(d + w + 1, (uint32_t)(plen + 4));
        d[w + 4] = (uint8_t)(crc >> 24);
        d[w + 5] = (uint8_t)(crc >> 16);
        d[w + 6] = (uint8_t)(crc >> 8);
        d[w + 7] = (uint8_t)crc;
        memcpy(d + w + 8, payload, plen);
        w += 8 + plen;
    }
    free(cb);
    return w;
}
static size_t base_snappy_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t w = 0;
    if (n < 10 || memcmp(s, "\xff\x06\x00\x00sNaPpY", 10) != 0) die("snappy magic");
    size_t off = 10;
    while (off + 8 <= n) { /* 残余不足一个 chunk 头即流结束 */
        uint8_t type = s[off];
        uint32_t total = (uint32_t)s[off + 1] | ((uint32_t)s[off + 2] << 8) | ((uint32_t)s[off + 3] << 16);
        if (total < 4) die("snappy len");
        size_t plen = total - 4;
        if (off + 8 + plen > n) die("snappy body");
        const uint8_t *body = s + off + 8; /* chunk 头 4 + CRC 4 之后才是 payload */
        uint32_t stored = ((uint32_t)s[off + 4] << 24) | ((uint32_t)s[off + 5] << 16) |
                          ((uint32_t)s[off + 6] << 8) | (uint32_t)s[off + 7];
        if (type == 0x00) {
            size_t ulen = cap - w;
            if (snappy_uncompress((const char *)body, plen, (char *)(d + w), &ulen) != SNAPPY_OK) die("snappy u");
            if (b_mask(b_crc32c(d + w, ulen)) != stored) die("snappy crc"); /* 与适配层同成本 */
            w += ulen;
        } else if (type == 0x01) {
            if (w + plen > cap) die("snappy dst");
            memcpy(d + w, body, plen);
            if (b_mask(b_crc32c(d + w, plen)) != stored) die("snappy crc");
            w += plen;
        }
        off += 8 + plen;
    }
    return w;
}
#endif

#ifdef OMNIC_CODEC_LZFSE
static size_t base_lzfse_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    (void)lvl;
    size_t r = lzfse_encode_buffer(d, cap, s, n, NULL);
    if (r == 0) die("lzfse enc");
    return r;
}
static size_t base_lzfse_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t r = lzfse_decode_buffer(d, cap, s, n, NULL);
    if (r != cap) die("lzfse dec"); /* cap = 原文大小，一次到位 */
    return r;
}
#endif

/* ---- 框架 ---- */

typedef size_t (*base_c_fn)(const uint8_t *, size_t, int, uint8_t *, size_t);
typedef size_t (*base_d_fn)(const uint8_t *, size_t, uint8_t *, size_t);

typedef struct {
    const omnic_codec_ops *ops;
    const char *label; /* 展示名（区分 zlib 家族三格式） */
    base_c_fn bc;
    base_d_fn bd;
    const int *levels;
    int nlevels;
} bench_codec;

/* 走适配层全路径：文件 -> ops -> 文件，返回耗时秒 */
static double adapt_run(const omnic_codec_ops *ops, const char *in_path, const char *out_path, int lvl,
                        bool compress) {
    double t0 = omnic_now_sec();
    FILE *fin = fopen(in_path, "rb");
    FILE *fout = fopen(out_path, "wb");
    if (fin == NULL || fout == NULL) die("open %s", in_path);
    omnic_reader r;
    omnic_writer w;
    omnic_reader_init(&r, fin);
    omnic_writer_init(&w, fout);
    omnic_status st = compress ? ops->compress(&r, &w, lvl, 0) : ops->decompress(&r, &w);
    omnic_reader_fini(&r);
    fclose(fin);
    fclose(fout);
    if (st != OMNIC_OK) die("adapt %s failed: %s", compress ? "compress" : "decompress", omnic_last_error());
    return omnic_now_sec() - t0;
}

/* 基线路径：读文件 -> 内存直调 -> 写文件，返回耗时秒与产物大小 */
static double base_run(base_c_fn bc, base_d_fn bd, const char *in_path, const char *out_path, int lvl,
                       bool compress, size_t in_len, size_t *out_len) {
    double t0 = omnic_now_sec();
    size_t n;
    uint8_t *src = read_whole(in_path, &n);
    uint8_t *dst;
    size_t dn;
    if (compress) {
        dst = (uint8_t *)xmalloc(n + n / 2 + 4096);
        dn = bc(src, n, lvl, dst, n + n / 2 + 4096);
    } else {
        dst = (uint8_t *)xmalloc(in_len);
        dn = bd(src, n, dst, in_len);
    }
    write_whole(out_path, dst, dn);
    free(src);
    free(dst);
    *out_len = dn;
    return omnic_now_sec() - t0;
}

static double median3(double a, double b, double c) {
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) { double t = b; b = c; c = t; }
    if (a > b) b = a;
    return b;
}

/* 轮数 1 取单值，2 取均值，3 取中位（上限 3） */
static double pick(double *v, int rounds) {
    if (rounds == 1) return v[0];
    if (rounds == 2) return (v[0] + v[1]) / 2.0;
    return median3(v[0], v[1], v[2]);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* 崩溃时也保留进度输出，便于定位 */
    size_t mib = 32;
    int rounds = 3;
    const char *csv_path = NULL;
    double check_pct = 0;
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) mib = (size_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csv_path = argv[++i];
        else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) check_pct = atof(argv[++i]);
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
        else die("usage: omnic_bench [--size MiB] [--rounds N] [--csv FILE] [--check pct] [--only name]");
    }
    if (rounds < 1) rounds = 1;
    if (rounds > 3) rounds = 3;

    size_t n = mib * 1024 * 1024;
    uint8_t *corpus = (uint8_t *)xmalloc(n);
    gen_corpus(corpus, n);
    write_whole(SRC_FILE, corpus, n);
    free(corpus);
    size_t src_len = n;

    /* 注册表驱动的 codec 集合（按启用条件组装） */
    static const int lv_zstd[] = {-3, 1, 3, 6, 9, 12, 16, 19};
    static const int lv_lz4[] = {0, 3, 9, 12};
    static const int lv_zlib[] = {1, 6, 9};
    static const int lv_ldef[] = {1, 6, 12};
    static const int lv_brotli[] = {1, 5, 9, 11};
    static const int lv_xz[] = {0, 3, 6, 9};
    static const int lv_snappy[] = {1, 2};
    static const int lv_one[] = {0};
    bench_codec cs[16];
    int ncs = 0;
#ifdef OMNIC_CODEC_ZSTD
    cs[ncs++] = (bench_codec){&omnic_codec_zstd, "zstd", base_zstd_c, base_zstd_d, lv_zstd, 8};
#endif
#ifdef OMNIC_CODEC_LZ4
    cs[ncs++] = (bench_codec){&omnic_codec_lz4, "lz4", base_lz4_c, base_lz4_d, lv_lz4, 4};
#endif
#ifdef OMNIC_CODEC_ZLIB
    cs[ncs++] = (bench_codec){&omnic_codec_zlib, "gzip", bc_gzip, bd_gzip, lv_zlib, 3};
    cs[ncs++] = (bench_codec){&omnic_codec_zlibstream, "zlib", bc_zlibstream, bd_zlibstream, lv_zlib, 3};
    cs[ncs++] = (bench_codec){&omnic_rawdeflate, "raw-deflate", bc_rawdeflate, bd_rawdeflate, lv_zlib, 3};
#endif
#ifdef OMNIC_CODEC_ZLIB_NG
    cs[ncs++] = (bench_codec){&omnic_codec_zlibng, "zlib-ng", bench_zng_c, bench_zng_d, lv_zlib, 3};
#endif
#ifdef OMNIC_CODEC_LIBDEFLATE
    cs[ncs++] = (bench_codec){&omnic_codec_libdeflate, "libdeflate", base_libdeflate_c, base_libdeflate_d, lv_ldef, 3};
#endif
#ifdef OMNIC_CODEC_BROTLI
    cs[ncs++] = (bench_codec){&omnic_codec_brotli, "brotli", base_brotli_c, base_brotli_d, lv_brotli, 4};
#endif
#ifdef OMNIC_CODEC_XZ
    cs[ncs++] = (bench_codec){&omnic_codec_xz, "xz", base_xz_c, base_xz_d, lv_xz, 4};
    cs[ncs++] = (bench_codec){&omnic_codec_lzma, "lzma", base_lzma_c, base_lzma_d, lv_zlib, 3};
#endif
#ifdef OMNIC_CODEC_SNAPPY
    cs[ncs++] = (bench_codec){&omnic_codec_snappy, "snappy", base_snappy_c, base_snappy_d, lv_snappy, 2};
#endif
#ifdef OMNIC_CODEC_LZFSE
    cs[ncs++] = (bench_codec){&omnic_codec_lzfse, "lzfse", base_lzfse_c, base_lzfse_d, lv_one, 1};
#endif

    FILE *csv = NULL;
    if (csv_path != NULL) {
        csv = fopen(csv_path, "wb");
        if (csv == NULL) die("create %s", csv_path);
        fprintf(csv,
                "codec,format,level,ratio_pct,comp_base_mbs,comp_adapt_mbs,comp_loss_pct,"
                "dec_base_mbs,dec_adapt_mbs,dec_loss_pct\n");
    }

    printf("omnic_bench: %zu MiB corpus, %d round(s) (median)\n", mib, rounds);
    printf("%-12s %-12s %5s %7s %26s %26s\n", "codec", "format", "level", "ratio", "compress MB/s",
           "decompress MB/s");
    printf("%-12s %-12s %5s %7s %26s %26s\n", "", "", "", "", "base -> adapt (loss%)",
           "base -> adapt (loss%)");

    double worst_c = 0, worst_d = 0;
    for (int ci = 0; ci < ncs; ci++) {
        bench_codec *bc = &cs[ci];
        if (only != NULL && strcmp(only, bc->label) != 0) continue;
        for (int li = 0; li < bc->nlevels; li++) {
            int lvl = bc->levels[li];
            double ac[3], ab[3], dc[3], db[3]; /* adapt/base × 压缩/解压 */
            size_t comp_len = 0;
            for (int r = 0; r < rounds; r++) {
                size_t dlen = 0;
                /* 轮次交替测量顺序：固定的"适配层先、基线后"会让基线吃到
                 * L3 缓存残留红利（libdeflate 解压实测虚高 ~6%），中位数吸收 */
                if (r & 1) {
                    ab[r] = base_run(bc->bc, bc->bd, SRC_FILE, OUT_B, lvl, true, src_len, &comp_len);
                    ac[r] = adapt_run(bc->ops, SRC_FILE, OUT_A, lvl, true);
                } else {
                    ac[r] = adapt_run(bc->ops, SRC_FILE, OUT_A, lvl, true);
                    ab[r] = base_run(bc->bc, bc->bd, SRC_FILE, OUT_B, lvl, true, src_len, &comp_len);
                }
                if (!files_identical(OUT_A, OUT_B))
                    printf("  WARN: %s level %d product mismatch (base vs adapt; "
                           "flush boundaries differ, both decode to identical output)\n",
                           bc->label, lvl);
                if (r & 1) {
                    db[r] = base_run(bc->bc, bc->bd, OUT_A, DEC_B, lvl, false, src_len, &dlen);
                    dc[r] = adapt_run(bc->ops, OUT_A, DEC_A, lvl, false);
                } else {
                    dc[r] = adapt_run(bc->ops, OUT_A, DEC_A, lvl, false);
                    db[r] = base_run(bc->bc, bc->bd, OUT_A, DEC_B, lvl, false, src_len, &dlen);
                }
                if (!files_identical(DEC_A, DEC_B) || !files_identical(DEC_A, SRC_FILE))
                    printf("  WARN: %s level %d decode mismatch\n", bc->label, lvl);
            }
            double c_base = (double)src_len / 1048576.0 / pick(ab, rounds);
            double c_adapt = (double)src_len / 1048576.0 / pick(ac, rounds);
            double d_base = (double)src_len / 1048576.0 / pick(db, rounds);
            double d_adapt = (double)src_len / 1048576.0 / pick(dc, rounds);
            double c_loss = (c_base - c_adapt) / c_base * 100.0;
            double d_loss = (d_base - d_adapt) / d_base * 100.0;
            if (c_loss > worst_c) worst_c = c_loss;
            if (d_loss > worst_d) worst_d = d_loss;
            double ratio = (double)comp_len / (double)src_len * 100.0;
            printf("%-12s %-12s %5d %6.1f%% %9.0f -> %-9.0f (%+5.1f%%) %9.0f -> %-9.0f (%+5.1f%%)\n",
                   bc->label, bc->ops->name, lvl, ratio, c_base, c_adapt, -c_loss, d_base, d_adapt,
                   -d_loss);
            if (csv != NULL)
                fprintf(csv, "%s,%s,%d,%.2f,%.1f,%.1f,%.2f,%.1f,%.1f,%.2f\n", bc->label, bc->ops->name,
                        lvl, ratio, c_base, c_adapt, c_loss, d_base, d_adapt, d_loss);
        }
    }

    remove(SRC_FILE);
    remove(OUT_A);
    remove(OUT_B);
    remove(DEC_A);
    remove(DEC_B);
    if (csv != NULL) fclose(csv);

    printf("worst adapter loss: compress %.2f%%, decompress %.2f%%\n", worst_c, worst_d);
    if (check_pct > 0) {
        if (worst_c <= check_pct && worst_d <= check_pct) {
            printf("CHECK PASS (<= %.1f%%)\n", check_pct);
            return 0;
        }
        printf("CHECK FAIL (> %.1f%%)\n", check_pct);
        return 1;
    }
    return 0;
}
