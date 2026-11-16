/* 解压端格式识别（SYS-DEC-001）：magic 前缀优先，后缀兜底。 */
#ifndef OMNIC_MAGIC_H
#define OMNIC_MAGIC_H

#include "omnic.h"

/* head 为文件头 OMNIC_MAGIC_SIZE 字节；n 为实际读到的字节数。 */
omnic_fmt_id omnic_detect_magic(const uint8_t *head, size_t n);
omnic_fmt_id omnic_detect_ext(const char *path);
/* 组合判定：magic > 后缀 > UNKNOWN */
omnic_fmt_id omnic_detect(const char *path, const uint8_t *head, size_t n);
/* 压缩侧内容分类（SW-DET-004）：magic 命中优先于扩展名，防伪后缀误路由。 */
omnic_class omnic_classify_magic(const uint8_t *head, size_t n);

#endif /* OMNIC_MAGIC_H */
