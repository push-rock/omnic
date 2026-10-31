/* 决策表（SW-POL-002 初值，lzbench 2.4 / Silesia 实测）。
 * 该文件为生成物：tools/train/fit.py 按 CSV 重新生成本文件（SW-TRAIN-002），
 * 一期先内置初值。手动修改需在表尾登记来源。 */
#include "policy/policy.h"

const omnic_policy_entry omnic_policy_entries[] = {
    /* 文本/代码：快压 lz4；高压缩 brotli -11（文本实测胜 xz -9，见表尾登记） */
    {OMNIC_CLS_TEXT, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "text: lz4 (fastest compression)"},
    {OMNIC_CLS_TEXT, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "text: zstd -3 (fast decompress)"},
    {OMNIC_CLS_TEXT, OMNIC_PROFILE_BALANCED, "zstd", 8, "text: zstd -8 (balanced)"},
    {OMNIC_CLS_TEXT, OMNIC_PROFILE_BEST_RATIO, "brotli", 11, "text: brotli -11 (best ratio)"},

    /* 可执行/对象：模式与文本接近 */
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "exec: lz4 (fastest compression)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "exec: zstd -3 (fast decompress)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BALANCED, "zstd", 8, "exec: zstd -8 (balanced)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "exec: xz -9 (best ratio)"},

    /* 结构化二进制 */
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "structured: lz4 (fastest compression)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "structured: zstd -3 (fast decompress)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BALANCED, "zstd", 8, "structured: zstd -8 (balanced)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "structured: xz -9 (best ratio)"},

    /* 未压缩媒体：位图/PCM，zstd 全档最优 */
    {OMNIC_CLS_MEDIA_RAW, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -3, "media: zstd --fast=-3 (fastest compression)"},
    {OMNIC_CLS_MEDIA_RAW, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "media: zstd -3 (fast decompress)"},
    {OMNIC_CLS_MEDIA_RAW, OMNIC_PROFILE_BALANCED, "zstd", 8, "media: zstd -8 (balanced)"},
    {OMNIC_CLS_MEDIA_RAW, OMNIC_PROFILE_BEST_RATIO, "zstd", 15, "media: zstd -15 (best ratio)"},

    /* 已压缩 / 高熵随机：跳过（SYS-CMP-003，假设 A1） */
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_FAST_COMPRESS, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)"},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_FAST_DECOMPRESS, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)"},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BALANCED, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)"},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BEST_RATIO, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)"},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_FAST_COMPRESS, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)"},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_FAST_DECOMPRESS, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)"},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_BALANCED, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)"},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_BEST_RATIO, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)"},

    /* 未知类型：zstd 保守档 */
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -1, "unknown: zstd --fast=-1 (fastest compression)"},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "unknown: zstd -3 (fast decompress)"},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_BALANCED, "zstd", 6, "unknown: zstd -6 (balanced)"},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_BEST_RATIO, "zstd", 19, "unknown: zstd -19 (best ratio)"},
};

/* 手动修改登记：
 * 2026-10-30 TEXT/BEST_RATIO: xz -9 -> brotli -11。源码合集语料(6.7MB)实测：
 * 压缩率 15.46% vs 15.55%，解压速度 ~3x；PE 二进制语料上 xz -9 仍占优
 * (16.23% vs 16.64%)，故 EXEC/STRUCTURED 维持 xz。zlib/zlib-ng/libdeflate/
 * snappy/lzfse 在全部 (类型×偏好) 组合均被 zstd/lz4 压制，不进自动选路，
 * 保留 -c 手选，待训练管线复测。 */
const size_t omnic_policy_entries_n = sizeof(omnic_policy_entries) / sizeof(omnic_policy_entries[0]);
