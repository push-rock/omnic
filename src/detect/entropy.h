#ifndef OMNIC_ENTROPY_H
#define OMNIC_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

/* 0 阶字节熵（bits/byte），SW-DET-003 的不可压缩判定输入。 */
double omnic_entropy(const uint8_t *p, size_t n);

#endif /* OMNIC_ENTROPY_H */
