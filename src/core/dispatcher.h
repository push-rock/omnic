/* dispatcher（架构 2.5）：压缩/解压流水线。原子写（临时文件+rename）、
 * 失败清理、跳过策略都在本层统一，adapter 不接触文件系统。 */
#ifndef OMNIC_DISPATCHER_H
#define OMNIC_DISPATCHER_H

#include "omnic.h"

/* 已由 api.c 保证 rep 非空、o 非空。 */
omnic_status omnic_run_compress(const char *path, const omnic_options *o, omnic_report *rep);
omnic_status omnic_run_decompress(const char *path, const omnic_options *o, omnic_report *rep);
/* 只探测+决策，不落盘（SW-DET-004） */
omnic_status omnic_run_analyze(const char *path, omnic_report *rep);

#endif /* OMNIC_DISPATCHER_H */
