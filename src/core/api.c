/* libomnic 公共 API（INT-LIB-001）：CLI 与二期 GUI 的共同入口。 */
#include <string.h>

#include "core/dispatcher.h"
#include "core/util.h"

omnic_status omnic_compress(const char *path, const omnic_options *o, omnic_report *rep) {
    omnic_report tmp;
    if (path == NULL || o == NULL) return OMNIC_E_USAGE;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    return omnic_run_compress(path, o, rep);
}

omnic_status omnic_decompress(const char *path, const omnic_options *o, omnic_report *rep) {
    omnic_report tmp;
    if (path == NULL || o == NULL) return OMNIC_E_USAGE;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    return omnic_run_decompress(path, o, rep);
}

omnic_status omnic_analyze(const char *path, omnic_report *rep) {
    omnic_report tmp;
    if (path == NULL) return OMNIC_E_USAGE;
    if (rep == NULL) rep = &tmp;
    memset(rep, 0, sizeof(*rep));
    return omnic_run_analyze(path, rep);
}

const char *omnic_status_str(omnic_status s) {
    switch (s) {
        case OMNIC_OK: return "ok";
        case OMNIC_E_USAGE: return "usage error";
        case OMNIC_E_DATA: return "data error";
        case OMNIC_E_SYSTEM: return "system error";
    }
    return "?";
}

const char *omnic_profile_str(omnic_profile p) {
    switch (p) {
        case OMNIC_PROFILE_FAST_COMPRESS: return "fast_compress";
        case OMNIC_PROFILE_FAST_DECOMPRESS: return "fast_decompress";
        case OMNIC_PROFILE_BEST_RATIO: return "best_ratio";
        case OMNIC_PROFILE_BALANCED: return "balanced";
        default: return "?";
    }
}

const char *omnic_class_str(omnic_class c) {
    switch (c) {
        case OMNIC_CLS_TEXT: return "text";
        case OMNIC_CLS_EXEC: return "executable";
        case OMNIC_CLS_STRUCTURED: return "structured";
        case OMNIC_CLS_MEDIA_RAW: return "raw media";
        case OMNIC_CLS_ALREADY_COMPRESSED: return "already compressed";
        case OMNIC_CLS_RANDOM: return "high entropy";
        case OMNIC_CLS_NTEXT: return "text/natural";
        case OMNIC_CLS_CODE: return "text/code";
        case OMNIC_CLS_GRAPHIC: return "media/graphic";
        case OMNIC_CLS_PHOTO: return "media/photo";
        case OMNIC_CLS_UNKNOWN: return "unknown";
        default: return "?";
    }
}

const char *omnic_fmt_str(omnic_fmt_id f) {
    switch (f) {
        case OMNIC_FMT_GZIP: return "gzip";
        case OMNIC_FMT_ZLIB: return "zlib";
        case OMNIC_FMT_RAW_DEFLATE: return "deflate";
        case OMNIC_FMT_ZSTD: return "zstd";
        case OMNIC_FMT_XZ: return "xz";
        case OMNIC_FMT_LZMA_ALONE: return "lzma";
        case OMNIC_FMT_LZIP: return "lzip";
        case OMNIC_FMT_BZIP2: return "bzip2";
        case OMNIC_FMT_LZ4: return "lz4";
        case OMNIC_FMT_BROTLI: return "brotli";
        case OMNIC_FMT_SNAPPY: return "snappy";
        case OMNIC_FMT_COMPRESS_Z: return "compress";
        case OMNIC_FMT_LZFSE: return "lzfse";
        case OMNIC_FMT_LZVN: return "lzvn";
        case OMNIC_FMT_LZHAM: return "lzham";
        case OMNIC_FMT_LZAV: return "lzav";
        default: return "unknown";
    }
}

const char *omnic_version_str(void) { return OMNIC_VERSION; }
