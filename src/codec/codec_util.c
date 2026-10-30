/* 整块式 codec（libdeflate/lzfse 等无流式 API 的库）的适配辅助：
 * 流式 reader → 全量内存缓冲。内存峰值 = 原始 + 压缩，与 SYS-CMP-005 的
 * 流式目标不同，属于这些库的一期折中（官方 CLI 同为全内存方案）。 */
#include <stdlib.h>

#include "codec/codec.h"

omnic_status omnic_read_all(omnic_reader *in, uint8_t **data, size_t *len) {
    size_t cap = 1u << 20;
    size_t n = 0;

    /* 可 seek 的 reader 预估总大小，一次分配到位：翻倍 realloc 对大输入
     * 的累计拷贝是可观的吞吐损耗（A7 实测 libdeflate 路径 ~15%）。
     * 预估失败（管道等）回退翻倍策略。 */
    long cur = -1;
    if (in->f != NULL) {
        cur = ftell(in->f);
        if (cur >= 0 && fseek(in->f, 0, SEEK_END) == 0) {
            long end = ftell(in->f);
            if (end >= cur && fseek(in->f, cur, SEEK_SET) == 0 && end > cur) {
                size_t want = in->pend_len + (size_t)(end - cur);
                /* +1 余量：精确容量下读满会误触发翻倍 realloc（大输入一次
                 * 全量 memcpy，libdeflate 路径实测 ~13%）；留一字节即可让
                 * EOF 轮读到 0 直接收尾 */
                if (want + 1 > cap && want <= (size_t)-1 / 2) cap = want + 1;
            } else {
                cur = -1;
            }
        } else {
            cur = -1;
        }
    }

    uint8_t *buf = (uint8_t *)malloc(cap);
    if (buf == NULL) return OMNIC_E_SYSTEM;
    for (;;) {
        if (n == cap) {
            uint8_t *nb;
            if (cap > (size_t)-1 / 2) { free(buf); return OMNIC_E_SYSTEM; }
            cap *= 2;
            nb = (uint8_t *)realloc(buf, cap);
            if (nb == NULL) { free(buf); return OMNIC_E_SYSTEM; }
            buf = nb;
        }
        size_t r = omnic_reader_read(in, buf + n, cap - n);
        n += r;
        if (r == 0) break; /* EOF */
    }
    *data = buf;
    *len = n;
    return OMNIC_OK;
}

void omnic_free_all(uint8_t *data) { free(data); }
