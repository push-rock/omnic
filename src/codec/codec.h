/* 统一 codec ops 接口（SW-CODEC-001）。
 * adapter 只见 reader/writer 流，不接触文件系统；原子写与失败清理统一在
 * dispatcher（架构 2.5 节）。新增 codec = 新增一个 ac_*.c + 注册表一行。 */
#ifndef OMNIC_CODEC_H
#define OMNIC_CODEC_H

#include "omnic.h"
#include "core/blockio.h"

typedef struct omnic_codec_ops {
    const char *name;       /* CLI 名，如 "zstd" */
    omnic_fmt_id fmt;       /* 输出格式 */
    const char *native_ext; /* 输出后缀（不带点），如 "zst" */
    bool can_compress;
    bool can_decompress;
    int min_level; /* 级别为各库原生语义，不做全局归一（架构 2.2） */
    int max_level;
    int default_level;
    /* 压缩：从 in 读全部数据写入 out；level/threads 已由 dispatcher 规整。 */
    omnic_status (*compress)(omnic_reader *in, omnic_writer *out, int level, int threads);
    /* 解压：从 in 读压缩流写入 out。in 的开头已含探测读过的字节（pushback 保证）。 */
    omnic_status (*decompress)(omnic_reader *in, omnic_writer *out);
} omnic_codec_ops;

/* 每个 adapter 导出一个 const ops 符号，注册表静态引用（零动态初始化）。 */
#ifdef OMNIC_CODEC_ZSTD
extern const omnic_codec_ops omnic_codec_zstd;
#endif
#ifdef OMNIC_CODEC_LZ4
extern const omnic_codec_ops omnic_codec_lz4;
#endif
#ifdef OMNIC_CODEC_XZ
extern const omnic_codec_ops omnic_codec_xz;
extern const omnic_codec_ops omnic_codec_lzma; /* lzma alone（.lzma） */
extern const omnic_codec_ops omnic_codec_lzip; /* lzip 容器（.lz），仅解码 */
#endif
#ifdef OMNIC_CODEC_ZLIB
extern const omnic_codec_ops omnic_codec_zlib;
extern const omnic_codec_ops omnic_codec_zlibstream; /* zlib 流（RFC 1950） */
extern const omnic_codec_ops omnic_rawdeflate;       /* raw deflate（.zz） */
#endif
#ifdef OMNIC_CODEC_ZLIB_NG
extern const omnic_codec_ops omnic_codec_zlibng;
#endif
#ifdef OMNIC_CODEC_LIBDEFLATE
extern const omnic_codec_ops omnic_codec_libdeflate;
#endif
#ifdef OMNIC_CODEC_BROTLI
extern const omnic_codec_ops omnic_codec_brotli;
#endif
#ifdef OMNIC_CODEC_SNAPPY
extern const omnic_codec_ops omnic_codec_snappy;
#endif
#ifdef OMNIC_CODEC_LZFSE
extern const omnic_codec_ops omnic_codec_lzfse;
extern const omnic_codec_ops omnic_codec_lzvn; /* lzvn（解码与 lzfse 同路） */
#endif
#ifdef OMNIC_CODEC_BZIP2
extern const omnic_codec_ops omnic_codec_bzip2; /* bzip2（vendored 解码半边） */
#endif
#ifdef OMNIC_CODEC_COMPRESSZ
extern const omnic_codec_ops omnic_codec_compressz; /* compress(.Z)（自研 LZW 解码） */
#endif

/* codec_util.c：整块式 API（libdeflate/lzfse 等）的适配辅助 */
omnic_status omnic_read_all(omnic_reader *in, uint8_t **data, size_t *len);
/* 释放 read_all 的产物 */
void omnic_free_all(uint8_t *data);

#endif /* OMNIC_CODEC_H */
