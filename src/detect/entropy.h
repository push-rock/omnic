#ifndef OMNIC_ENTROPY_H
#define OMNIC_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

/* 0 阶字节熵（bits/byte），SW-DET-003 的不可压缩判定输入。 */
double omnic_entropy(const uint8_t *p, size_t n);

/* 直方图熵（k 桶，n 样本数）。 */
double omnic_entropy_u32(const uint32_t *hist, int k, size_t n);

#endif /* OMNIC_ENTROPY_H */
