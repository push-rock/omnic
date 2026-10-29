/* 决策器（SW-POL-001）：编译期查表，输入 (class, profile) 输出 (codec, level)。
 * 表本体在 policy_table.gen.c，一期为 lzbench 数据初值，
 * 上线训练管线后由 tools/train/fit.py 重新生成（SW-TRAIN-002）。 */
#ifndef OMNIC_POLICY_H
#define OMNIC_POLICY_H

#include "omnic.h"

typedef struct omnic_policy_entry {
    uint8_t klass;   /* omnic_class */
    uint8_t profile; /* omnic_profile */
    const char *codec;
    int level; /* codec 原生级别；zstd 负值 = --fast 模式 */
    const char *reason;
} omnic_policy_entry;

const omnic_policy_entry *omnic_policy_lookup(omnic_class c, omnic_profile p);
bool omnic_policy_is_skip(const omnic_policy_entry *e);
size_t omnic_policy_entry_count(void);
const omnic_policy_entry *omnic_policy_entry_at(size_t i);

#endif /* OMNIC_POLICY_H */
