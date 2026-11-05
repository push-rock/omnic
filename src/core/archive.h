/* archive：.omnic 归档（ARCHIVE_SPEC v1）。pack 打包目录树，unpack 还原/提取。
 * 实现于 archive.c；lib API 同步声明于 omnic.h（AR-LIB-001）。 */
#ifndef OMNIC_ARCHIVE_H
#define OMNIC_ARCHIVE_H

#include "omnic.h"

/* 格式版本与 flags（ARCHIVE_SPEC §3.2） */
#define OMNIC_ARC_VERSION 1u
#define OMNIC_ARC_F_MTIME 0x01u
#define OMNIC_ARC_F_INDEX_ZSTD 0x04u

/* solid 块默认 64 MiB（实验 H2：0.12pp 损失） */
#define OMNIC_ARC_BLOCK_MIB 64u

omnic_status omnic_pack(const char *root, const char *archive_path,
                        const omnic_options *o, omnic_report *rep);
omnic_status omnic_unpack(const char *archive_path, const char *dest_dir,
                          const omnic_options *o, omnic_report *rep);
/* CLI 层扩展（SPEC §5.2：v1 归 CLI，公共 API v2 再评估） */
omnic_status omnic_unpack_files(const char *archive_path, const char *dest_dir,
                                const char *const *names, size_t n_names,
                                const omnic_options *o, omnic_report *rep);
omnic_status omnic_arc_verify(const char *archive_path, const omnic_options *o,
                              omnic_report *rep);
omnic_status omnic_arc_list(const char *archive_path, const omnic_options *o);

#endif /* OMNIC_ARCHIVE_H */
