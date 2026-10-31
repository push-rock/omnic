/* 决策表（SW-POL-002，二期细分类初值——PHASE2_DECIDER_DESIGN §3.4）。
 * 8 类 × 4 档 = 32 行；粗类 TEXT/MEDIA_RAW 经 γ 细分后不进表；STRUCTURED
 * 细分撤销（tar 三段熵实测无分离度），保留粗类行待 M5 实测后重评估。
 * 初值来源：一期 lzbench/Silesia 基线 + R4 支配实证 + feature_probe 实测
 * 校准（2026-10-31：CODE/BR 取 xz-9——源码合集 brotli-11 仅胜 0.09%，
 * 标记密集 XML 上 xz-9 明确胜，混合期望下更稳）。
 * M6 后由 tools/train/fit.py 按 CSV 重新生成本文件（SW-TRAIN-002），
 * 手动修改需在表尾登记来源。 */
#include "policy/policy.h"

const omnic_policy_entry omnic_policy_entries[] = {
    /* 自然语言文本：BR 用 xz（enwik8 实测 xz-9 支配 brotli-11） */
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "natural text: lz4 (fastest compression)"},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "natural text: zstd -3 (fast decompress)"},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_BALANCED, "zstd", 8, "natural text: zstd -8 (balanced)"},
    {OMNIC_CLS_NTEXT, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "natural text: xz -9 (best ratio)"},

    /* 源代码/标记文本：高 token 重复让 zstd-3 与 lz4 压速接近而 ρ 好 3 倍
     * （R4 支配项 1）；BR 混合期望取 xz-9（见表头注） */
    {OMNIC_CLS_CODE, OMNIC_PROFILE_FAST_COMPRESS, "zstd", 3, "source code: zstd -3 (fast compression)"},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "source code: zstd -3 (fast decompress)"},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_BALANCED, "zstd", 8, "source code: zstd -8 (balanced)"},
    {OMNIC_CLS_CODE, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "source code: xz -9 (best ratio)"},

    /* 可执行/对象：模式与文本接近（R4：EXEC 四档全过，不细分） */
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "exec: lz4 (fastest compression)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "exec: zstd -3 (fast decompress)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BALANCED, "zstd", 8, "exec: zstd -8 (balanced)"},
    {OMNIC_CLS_EXEC, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "exec: xz -9 (best ratio)"},

    /* 结构化二进制：细分撤销，沿一期基线（tar 支配项 3/4 待 M5 语料实测重评估） */
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_COMPRESS, "lz4", 0, "structured: lz4 (fastest compression)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "structured: zstd -3 (fast decompress)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BALANCED, "zstd", 8, "structured: zstd -8 (balanced)"},
    {OMNIC_CLS_STRUCTURED, OMNIC_PROFILE_BEST_RATIO, "xz", 9, "structured: xz -9 (best ratio)"},

    /* 合成图/图形位图：brotli 在 BAL/BR 支配（R4 支配项 6/7） */
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -3, "graphic: zstd --fast=-3 (fastest compression)"},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "graphic: zstd -3 (fast decompress)"},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_BALANCED, "brotli", 5, "graphic: brotli -5 (balanced)"},
    {OMNIC_CLS_GRAPHIC, OMNIC_PROFILE_BEST_RATIO, "brotli", 11, "graphic: brotli -11 (best ratio)"},

    /* 照片/噪声主导：xz 在 BR 支配 zstd-16（R4 支配项 5，ρ -19%） */
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_FAST_COMPRESS, "zstd", -3, "photo: zstd --fast=-3 (fastest compression)"},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_FAST_DECOMPRESS, "zstd", 3, "photo: zstd -3 (fast decompress)"},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_BALANCED, "zstd", 8, "photo: zstd -8 (balanced)"},
    {OMNIC_CLS_PHOTO, OMNIC_PROFILE_BEST_RATIO, "xz", 6, "photo: xz -6 (best ratio)"},

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
 * 2026-10-30（一期）TEXT/BEST_RATIO: xz -9 -> brotli -11，源码合集实测；
 * 该校准在 enwik8（XML/自然语言主导）上翻转（R4 支配项 2），二期类细分
 * 后吸收为 NTEXT/BR=xz-9 与 CODE/BR=xz-9，矛盾解除。
 * 2026-10-31（二期 M2）表重构为 8 类 × 4 档。CODE/BR 初值经 feature_probe
 * 实测校准为 xz-9（依据见表头注）；STRUCTURED 细分撤销保留粗类行。
 * zlib/zlib-ng/libdeflate/snappy/lzfse 在全部 (类×档) 组合均被
 * zstd/lz4 压制，不进自动选路，保留 -c 手选，待训练管线复测。 */
const size_t omnic_policy_entries_n = sizeof(omnic_policy_entries) / sizeof(omnic_policy_entries[0]);
