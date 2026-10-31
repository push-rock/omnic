/* 内容特征向量 φ(S)（PHASE2_DECIDER_DESIGN §3.3）。
 * S = 头/中/尾三段各 ≤256KiB；特征是细分类 γ 的唯一输入，
 * 阈值与复调规则见该文档——改动判据须先改文档再改代码。 */
#ifndef OMNIC_FEATURE_H
#define OMNIC_FEATURE_H

#include "omnic.h"

typedef struct omnic_features {
    double entropy_h, entropy_m, entropy_t; /* 各段一阶熵 */
    double entropy_delta;                   /* max-min，段间异质度 */
    double printable;                       /* 可打印率（头+中） */
    double line_cv;                         /* 中段行长变异系数；无足够换行时 = -1 */
    double dup_rate;                        /* 4KiB 窗口重复率（合成图块重复 vs 照片噪声） */
    double dup_rate_16;                     /* 16B 对齐 n-gram 重复率（token 级，CODE 检测） */
    double zero_rate;                       /* 零字节率（三段合计） */
} omnic_features;

/* 三段允许退化：中/尾段为空时复用头段（单段文件）。 */
void omnic_feature_extract(const uint8_t *h, size_t hn, const uint8_t *m, size_t mn,
                           const uint8_t *t, size_t tn, omnic_features *out);

/* 细分类 γ：粗类 × 特征 → 细类（PHASE2_DECIDER_DESIGN §3.4，阈值初值待 M6 复调）。
 * 仅细分 TEXT/MEDIA_RAW/STRUCTURED，其余原样返回。 */
omnic_class omnic_refine_class(omnic_class coarse, const omnic_features *g);

#endif /* OMNIC_FEATURE_H */
