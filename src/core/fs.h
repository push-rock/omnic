/* fs：归档的可移植文件系统层。
 * 路径对外统一 UTF-8（'/' 分隔）；Windows 内部转成带 \\?\ 前缀的绝对 UTF-16 路径
 * （不受 MAX_PATH 260 上限约束，上限即 OMNIC_FS_PATH_MAX），GBK 源路径经 CP_ACP
 * 兜底转换。收录文件、目录、符号链接与目录联接；FIFO/socket/设备文件与其他
 * 重解析点不收录，逐项计数上报。 */
#ifndef OMNIC_FS_H
#define OMNIC_FS_H

#include <stddef.h>
#include <stdio.h>

#include "omnic.h"

#define OMNIC_FS_PATH_MAX 4096

/* Windows 上可还原的文件属性子集（位值与 Win32 FILE_ATTRIBUTE_* 一致） */
#define OMNIC_FS_ATTR_READONLY 0x1u
#define OMNIC_FS_ATTR_HIDDEN 0x2u
#define OMNIC_FS_ATTR_SYSTEM 0x4u
#define OMNIC_FS_ATTR_MASK 0x7u

/* 链接种类（link_kind） */
enum {
    OMNIC_FS_LINK_SYMLINK = 1,     /* 文件符号链接 */
    OMNIC_FS_LINK_DIR_SYMLINK = 2, /* 指向目录的符号链接 */
    OMNIC_FS_LINK_JUNCTION = 3     /* Windows 目录联接（目标必为绝对路径） */
};

/* 遍历条目。字符串均 malloc，用 omnic_fs_entries_free 整体释放。 */
typedef struct {
    char *rel;                      /* 相对归档根的路径（无前导 '/'） */
    unsigned long long size;        /* 文件字节；目录 0；链接 = 目标串字节数 */
    long long mtime;                /* Unix 秒；未知为 0 */
    long long ctime;                /* 创建时间 Unix 秒；未采集或未知为 0 */
    unsigned mode;                  /* POSIX 权限位（07777）；has_mode 为 0 时无意义 */
    unsigned attrs;                 /* OMNIC_FS_ATTR_*；POSIX 上 0 */
    int has_mode;                   /* 1 = mode 有效（0000 也是合法的权限位，不能拿 0 判有无） */
    unsigned long long dev, ino;    /* 文件身份（硬链接识别）；未知为 0 */
    int is_dir;
    int link_kind;                  /* OMNIC_FS_LINK_*；非链接为 0 */
    char *link_target;              /* 链接目标，原样；非链接为 NULL */
} omnic_fs_entry;

/* 解包要恢复的元数据；0 表示不设置 */
typedef struct {
    long long mtime, ctime;
    unsigned mode, attrs;
} omnic_fs_meta;

/* 未收录进归档的条目：按原因计数，并保留最先遇到的若干条路径（截断存放） */
#define OMNIC_FS_SKIP_SAMPLES 8
#define OMNIC_FS_SKIP_NAME 256
typedef struct {
    size_t special;    /* FIFO / socket / 设备文件 / 不支持的链接（云文件占位、去重、应用别名…） */
    size_t unreadable; /* 打不开的目录或文件 */
    size_t too_long;   /* 路径超出缓冲区 */
    size_t total;
    char first[OMNIC_FS_SKIP_SAMPLES][OMNIC_FS_SKIP_NAME];
    size_t n_first;
} omnic_fs_skip_report;

/* omnic_fs_walk_ex 的采集项 */
#define OMNIC_FS_WANT_CTIME 0x1u /* 创建时间：POSIX 上每个条目多一次 statx，按需采集 */

/* 递归枚举 root（不跟随链接）。返回 malloc 条目数组（文件、目录含空目录、链接），
 * *n 出数量；不可读子目录、超长/特殊文件计入跳过数，不中断。失败返回 NULL。 */
omnic_fs_entry *omnic_fs_walk_ex(const char *root, size_t *n, omnic_fs_skip_report *rep,
                                 unsigned want);
omnic_fs_entry *omnic_fs_walk_report(const char *root, size_t *n, omnic_fs_skip_report *rep);
omnic_fs_entry *omnic_fs_walk(const char *root, size_t *n, size_t *skipped);
void omnic_fs_entries_free(omnic_fs_entry *v, size_t n);

/* mkdir -p；路径已存在且为目录视为成功 */
omnic_status omnic_fs_mkdir_p(const char *path);

/* 在已打开的流上设置 mtime（省一次独立打开）；须在 fclose 前调用 */
omnic_status omnic_fs_set_mtime_stream(FILE *f, long long mtime);

/* 以 UTF-8 路径打开文件（Windows 内部转宽字符，规避 CRT ANSI fopen 的 GBK 坑） */
FILE *omnic_fs_fopen(const char *utf8_path, const char *mode);

/* 文件身份（硬链接识别用）。Windows 上要开一次句柄查询（约 16 µs/文件，放在
 * 分类池里做）；POSIX 上等价于一次 stat。dev/ino 同时为 0 表示未知。 */
omnic_status omnic_fs_file_id(const char *path, unsigned long long *dev, unsigned long long *ino);

/* 整文件直写：创建/覆写 + 数据 + 元数据，不经 CRT stdio（其锁路径在多 writer
 * 并发下逐文件成本线性膨胀）。force：目标存在且拒绝覆写（只读等）时先强制删除。
 * OMNIC_OK=成功；E_USAGE=路径转换失败；E_SYSTEM=打开或写入失败。opened 非空时
 * 回写 1=文件已打开（写失败≠无法创建）。 */
omnic_status omnic_fs_write_file_meta(const char *path, const void *data, size_t n,
                                      const omnic_fs_meta *m, int force, int *opened);

/* 按路径设置权限位与属性（目录的元数据要等其内文件全部写完再设） */
omnic_status omnic_fs_set_meta(const char *path, const omnic_fs_meta *m);

/* 建符号链接。is_dir：Windows 上决定符号链接的类型；POSIX 上忽略。
 * *unprivileged（可空）回写 1 = 失败是因为没有创建符号链接的权限（Windows 普通权限
 * 且未开开发者模式）；其余失败为 E_SYSTEM。 */
omnic_status omnic_fs_make_symlink(const char *path, const char *target, int is_dir,
                                   int *unprivileged);
omnic_status omnic_fs_make_junction(const char *path, const char *target);
omnic_status omnic_fs_make_hardlink(const char *existing, const char *path);

/* 强制删除文件或链接（先清只读）；不存在视为成功。不删非空目录。 */
omnic_status omnic_fs_remove_force(const char *path);

/* 原子替换：from 改名为 to，to 已存在则被整体替换（同目录内原子）。失败时 to 不变——
 * pack 借此发布归档：失败的路上，输出路径上原有的归档不会被截断。 */
omnic_status omnic_fs_replace(const char *from, const char *to);

/* 删除文件 / 空目录（不存在视为成功） */
omnic_status omnic_fs_remove(const char *path);
omnic_status omnic_fs_rmdir(const char *path);

/* pack 的默认归档路径：<root 去掉尾部分隔符>.omnic。返回 malloc 字符串，失败 NULL。 */
char *omnic_fs_default_archive_path(const char *root);

/* 绝对化路径（不要求存在；分隔符统一 '/'）。返回 malloc 字符串，失败 NULL。 */
char *omnic_fs_abspath(const char *path);

/* path_abs 是否位于 root_abs 子树内（含平台大小写语义；路径须先 abspath）。 */
int omnic_fs_path_inside(const char *root_abs, const char *path_abs);

#endif /* OMNIC_FS_H */
