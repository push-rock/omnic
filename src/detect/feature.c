#include "detect/feature.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "detect/entropy.h"

/* 窗口 FNV-1a 32（4KiB 整窗与 16B n-gram 共用） */
static uint32_t win_hash(const uint8_t *p, size_t win) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < win; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* 对齐窗口哈希收集 + 排序去重 → 重复率 1 − distinct/n。 */
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double dup_rate(uint32_t *hs, size_t n) {
    if (n == 0) return 0.0;
    qsort(hs, n, sizeof(uint32_t), cmp_u32);
    size_t distinct = 1;
    for (size_t i = 1; i < n; i++) {
        if (hs[i] != hs[i - 1]) distinct++;
    }
    return 1.0 - (double)distinct / (double)n;
}

static double line_cv_of(const uint8_t *m, size_t mn) {
    /* 行长 = 相邻 \n 间距；样本 < 8 行不足以稳定估方差 */
    size_t lens[4096];
    size_t nlines = 0, last = 0;
    for (size_t i = 0; i < mn && nlines < sizeof(lens) / sizeof(lens[0]); i++) {
        if (m[i] == '\n') {
            lens[nlines++] = i - last;
            last = i + 1;
        }
    }
    if (nlines < 8) return -1.0;
    double mu = 0.0;
    for (size_t i = 0; i < nlines; i++) mu += (double)lens[i];
    mu /= (double)nlines;
    if (mu < 1.0) return -1.0; /* 平均行长不足 1 字节，非文本形态 */
    double var = 0.0;
    for (size_t i = 0; i < nlines; i++) {
        double d = (double)lens[i] - mu;
        var += d * d;
    }
    var /= (double)nlines;
    return var > 0.0 ? sqrt(var) / mu : 0.0;
}

void omnic_feature_extract(const uint8_t *h, size_t hn, const uint8_t *m, size_t mn,
                           const uint8_t *t, size_t tn, omnic_features *out) {
    memset(out, 0, sizeof(*out));
    if (hn == 0) return;
    if (mn == 0) {
        mn = hn;
        m = h; /* 段退化：复用头段 */
    }
    if (tn == 0) {
        tn = hn;
        t = h;
    }

    out->entropy_h = omnic_entropy(h, hn);
    out->entropy_m = omnic_entropy(m, mn);
    out->entropy_t = omnic_entropy(t, tn);
    {
        double mx = out->entropy_h > out->entropy_m ? out->entropy_h : out->entropy_m;
        if (out->entropy_t > mx) mx = out->entropy_t;
        double mn3 = out->entropy_h < out->entropy_m ? out->entropy_h : out->entropy_m;
        if (out->entropy_t < mn3) mn3 = out->entropy_t;
        out->entropy_delta = mx - mn3;
    }

    size_t printable = 0, zeros = 0;
    for (size_t i = 0; i < hn + mn; i++) {
        const uint8_t *p = (i < hn) ? h + i : m + (i - hn);
        uint8_t b = *p;
        if ((b >= 32 && b <= 126) || b == 9 || b == 10 || b == 13) printable++;
    }
    for (size_t i = 0; i < hn; i++) {
        if (h[i] == 0) zeros++;
    }
    for (size_t i = 0; i < mn; i++) {
        if (m[i] == 0) zeros++;
    }
    for (size_t i = 0; i < tn; i++) {
        if (t[i] == 0) zeros++;
    }
    out->printable = (double)printable / (double)(hn + mn);
    out->zero_rate = (double)zeros / (double)(hn + mn + tn);
    out->line_cv = line_cv_of(m, mn);

    /* 4KiB 整窗（段内对齐）：合成图块重复 vs 照片噪声。
     * 退化段与头段同源（指针相等），只计一次，否则 dup 被虚增 2/3。 */
    {
        const uint8_t *segs[3] = {h, m, t};
        size_t segn[3] = {hn, mn, tn};
        uint32_t hs[3 * (OMNIC_SAMPLE_SEG / 4096u) + 3];
        size_t n = 0;
        for (int s = 0; s < 3; s++) {
            if (s > 0 && segs[s] == segs[0]) continue;
            for (size_t off = 0; off + 4096u <= segn[s]; off += 4096u) {
                if (n < sizeof(hs) / sizeof(hs[0])) hs[n++] = win_hash(segs[s] + off, 4096u);
            }
        }
        out->dup_rate = dup_rate(hs, n);
    }
    /* 16B n-gram（中段，采样前 4096 个）：token 级重复，CODE 检测 */
    {
        uint32_t hs[4096];
        size_t n = 0;
        for (size_t off = 0; off + 16u <= mn && n < sizeof(hs) / sizeof(hs[0]); off += 16u) {
            hs[n++] = win_hash(m + off, 16u);
        }
        out->dup_rate_16 = dup_rate(hs, n);
    }
}

omnic_class omnic_refine_class(omnic_class coarse, const omnic_features *g) {
    switch (coarse) {
        case OMNIC_CLS_TEXT:
            /* CODE：可打印 + token 级重复（关键字/样板）。
             * 实测分离带（feature_probe，2026-10-31）：自然语言 dup16 0.001~0.022
             * （dickens/enwik8），真实 C 源码 0.092（mnn 5MB），标记密集 XML 0.36；
             * 阈值 0.05 居分离带中点。 */
            if (g->printable >= 0.85 && g->dup_rate_16 >= 0.05) return OMNIC_CLS_CODE;
            return OMNIC_CLS_NTEXT;
        case OMNIC_CLS_MEDIA_RAW:
            /* 实测（3MB BMP 对照）：大色块合成图 dup4k=0.948，噪声照片=0.000；
             * 分离度大，0.30 阈值稳健。真照片有局部相关性但整窗相同不可能。 */
            return (g->dup_rate >= 0.30) ? OMNIC_CLS_GRAPHIC : OMNIC_CLS_PHOTO;
        default:
            /* STRUCTURED 细分撤销：tar 三段熵实测仅差 0.08（code_as.tar），
             * delta 判据无分离度；M5 corpora 实测后再评估（PHASE2 文档登记）。 */
            return coarse;
    }
}
