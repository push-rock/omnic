/* CLI 参数解析（SW-CLI-001）：零依赖手写。 */
#ifndef OMNIC_ARGP_H
#define OMNIC_ARGP_H

#include "omnic.h"

typedef struct omnic_args {
    const char *cmd; /* c / d / analyze / list / version / help */
    omnic_options opts;
    const char **files;
    int nfiles;
    int cap;
} omnic_args;

/* 返回 0 成功；1 用法错误（错误消息已打印到 stderr）。 */
int omnic_parse_args(int argc, char **argv, omnic_args *a);

#endif /* OMNIC_ARGP_H */
