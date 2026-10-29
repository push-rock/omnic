/* lzfse adapter：.lzfse 原生流（"bvx-f"），整块 API（库无流式接口）。
 * 压缩缓冲用官方 CLI 同款估计公式（in + in/16 + 64）；解压侧 lzfse 的
 * "返回值 == 分配大小"语义不区分正好与不足，按容量翻倍重试消歧。 */
#include <stdlib.h>
#include <string.h>
#include <lzfse.h>

#include "codec/codec.h"

static omnic_status omnic_lzfse_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)level;  /* lzfse 单一速率点，无级别语义 */
    (void)threads;
    omnic_status ret = OMNIC_OK;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    size_t src_len = 0;

    if (omnic_read_all(in, &src, &src_len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    if (src_len == 0) goto done; /* 空输入产出空流，与解压端对称 */
    {
        size_t cap = src_len + src_len / 16 + 64;
        dst = (uint8_t *)malloc(cap);
        if (dst == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        size_t dlen = lzfse_encode_buffer(dst, cap, src, src_len, NULL);
        if (dlen == 0 || dlen > cap) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_writer_write(out, dst, dlen) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
    }
done:
    free(dst);
    omnic_free_all(src);
    return ret;
}

static omnic_status omnic_lzfse_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    size_t src_len = 0;

    if (omnic_read_all(in, &src, &src_len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    if (src_len == 0) goto done; /* 空流对应空输出 */
    {
        size_t cap = (src_len < (1u << 20)) ? (1u << 20) : src_len * 4;
        for (;;) {
            dst = (uint8_t *)malloc(cap);
            if (dst == NULL) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            size_t dlen = lzfse_decode_buffer(dst, cap, src, src_len, NULL);
            if (dlen == 0) {
                ret = OMNIC_E_DATA; /* 坏数据 */
                goto done;
            }
            if (dlen < cap) {
                if (omnic_writer_write(out, dst, dlen) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
                goto done;
            }
            free(dst);
            dst = NULL;
            if (cap > (size_t)-1 / 2) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            cap *= 2;
        }
    }
done:
    free(dst);
    omnic_free_all(src);
    return ret;
}

const omnic_codec_ops omnic_codec_lzfse = {
    "lzfse",
    OMNIC_FMT_LZFSE,
    "lzfse",
    true,
    true,
    0,
    0,
    0,
    omnic_lzfse_compress,
    omnic_lzfse_decompress,
};
