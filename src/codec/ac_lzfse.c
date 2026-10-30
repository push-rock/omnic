/* lzfse adapter：.lzfse 原生流（"bvx-f"），整块 API（库无流式接口）。
 * 压缩缓冲用官方 CLI 同款估计公式（in + in/16 + 64）；解压侧直接驱动
 * lzfse 内部解码状态机而非 buffer API——后者把"缺输入（截断）"也报告成
 * DST_FULL，容量翻倍循环会空转到内存耗尽。区分标准：DST_FULL 且 dst
 * 未满 = 解码器卡在输入耗尽，判 E_DATA；dst 真满才翻倍重试。 */
#include <stdlib.h>
#include <string.h>
#include <lzfse.h>

#include "codec/codec.h"
/* lzfse 内部头（解码状态机直接驱动）。其内联代码有已知 sign-compare
 * 噪声，属上游代码，不修改，仅在本 TU 屏蔽该告警。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "lzfse_internal.h"
#pragma GCC diagnostic pop

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
    void *scratch = NULL;
    size_t src_len = 0;

    if (omnic_read_all(in, &src, &src_len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    if (src_len == 0) goto done; /* 空流对应空输出 */
    {
        size_t cap = (src_len < (1u << 20)) ? (1u << 20) : src_len * 4;
        scratch = malloc(lzfse_decode_scratch_size());
        if (scratch == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        for (;;) {
            dst = (uint8_t *)malloc(cap);
            if (dst == NULL) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            memset(scratch, 0, lzfse_decode_scratch_size());
            lzfse_decoder_state *s = (lzfse_decoder_state *)scratch;
            s->src = src;
            s->src_begin = src;
            s->src_end = src + src_len;
            s->dst = dst;
            s->dst_begin = dst;
            s->dst_end = dst + cap;
            int st = lzfse_decode(s);
            if (st == LZFSE_STATUS_OK) {
                size_t dlen = (size_t)(s->dst - dst);
                if (dlen > 0 && omnic_writer_write(out, dst, dlen) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
                goto done; /* dlen == 0：零字节流（如 bvx$ 终止块独占），合法空输出 */
            }
            if (st == LZFSE_STATUS_DST_FULL && s->dst == s->dst_end) {
                free(dst); /* 真缓冲满：容量翻倍重试 */
                dst = NULL;
                if (cap > (size_t)-1 / 2) {
                    ret = OMNIC_E_SYSTEM;
                    goto done;
                }
                cap *= 2;
                continue;
            }
            ret = OMNIC_E_DATA; /* ERROR / SRC_EMPTY / dst 未满却停滞 = 坏数据或截断 */
            goto done;
        }
    }
done:
    free(dst);
    free(scratch);
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

/* lzvn 条目（需求 4.4 节）："bvn" 前缀的流是首块为 LZVN 块（bvxn）的 lzfse
 * 流，解码与 lzfse 完全同路。上游无独立 lzvn 封装 API，仅解码。 */
const omnic_codec_ops omnic_codec_lzvn = {
    "lzvn",
    OMNIC_FMT_LZVN,
    "lzvn",
    false,
    true,
    0,
    0,
    0,
    NULL,
    omnic_lzfse_decompress,
};
