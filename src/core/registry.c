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
