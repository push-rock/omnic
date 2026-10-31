/* 特征探针（PHASE2_DECIDER_DESIGN）：打印文件的 φ 向量 CSV。
 * 用于判据阈值的实测校准与 M5 corpora 特征统计——测量驱动，不猜阈值。
 * 采样布局与 dispatcher.c decide() 一致（头/中/尾 256KiB）。 */
#include <stdio.h>
#include <stdlib.h>

#include "detect/feature.h"
#include "detect/filetype.h"

#define SEG OMNIC_SAMPLE_SEG

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file...>  # prints feature-vector CSV\n", argv[0]);
        return 2;
    }
    printf("path,H_h,H_m,H_t,delta,printable,line_cv,dup4k,dup16,zero,coarse,refined\n");
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (f == NULL) {
            fprintf(stderr, "skip %s\n", argv[i]);
            continue;
        }
        uint8_t *seg = (uint8_t *)malloc(3 * SEG);
        long sz = -1;
        if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t hn = fread(seg, 1, SEG, f), mn = 0, tn = 0;
        if (sz >= (long)(3 * SEG)) {
            if (fseek(f, sz / 2 - (long)(SEG / 2), SEEK_SET) == 0)
                mn = fread(seg + SEG, 1, SEG, f);
            if (fseek(f, sz - (long)SEG, SEEK_SET) == 0)
                tn = fread(seg + 2 * SEG, 1, SEG, f);
        }
        omnic_features g;
        omnic_feature_extract(seg, hn, seg + SEG, mn, seg + 2 * SEG, tn, &g);
        omnic_class coarse = omnic_classify_ext(argv[i]);
        omnic_class fine = omnic_refine_class(coarse, &g);
        printf("%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%s\n", argv[i], g.entropy_h,
               g.entropy_m, g.entropy_t, g.entropy_delta, g.printable, g.line_cv, g.dup_rate,
               g.dup_rate_16, g.zero_rate, omnic_class_str(coarse), omnic_class_str(fine));
        fclose(f);
        free(seg);
    }
    return 0;
}
