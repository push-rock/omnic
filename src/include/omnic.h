/* omnic - universal compressor
 * Public API of libomnic (INT-LIB-001).
 * See docs/ARCHITECTURE.md for the design baseline. */
#ifndef OMNIC_H
#define OMNIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OMNIC_VERSION "0.1.0"

/* 统一状态码，与 CLI 退出码同源（SW-CLI-002） */
typedef enum {
    OMNIC_OK = 0,
    OMNIC_E_USAGE = 1,  /* 用法/参数错误 */
    OMNIC_E_DATA = 2,   /* 数据错误：损坏、不可识别、不可压缩 */
    OMNIC_E_SYSTEM = 3  /* 系统错误：I/O、内存 */
} omnic_status;

/* 用户偏好（BUS-002） */
typedef enum {
    OMNIC_PROFILE_BALANCED = 0,
    OMNIC_PROFILE_FAST_COMPRESS,
    OMNIC_PROFILE_FAST_DECOMPRESS,
    OMNIC_PROFILE_BEST_RATIO,
    OMNIC_PROFILE_COUNT
} omnic_profile;

/* 数据类型分类（SW-DET-002） */
typedef enum {
    OMNIC_CLS_UNKNOWN = 0,        /* 未知二进制，靠采样熵细分 */
    OMNIC_CLS_TEXT,               /* 文本/代码/标记语言（γ 细分前的粗类） */
    OMNIC_CLS_EXEC,               /* 可执行/对象文件 */
    OMNIC_CLS_STRUCTURED,         /* 结构化二进制（γ 细分前的粗类） */
    OMNIC_CLS_MEDIA_RAW,          /* 未压缩媒体（γ 细分前的粗类） */
    OMNIC_CLS_ALREADY_COMPRESSED, /* 已压缩数据，默认跳过（SYS-CMP-003） */
    OMNIC_CLS_RANDOM,             /* 高熵随机数据（仅采样判定） */
    /* 二期细分类（PHASE2_DECIDER_DESIGN §3.4）：γ 由内容特征驱动，
     * 粗类值不重排、CLI/ABI 向后兼容；决策表按细类建行。
     * STRUCTURED 细分（CONTAINER/DB）实测无分离特征，撤销（见 feature.c 注） */
    OMNIC_CLS_NTEXT,              /* 自然语言文本（TEXT 细分） */
    OMNIC_CLS_CODE,               /* 源代码/标记语言（TEXT 细分） */
    OMNIC_CLS_GRAPHIC,            /* 合成图/图形位图（MEDIA_RAW 细分） */
    OMNIC_CLS_PHOTO               /* 照片/噪声主导位图（MEDIA_RAW 细分） */
} omnic_class;

/* 支持的压缩流格式（需求 4.4 节清单） */
typedef enum {
    OMNIC_FMT_UNKNOWN = 0,
    OMNIC_FMT_GZIP,
    OMNIC_FMT_ZLIB,
    OMNIC_FMT_RAW_DEFLATE,
    OMNIC_FMT_ZSTD,
    OMNIC_FMT_XZ,
    OMNIC_FMT_LZMA_ALONE,
    OMNIC_FMT_LZIP,
    OMNIC_FMT_BZIP2,
    OMNIC_FMT_LZ4,
    OMNIC_FMT_BROTLI,
    OMNIC_FMT_SNAPPY,
    OMNIC_FMT_COMPRESS_Z, /* UNIX compress (.Z) */
    OMNIC_FMT_LZFSE,
    OMNIC_FMT_LZVN,
    OMNIC_FMT_LZHAM,
    OMNIC_FMT_LZAV
} omnic_fmt_id;

#define OMNIC_SKIP_CODEC "skip" /* 决策表中的跳过哨兵值（SYS-CMP-003） */

typedef struct omnic_options {
    omnic_profile profile;
    const char *codec;     /* NULL = 决策器自动选择；否则 codec 名 */
    int level;             /* 0 = codec 默认；>0 显式级别 */
    int threads;           /* 0 = 单线程 */
    unsigned flags;        /* OMNIC_F_* */
    const char *output_dir;
} omnic_options;

#define OMNIC_F_FORCE 0x01u   /* 覆盖已存在输出；强制压缩不可压缩数据 */
#define OMNIC_F_KEEP 0x02u    /* 保留源文件 */
#define OMNIC_F_QUIET 0x04u
#define OMNIC_F_EXPLAIN 0x08u /* 输出决策依据（SW-POL-003） */

/* 采样与决策参数（SYS-CMP-002：决策 ≤50ms）
 * 二期分层采样（PHASE2_DECIDER_DESIGN §3.2）：头/中/尾各 256KiB，
 * 一阶熵在 256KiB 已收敛（|Ĥ_256K − Ĥ_4M| < 0.02 bits），IO 降至一期 4MiB 前缀的 1/5.3 */
#define OMNIC_SAMPLE_SEG (256u * 1024u)
#define OMNIC_MAGIC_SIZE 16u
#define OMNIC_ENTROPY_SKIP 7.5 /* bits/byte，SW-DET-003（三段熵中位数判定） */

typedef struct omnic_report {
    char codec[16];
    int level;
    omnic_class klass;
    omnic_fmt_id fmt;
    uint64_t in_bytes, out_bytes;
    double seconds; /* 纯 codec 耗时，不含采样与决策 */
    double ratio;   /* out/in，0.37 表示 37% */
    double mbps;    /* 输入吞吐 */
    double entropy; /* 采样熵 bits/byte（analyze 输出） */
    bool skipped;
    char reason[96]; /* 决策依据（--explain / analyze） */
} omnic_report;

/* libomnic API（INT-LIB-001）：CLI 与二期 GUI 共用 */
omnic_status omnic_compress(const char *path, const omnic_options *o, omnic_report *rep);
omnic_status omnic_decompress(const char *path, const omnic_options *o, omnic_report *rep);
omnic_status omnic_analyze(const char *path, omnic_report *rep);

const char *omnic_status_str(omnic_status s);
const char *omnic_profile_str(omnic_profile p);
const char *omnic_class_str(omnic_class c);
const char *omnic_fmt_str(omnic_fmt_id f);
const char *omnic_version_str(void);

#ifdef __cplusplus
}
#endif
#endif /* OMNIC_H */
