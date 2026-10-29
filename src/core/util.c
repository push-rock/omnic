#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
double omnic_now_sec(void) {
    static LARGE_INTEGER freq = {0}; /* 函数内静态懒初始化，避免全局动态构造 */
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
double omnic_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

static char g_err[256];

void omnic_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

const char *omnic_last_error(void) { return g_err; }

const char *omnic_basename(const char *path) {
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *s = (s1 > s2) ? s1 : s2;
    return (s != NULL) ? s + 1 : path;
}

char *omnic_join_ext(const char *in, const char *ext, const char *outdir) {
    if (outdir == NULL) {
        /* 就地追加后缀：保留输入文件的目录部分 */
        size_t n = strlen(in) + strlen(ext) + 2;
        char *p = (char *)malloc(n);
        if (p == NULL) return NULL;
        snprintf(p, n, "%s.%s", in, ext);
        return p;
    }
    const char *base = omnic_basename(in);
    size_t dlen = strlen(outdir);
    int need_slash = (dlen > 0 && outdir[dlen - 1] != '/' && outdir[dlen - 1] != '\\') ? 1 : 0;
    size_t n = dlen + (size_t)need_slash + strlen(base) + 1 + strlen(ext) + 1;
    char *p = (char *)malloc(n);
    if (p == NULL) return NULL;
    if (need_slash) {
        snprintf(p, n, "%s/%s.%s", outdir, base, ext);
    } else {
        snprintf(p, n, "%s%s.%s", outdir, base, ext);
    }
    return p;
}

char *omnic_strip_ext(const char *in) {
    const char *dot = strrchr(in, '.');
    const char *s1 = strrchr(in, '/');
    const char *s2 = strrchr(in, '\\');
    const char *slash = (s1 > s2) ? s1 : s2;
    bool has_ext = (dot != NULL) && (slash == NULL || dot > slash) && (dot - in > 0) &&
                   (slash == NULL ? dot != in : dot > slash + 1);
    size_t n = strlen(in) + 8;
    char *p = (char *)malloc(n);
    if (p == NULL) return NULL;
    if (has_ext) {
        size_t keep = (size_t)(dot - in);
        memcpy(p, in, keep);
        p[keep] = '\0';
    } else {
        snprintf(p, n, "%s.out", in);
    }
    return p;
}
