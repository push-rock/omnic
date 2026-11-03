/* 决策表（SW-POL-002）。由 fit.json 生成（2026-11-03），覆盖类: code, exec, graphic, ntext, photo, structured, synth_pic。
 * 结构: 8 类 x 4 档 = 32 行；粗类 TEXT/MEDIA_RAW 经 gamma 细分后不进表。
 * 规则与支配口径见 docs/PHASE2_DECIDER_DESIGN.md（本地）；生成勿手改，
 * 手动调整须在表尾登记（SW-TRAIN-002 由 tools/train 生成）。 */
#include "policy/policy.h"

const omnic_policy_entry omnic_policy_entries[] = {

    /* ---- ntext ---- */
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -1, "natural text: zstd --fast=-1 (fastest compression)", "lz4", 0},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_FAST_DECOMPRESS, "lz4", 12, "natural text: lz4 -12 (fast decompress)", NULL, 0},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_BALANCED, "zstd", 6, "natural text: zstd -6 (balanced)", NULL, 0},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "natural text: xz -9 (best ratio)", NULL, 0},

    /* ---- code ---- */
    {OMNIC_CLS_CODE, OMNIC_PROFILE_FAST_COMPRESS, "zstd", 1, "source code: zstd -1 (fastest compression)", "snappy", 1},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_FAST_DECOMPRESS, "lz4", 12, "source code: lz4 -12 (fast decompress)", NULL, 0},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_BALANCED, "zstd", 9, "source code: zstd -9 (balanced)", NULL, 0},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "source code: xz -9 (best ratio)", NULL, 0},

    /* ---- exec ---- */
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_COMPRESS, "zstd", 6, "exec: zstd -6 (fastest compression)", "zstd", -3},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 8, "exec: zstd -8 (fast decompress)", NULL, 0},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BALANCED, "zstd", 9, "exec: zstd -9 (balanced)", NULL, 0},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "exec: xz -9 (best ratio)", NULL, 0},

    /* ---- structured ---- */
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_COMPRESS, "snappy", 1, "structured: snappy -1 (fastest compression)", NULL, 0},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_DECOMPRESS, "snappy", 2, "structured: snappy -2 (fast decompress)", NULL, 0},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BALANCED, "zstd", 9, "structured: zstd -9 (balanced)", NULL, 0},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BEST_RATIO, "xz", 6, "structured: xz -6 (best ratio)", NULL, 0},

    /* ---- graphic ---- */
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_FAST_COMPRESS, "zstd", 1, "graphic: zstd -1 (fastest compression)", "zlib-ng", 1},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_FAST_DECOMPRESS, "zlib-ng", 9, "graphic: zlib-ng -9 (fast decompress)", NULL, 0},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_BALANCED, "zlib-ng", 9, "graphic: zlib-ng -9 (balanced)", NULL, 0},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_BEST_RATIO, "brotli", 11, "graphic: brotli -11 (best ratio)", NULL, 0},

    /* ---- photo ---- */
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_FAST_COMPRESS, "snappy", 1, "photo: snappy -1 (fastest compression)", NULL, 0},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_FAST_DECOMPRESS, "snappy", 1, "photo: snappy -1 (fast decompress)", NULL, 0},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_BALANCED, "zstd", 8, "photo: zstd -8 (balanced)", NULL, 0},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_BEST_RATIO, "xz", 6, "photo: xz -6 (best ratio)", NULL, 0},

    /* ---- already_compressed ---- */
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_FAST_COMPRESS, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)", NULL, 0},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_FAST_DECOMPRESS, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)", NULL, 0},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BALANCED, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)", NULL, 0},
    {OMNIC_CLS_ALREADY_COMPRESSED, OMNIC_PROFILE_BEST_RATIO, OMNIC_SKIP_CODEC, 0, "already compressed: skip (use --force)", NULL, 0},

    /* ---- random ---- */
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_FAST_COMPRESS, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)", NULL, 0},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_FAST_DECOMPRESS, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)", NULL, 0},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_BALANCED, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)", NULL, 0},
    {OMNIC_CLS_RANDOM, OMNIC_PROFILE_BEST_RATIO, OMNIC_SKIP_CODEC, 0, "high entropy >=7.5 bits/byte: skip (use --force)", NULL, 0},

    /* ---- unknown ---- */
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -1, "unknown: zstd --fast=-1 (fastest compression)", NULL, 0},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "unknown: zstd -3 (fast decompress)", NULL, 0},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_BALANCED, "zstd", 6, "unknown: zstd -6 (balanced)", NULL, 0},
    {OMNIC_CLS_UNKNOWN, OMNIC_PROFILE_BEST_RATIO, "zstd", 19, "unknown: zstd -19 (best ratio)", NULL, 0},
};

/* 手动修改登记：
 * 2026-10-30（一期）TEXT/BEST_RATIO: xz -9 -> brotli -11，源码合集实测；
 * 2026-10-31（二期 M2）手工初值表（8 类），R4 支配实证 + feature_probe 校准。
 * 2026-10-31（二期 M4）tools/train 训练管线接管生成（fit 规则预注册见文档）。
 * zlib/zlib-ng/libdeflate/snappy/lzfse 在一期全组合被压制不进自动选路——
 * §3.7 F2 反驳了该结论，libdeflate 已重新纳入训练候选（bench 矩阵含其全部档位）。 */
const size_t omnic_policy_entries_n = sizeof(omnic_policy_entries) / sizeof(omnic_policy_entries[0]);
