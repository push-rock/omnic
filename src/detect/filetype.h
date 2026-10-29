/* 压缩端数据类型分类（SW-DET-002）：后缀映射表 + 熵兜底。 */
#ifndef OMNIC_FILETYPE_H
#define OMNIC_FILETYPE_H

#include "omnic.h"

omnic_class omnic_classify_ext(const char *path);
/* 组合分类：先按后缀，未知再按采样熵细分（SW-DET-003）。 */
omnic_class omnic_classify(const char *path, const uint8_t *sample, size_t n);

#endif /* OMNIC_FILETYPE_H */
