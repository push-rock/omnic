/* 整块式 codec（libdeflate/lzfse 等无流式 API 的库）的适配辅助：
 * 流式 reader → 全量内存缓冲。内存峰值 = 原始 + 压缩，与 SYS-CMP-005 的
 * 流式目标不同，属于这些库的一期折中（官方 CLI 同为全内存方案）。 */
#include <stdlib.h>

#include "codec/codec.h"

omnic_status omnic_read_all(omnic_reader *in, uint8_t **data, size_t *len) {
    size_t cap = 1u << 20;
    size_t n = 0;
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
