/* compress(.Z) adapter（需求 4.4）：LZW 解码器按 ncompress 5.0（Unlicense）
 * 的 decompress() 语义实现。要点：
 *  - LSB-first 位流（码位自低位填充，ncompress input 宏语义）；位宽切换
 *    与 CLEAR 处对齐到 8×n_bits 位边界，相对上一切换点（编码端 boff 的
 *    等价物——compress 格式的历史怪癖）。
 *  - 解码器滞后一码：code == free_ent 走 KwKwK 特例；CLEAR 后空出一槽
 *    （free_ent = FIRST-1）补齐滞后。
 *  - 上游 -b 9 自不兼容（编码端永不升 10 位，解码端却升），本实现按上游
 *    解码器行为拒绝该类文件。
 *  - 整文件载入（位对齐语义下流式窗口复杂，同 lzfse 的 O(n) 豁免口径）。
 *  - 格式无校验和：截断只能解出前缀（uncompress 同行为）。
 * 仅解码。 */
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"

#define ZC_INIT_BITS 9
#define ZC_FIRST 257
#define ZC_CLEAR 256

/* LSB-first 取码（ncompress input 宏的边界安全版）：流位 i 位于字节 i>>3
 * 的第 i&7 位（自低位起），码位 j = 流位 bit+j。LE 3 字节窗口右移取低
 * n_bits 位；越界 lookahead 按 0（尾部补位语义）。 */
static uint32_t z_getcode(const uint8_t *src, size_t len, uint64_t bit, unsigned n_bits) {
    size_t idx = (size_t)(bit >> 3);
    unsigned off = (unsigned)(bit & 7);
    uint32_t w = 0;
    if (idx < len) w |= (uint32_t)src[idx];
    if (idx + 1 < len) w |= (uint32_t)src[idx + 1] << 8;
    if (idx + 2 < len) w |= (uint32_t)src[idx + 2] << 16;
    return (w >> off) & ((1u << n_bits) - 1);
}

static omnic_status omnic_compressz_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    uint8_t *src = NULL;
    uint16_t *prefix = NULL;
    uint8_t *suffix = NULL;
    uint8_t *stk = NULL;
    size_t len = 0;

    if (omnic_read_all(in, &src, &len) != OMNIC_OK) return OMNIC_E_SYSTEM;
    if (len < 3 || src[0] != 0x1F || src[1] != 0x9D) {
        omnic_free_all(src);
        return OMNIC_E_DATA;
    }
    unsigned maxbits = src[2] & 0x1F;
    bool block = (src[2] & 0x80) != 0;
    if (maxbits < ZC_INIT_BITS || maxbits > 16 || (src[2] & 0x60) != 0) {
        omnic_free_all(src);
        return OMNIC_E_DATA;
    }
    uint32_t maxmaxcode = 1u << maxbits;
    prefix = (uint16_t *)malloc(maxmaxcode * sizeof(uint16_t));
    suffix = (uint8_t *)malloc(maxmaxcode);
    stk = (uint8_t *)malloc(maxmaxcode + 8); /* 前缀链严格递减，深度 ≤ 表项数 */
    if (prefix == NULL || suffix == NULL || stk == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    memset(prefix, 0, maxmaxcode * sizeof(uint16_t));
    for (int i = 0; i < 256; i++) suffix[i] = (uint8_t)i;

    uint64_t total_bits = (uint64_t)len * 8;
    uint64_t bit = 24;  /* 绝对位（含 3 字节头） */
    uint64_t base = 24; /* 上一位宽切换点：对齐基准 */
    uint32_t n_bits = ZC_INIT_BITS;
    uint32_t maxcode = (1u << n_bits) - 1; /* MAXCODE(9)-1，含 -b 9 边界语义 */
    uint32_t free_ent = block ? ZC_FIRST : 256;
    int oldcode = -1;
    unsigned finchar = 0;

    for (;;) {
        if (free_ent > maxcode) {
            /* 位宽切换：对齐到 8×n_bits（相对上一切换点）后升位宽 */
            uint64_t k = (uint64_t)n_bits * 8;
            bit = base + ((bit - base + k - 1) / k) * k;
            base = bit;
            n_bits++;
            maxcode = (n_bits == maxbits) ? maxmaxcode : (1u << n_bits) - 1;
            continue;
        }
        if (bit + n_bits > total_bits) break; /* 流尽，尾部补位忽略 */
        uint32_t code = z_getcode(src, len, bit, n_bits);
        bit += n_bits;

        if (oldcode < 0) {
            if (code >= 256) { /* 首码必须是字面量 */
                ret = OMNIC_E_DATA;
                break;
            }
            finchar = code;
            oldcode = (int)code;
            uint8_t b = (uint8_t)finchar;
            if (omnic_writer_write(out, &b, 1) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                break;
            }
            continue;
        }
        if (code == ZC_CLEAR && block) {
            memset(prefix, 0, maxmaxcode * sizeof(uint16_t));
            free_ent = ZC_FIRST - 1; /* 空一槽补齐解码器滞后 */
            uint64_t k = (uint64_t)n_bits * 8;
            bit = base + ((bit - base + k - 1) / k) * k;
            base = bit;
            n_bits = ZC_INIT_BITS;
            maxcode = (1u << n_bits) - 1;
            continue;
        }
        /* 解出一个串：KwKwK（code == free_ent）先压 finchar 再走旧串 */
        uint32_t incode = code;
        uint8_t *sp = stk + maxmaxcode + 8;
        if (code >= free_ent) {
            if (code > free_ent) { /* 越界码 = 坏流 */
                ret = OMNIC_E_DATA;
                break;
            }
            *--sp = (uint8_t)finchar;
            code = (uint32_t)oldcode;
        }
        while (code >= 256) {
            *--sp = suffix[code];
            code = prefix[code];
        }
        finchar = suffix[code];
        *--sp = (uint8_t)finchar;
        if (omnic_writer_write(out, sp, (size_t)(stk + maxmaxcode + 8 - sp)) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            break;
        }
        if (free_ent < maxmaxcode) {
            prefix[free_ent] = (uint16_t)oldcode;
            suffix[free_ent] = (uint8_t)finchar;
            free_ent++;
        }
        oldcode = (int)incode;
    }
done:
    free(prefix);
    free(suffix);
    free(stk);
    omnic_free_all(src);
    return ret;
}

const omnic_codec_ops omnic_codec_compressz = {
    "compress",
    OMNIC_FMT_COMPRESS_Z,
    "Z",
    false,
    true,
    0,
    0,
    0,
    NULL,
    omnic_compressz_decompress,
};
