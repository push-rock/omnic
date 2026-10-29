#include "core/registry.h"

#include <string.h>

/* 一期编入的 codec 由 CMake 宏控制（SW-CODEC-005），
 * 核心族默认开（需求 4.5 节）。 */
static const omnic_codec_ops *const g_codecs[] = {
#ifdef OMNIC_CODEC_ZSTD
    &omnic_codec_zstd,
#endif
#ifdef OMNIC_CODEC_LZ4
    &omnic_codec_lz4,
#endif
#ifdef OMNIC_CODEC_XZ
    &omnic_codec_xz,
#endif
    /* gzip 格式的解压入口取注册顺序第一个：zlib 的 inflate 是最标准的实现，
     * zlib-ng / libdeflate 产出同格式流但各自保留 decompress 能力。 */
#ifdef OMNIC_CODEC_ZLIB
    &omnic_codec_zlib,
#endif
#ifdef OMNIC_CODEC_ZLIB_NG
    &omnic_codec_zlibng,
#endif
#ifdef OMNIC_CODEC_LIBDEFLATE
    &omnic_codec_libdeflate,
#endif
#ifdef OMNIC_CODEC_BROTLI
    &omnic_codec_brotli,
#endif
#ifdef OMNIC_CODEC_SNAPPY
    &omnic_codec_snappy,
#endif
#ifdef OMNIC_CODEC_LZFSE
    &omnic_codec_lzfse,
#endif
};

#define G_CODEC_N (sizeof(g_codecs) / sizeof(g_codecs[0]))

size_t omnic_codec_count(void) { return G_CODEC_N; }

const omnic_codec_ops *omnic_codec_at(size_t i) { return i < G_CODEC_N ? g_codecs[i] : NULL; }

const omnic_codec_ops *omnic_codec_find(const char *name) {
    for (size_t i = 0; i < G_CODEC_N; i++) {
        if (strcmp(g_codecs[i]->name, name) == 0) return g_codecs[i];
    }
    return NULL;
}

const omnic_codec_ops *omnic_codec_for_fmt(omnic_fmt_id fmt) {
    for (size_t i = 0; i < G_CODEC_N; i++) {
        if (g_codecs[i]->fmt == fmt && g_codecs[i]->can_decompress) return g_codecs[i];
    }
    return NULL;
}
