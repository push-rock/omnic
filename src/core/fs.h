/* fs：归档的可移植文件系统层。
 * 路径对外统一 UTF-8（'/' 分隔），Windows 内部转 UTF-16；GBK 源路径经
 * CP_ACP 兜底转换。符号链接/重解析点与其他特殊文件跳过（归档 v1 不收录）。 */
#ifndef OMNIC_FS_H
#define OMNIC_FS_H

#include <stddef.h>
#include <stdio.h>

#include "omnic.h"

#define OMNIC_FS_PATH_MAX 4096

/* 遍历条目：rel 为相对归档根的路径（无前导 '/'） */
typedef struct {
    char rel[OMNIC_FS_PATH_MAX];
    unsigned long long size; /* 文件字节；目录恒 0 */
    long long mtime;         /* Unix 秒；未知为 0 */
    int is_dir;
} omnic_fs_entry;

/* 递归枚举 root（不跟随链接）。返回 malloc 条目数组（文件与目录，含空目录），
 * *n 出数量；不可读子目录、超长/特殊文件计入 *skipped 不中断。失败返回 NULL。 */
omnic_fs_entry *omnic_fs_walk(const char *root, size_t *n, size_t *skipped);

/* mkdir -p；路径已存在且为目录视为成功 */
omnic_status omnic_fs_mkdir_p(const char *path);

/* 设置文件或目录的 mtime（Unix 秒） */
omnic_status omnic_fs_set_mtime(const char *path, long long mtime);

/* 在已打开的流上设置 mtime（省一次独立打开）；须在 fclose 前调用 */
omnic_status omnic_fs_set_mtime_stream(FILE *f, long long mtime);

/* 整文件直写：创建/覆写 + 数据 + mtime 一体，不经 CRT stdio（其锁路径在多
 * writer 并发下逐文件成本线性膨胀）。OMNIC_OK=成功；E_USAGE=路径转换失败；
 * E_SYSTEM=打开或写入失败。opened 非空时回写 1=文件已打开（写失败≠无法创建）。 */
omnic_status omnic_fs_write_file(const char *utf8_path, const void *data, size_t n,
                                 long long mtime, int *opened);

/* 以 UTF-8 路径打开文件（Windows 内部转宽字符，规避 CRT ANSI fopen 的 GBK 坑） */
FILE *omnic_fs_fopen(const char *utf8_path, const char *mode);

/* 绝对化路径（不要求存在；分隔符统一 '/'）。返回 malloc 字符串，失败 NULL。 */
char *omnic_fs_abspath(const char *path);

/* path_abs 是否位于 root_abs 子树内（含平台大小写语义；路径须先 abspath）。 */
int omnic_fs_path_inside(const char *root_abs, const char *path_abs);

#endif /* OMNIC_FS_H */
