#include "detect/magic.h"

#include <string.h>

typedef struct {
    uint8_t bytes[8];
    uint8_t len;
    omnic_fmt_id fmt;
} omnic_magic;

/* 前缀表（需求 4.4 节）：长前缀在前，保证精确优先。
 * zlib stream 的 CMF/FLG 组合仅枚举常见值（78 01/5E/9C/DA）。 */
static const omnic_magic g_magic[] = {
    {{0xFF, 0x06, 0x00, 0x00, 0x53, 0x4E, 0x50, 0x41}, 8, OMNIC_FMT_SNAPPY},   /* sNaPpY framed */
    {{0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, 6, OMNIC_FMT_XZ},                   /* \xfd7zXZ\0 */
    {{0x28, 0xB5, 0x2F, 0xFD}, 4, OMNIC_FMT_ZSTD},
    {{0x4C, 0x5A, 0x49, 0x50}, 4, OMNIC_FMT_LZIP},                             /* "LZIP" */
    {{0x04, 0x22, 0x4D, 0x18}, 4, OMNIC_FMT_LZ4},
    {{0x62, 0x76, 0x78, 0x2D}, 4, OMNIC_FMT_LZFSE},                            /* "bvx-" */
    {{0x42, 0x5A, 0x68}, 3, OMNIC_FMT_BZIP2},                                  /* "BZh" */
    {{0x62, 0x76, 0x6E}, 3, OMNIC_FMT_LZVN},                                   /* "bvn" */
    {{0x5D, 0x00, 0x00}, 3, OMNIC_FMT_LZMA_ALONE},                             /* props 0x5D + size */
    {{0x1F, 0x8B}, 2, OMNIC_FMT_GZIP},
    {{0x1F, 0x9D}, 2, OMNIC_FMT_COMPRESS_Z},
    {{0x78, 0x01}, 2, OMNIC_FMT_ZLIB},
    {{0x78, 0x5E}, 2, OMNIC_FMT_ZLIB},
    {{0x78, 0x9C}, 2, OMNIC_FMT_ZLIB},
    {{0x78, 0xDA}, 2, OMNIC_FMT_ZLIB},
};

omnic_fmt_id omnic_detect_magic(const uint8_t *head, size_t n) {
    for (size_t i = 0; i < sizeof(g_magic) / sizeof(g_magic[0]); i++) {
        if (n >= g_magic[i].len && memcmp(head, g_magic[i].bytes, g_magic[i].len) == 0) {
            return g_magic[i].fmt;
        }
    }
    return OMNIC_FMT_UNKNOWN;
}

typedef struct {
    const char *ext;
    omnic_fmt_id fmt;
} omnic_ext_fmt;

/* 后缀兜底表（无 magic 的格式：brotli/lzav/lzham/raw-deflate 等） */
static const omnic_ext_fmt g_ext_fmt[] = {
    {"gz", OMNIC_FMT_GZIP},      {"z", OMNIC_FMT_COMPRESS_Z},
    {"zlib", OMNIC_FMT_ZLIB},    {"zz", OMNIC_FMT_RAW_DEFLATE},
    {"deflate", OMNIC_FMT_RAW_DEFLATE},
    {"zst", OMNIC_FMT_ZSTD},     {"xz", OMNIC_FMT_XZ},
    {"lzma", OMNIC_FMT_LZMA_ALONE}, {"lz", OMNIC_FMT_LZIP},
    {"bz2", OMNIC_FMT_BZIP2},    {"lz4", OMNIC_FMT_LZ4},
    {"br", OMNIC_FMT_BROTLI},    {"sz", OMNIC_FMT_SNAPPY},
    {"snappy", OMNIC_FMT_SNAPPY},
    {"lzfse", OMNIC_FMT_LZFSE},  {"lzvn", OMNIC_FMT_LZVN},
    {"lzham", OMNIC_FMT_LZHAM},  {"lzav", OMNIC_FMT_LZAV},
};

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0') return NULL;
    return dot + 1;
}

omnic_fmt_id omnic_detect_ext(const char *path) {
    const char *ext = ext_of(path);
    if (ext == NULL) return OMNIC_FMT_UNKNOWN;
    char buf[16];
    size_t i = 0;
    for (; ext[i] != '\0' && i < sizeof(buf) - 1; i++) {
        buf[i] = (char)((ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i]);
    }
    buf[i] = '\0';
    for (size_t k = 0; k < sizeof(g_ext_fmt) / sizeof(g_ext_fmt[0]); k++) {
        if (strcmp(buf, g_ext_fmt[k].ext) == 0) return g_ext_fmt[k].fmt;
    }
    return OMNIC_FMT_UNKNOWN;
}

omnic_fmt_id omnic_detect(const char *path, const uint8_t *head, size_t n) {
    omnic_fmt_id f = omnic_detect_magic(head, n);
    if (f != OMNIC_FMT_UNKNOWN) return f;
    return omnic_detect_ext(path);
}
