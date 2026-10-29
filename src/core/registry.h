/* codec 注册表（SYS-ARCH-001）：静态 const 表，零动态初始化。 */
#ifndef OMNIC_REGISTRY_H
#define OMNIC_REGISTRY_H

#include "codec/codec.h"

size_t omnic_codec_count(void);
const omnic_codec_ops *omnic_codec_at(size_t i);
const omnic_codec_ops *omnic_codec_find(const char *name);
/* 解压端：格式 -> 第一个可解该格式的 codec（SYS-DEC-001 分发） */
const omnic_codec_ops *omnic_codec_for_fmt(omnic_fmt_id fmt);

#endif /* OMNIC_REGISTRY_H */
