/* util：计时 + 单线程错误上下文（一期 CLI；TLS 留二期）。 */
#ifndef OMNIC_UTIL_H
#define OMNIC_UTIL_H

#include <stdarg.h>

#include "omnic.h"

double omnic_now_sec(void);

/* 线程不安全的 last-error（一期 CLI 单线程使用，SW-CODEC-003 的上下文消息） */
void omnic_set_error(const char *fmt, ...);
const char *omnic_last_error(void);

/* 拼接输出路径（malloc，调用方负责 free） */
char *omnic_join_ext(const char *in, const char *ext, const char *outdir);
/* 去掉最后一个后缀（malloc）；无合法后缀时追加 ".out" */
char *omnic_strip_ext(const char *in);
/* 去掉路径目录部分（指向 in 内部，不分配） */
const char *omnic_basename(const char *path);

#endif /* OMNIC_UTIL_H */
