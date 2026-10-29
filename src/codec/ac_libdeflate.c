/* libdeflate adapter：gzip 格式，整块 API（库无流式接口）。
 * 输出单 member gzip 流；解压用尾部 ISIZE（原长 mod 2^32）精确预分配，
 * 4GB+ 原长溢出场景靠容量翻倍重试兜底。内存策略见 codec_util.c 注释。 */
#include <stdlib.h>
#include <string.h>
#include <libdeflate.h>

#include "codec/codec.h"

static uint32_t ld_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static omnic_status omnic_libdeflate_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads;
    omnic_status ret = OMNIC_OK;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    struct libdeflate_compressor *c = NULL;
    size_t src_len = 0;

    if (omnic_read_all(in, &src, &src_len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    c = libdeflate_alloc_compressor(level);
    if (c == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    {
        size_t bound = libdeflate_gzip_compress_bound(c, src_len);
        dst = (uint8_t *)malloc(bound);
        if (dst == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        size_t dlen = libdeflate_gzip_compress(c, src, src_len, dst, bound);
        if (dlen == 0) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_writer_write(out, dst, dlen) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
    }
done:
    if (c != NULL) libdeflate_free_compressor(c);
    free(dst);
    omnic_free_all(src);
    return ret;
}

static omnic_status omnic_libdeflate_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    struct libdeflate_decompressor *d = NULL;
    size_t src_len = 0;

    if (omnic_read_all(in, &src, &src_len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    if (src_len < 18) { /* gzip 头 10B + 尾 8B 为最小合法结构 */
        ret = OMNIC_E_DATA;
        goto done;
    }
    d = libdeflate_alloc_decompressor();
    if (d == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    size_t cap = ld_le32(src + src_len - 4);
    if (cap < 65536) cap = 65536;
    for (;;) {
        dst = (uint8_t *)malloc(cap);
        if (dst == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        size_t dlen = 0;
        enum libdeflate_result r = libdeflate_gzip_decompress(d, src, src_len, dst, cap, &dlen);
        if (r == LIBDEFLATE_SUCCESS) {
            if (omnic_writer_write(out, dst, dlen) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
            goto done;
        }
        free(dst);
        dst = NULL;
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || cap > (size_t)-1 / 2) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        cap *= 2;
    }
done:
    if (d != NULL) libdeflate_free_decompressor(d);
    free(dst);
    omnic_free_all(src);
    return ret;
}

const omnic_codec_ops omnic_codec_libdeflate = {
    "libdeflate",
    OMNIC_FMT_GZIP,
    "gz",
    true,
    true,
    0,
    12,
    6,
    omnic_libdeflate_compress,
    omnic_libdeflate_decompress,
};
