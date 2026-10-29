#include "detect/entropy.h"

#include <math.h>

double omnic_entropy(const uint8_t *p, size_t n) {
    if (n == 0) return 0.0;
    size_t hist[256] = {0};
    for (size_t i = 0; i < n; i++) hist[p[i]]++;
    double h = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] == 0) continue;
        double pr = (double)hist[i] / (double)n;
        h -= pr * log2(pr);
    }
    return h;
}
