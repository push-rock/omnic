# omnic 软件架构设计

| 项目 | 内容 |
|---|---|
| 版本 | v0.1 (对应需求基线 v0.1) |
| 日期 | 2026-10-29 |
| 需求追溯 | 对应 `REQUIREMENTS.md` 各条 SYS/SW 需求编号 |

---

## 1. 架构总览

三层架构：**决策层 → 调度层 → 适配层**，自上而下单向依赖；第三方压缩库全部隔离在最底层 adapter 之后。

```
┌────────────────────────────────────────────────────────────┐
│  CLI (src/cli)          libomnic (src/core)                │  ← 接口层
│  omnic c/d/analyze/list  omnic_compress()/decompress()     │    (INT-CLI/INT-LIB)
├────────────────────────────────────────────────────────────┤
│  调度层 (src/core)                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐      │
│  │ detector │→│ policy   │→│ dispatcher            │      │
│  │ 探测/分类 │  │ 决策查表  │  │ 压缩/解压流水线、原子写 │      │
│  └──────────┘  └──────────┘  └──────────────────────┘      │
├────────────────────────────────────────────────────────────┤
│  适配层 (src/codec)                                         │
│  codec registry ──► ops[12]：zstd lz4 zlib zlib-ng ...     │
│  统一流式块 I/O (4MB) + 统一错误码                            │
├────────────────────────────────────────────────────────────┤
│  vendored 第三方库 (third_party/)                            │
│  zstd lz4 zlib zlib-ng libdeflate brotli xz bzip2 ...      │
└────────────────────────────────────────────────────────────┘
   离线: tools/train (Python) ──生成──► policy_table.gen.c
```

### 1.1 关键架构决策 (ADR 摘要)

| # | 决策 | 理由 | 追溯 |
|---|---|---|---|
| ADR-1 | 输出**原生格式+原生后缀**，不自造容器 | 用户决策：兼容性优先，第三方工具可解 | SYS-FMT-001 |
| ADR-2 | 解压识别 **magic 优先、后缀兜底** | 后缀可被修改/丢失；magic 是格式自描述 | SYS-DEC-001 |
| ADR-3 | 核心与业务代码 **100% C11**，第三方库允许 C++ 混编链接（lzham），adapter 以 `extern "C"` 隔离 | 用户技术栈偏好；所有候选库均提供 C API | SYS-ARCH-001, R7 |
| ADR-4 | 决策器 = **编译期查表**（`policy_table.gen.c`），离线训练、在线查表 | 满足"决策要快"（≤1ms）；运行时零 Python/模型依赖 | SYS-CMP-002, SW-POL-004 |
| ADR-5 | codec **注册表 + ops 函数指针表**，可插拔 | 新增 codec 不改核心（≤2 文件改动）；CMake 每 codec 开关 | SYS-ARCH-001, SW-CODEC-005 |
| ADR-6 | **vendored** 第三方源码（裁剪版），不用系统库 | 可复现构建；三平台 CI 一致；版本锁定 | SW-INF-002 |
| ADR-7 | GPL/LGPL 库**不默认集成**，预留 `OMNIC_ENABLE_GPL` 构建隔离 | 许可证合规（MIT 项目不得静态链 GPL） | SYS-CODEC-002, R1 |
| ADR-8 | 全链路**流式分块**（默认 4MB 块），禁止整文件载入 | 内存常数化（≤256MB）；大文件可用 | SYS-CMP-005 |
| ADR-9 | `libomnic` C API 与 CLI 同源，CLI 是 API 的薄封装 | 二期 GUI 复用 API，不重复实现 | INT-LIB-001 |
| ADR-10 | lzip 格式用 **liblzma raw LZMA1 + 自研 13 字节头解析**，不引入 lzlib(GPL) | 一期解压覆盖 .lz 且零 GPL 依赖 | SYS-FMT 清单 |

---

## 2. 模块设计

### 2.1 目录结构

```
omnic/
├── src/
│   ├── include/omnic.h          # libomnic 公共 API (INT-LIB-001)
│   ├── core/
│   │   ├── api.c                # omnic_compress/decompress/analyze 实现
│   │   ├── dispatcher.c         # 流水线调度：探测→决策→codec 执行→原子落盘
│   │   ├── registry.c/.h        # codec 注册表
│   │   ├── blockio.c/.h         # 4MB 分块流式 I/O（复用 buffer）
│   │   ├── status.c/.h          # 统一错误码 + 上下文消息
│   │   └── options.c/.h         # 选项/偏好解析与传递
│   ├── detect/
│   │   ├── magic.c/.h           # magic 前缀表（解压端格式识别）
│   │   ├── filetype.c/.h        # 后缀→数据类型分类表（~200 后缀，压缩端）
│   │   └── entropy.c/.h         # 4MB 采样熵估计（0-order）
│   ├── policy/
│   │   ├── policy.c/.h          # 查表引擎 + explain 输出
│   │   └── policy_table.gen.c   # 训练管线生成的决策表（SW-TRAIN-002）
│   ├── codec/
│   │   ├── codec.h              # 统一 ops 接口（2.2 节）
│   │   ├── ac_zstd.c            # 12 个 adapter（ac_ 前缀）
│   │   ├── ac_lz4.c  ac_zlib.c  ac_zlibng.c  ac_libdeflate.c
│   │   ├── ac_brotli.c  ac_xz.c  ac_bzip2.c  ac_snappy.c
│   │   ├── ac_lzfse.c  ac_lzham.cpp  ac_lzav.c
│   │   ├── dec_lzip.c           # lzip 容器解析 + liblzma raw LZMA1 (ADR-10)
│   │   └── dec_lzw.c            # 自研 compress .Z 解码器
│   └── cli/
│       ├── main.c               # 子命令路由
│       ├── argp.c/.h            # 选项解析（自研，零依赖）
│       └── report.c/.h          # 进度/结果报告（tty 检测）
├── tools/
│   └── train/                   # Python 离线管线（SW-TRAIN）
│       ├── bench.py             # 语料 × (codec,level) 矩阵 → CSV
│       ├── fit.py               # CSV → Pareto → policy_table.gen.c
│       └── gen_magic.py         # magic 表生成/校验
├── corpora/                     # 训练语料（7 类 × ≥3 样本）
├── tests/                       # 单测 + 交叉验证 + fuzz + 性能回归
├── third_party/                 # vendored 裁剪库 + THIRD_PARTY_NOTICES.md
├── docs/                        # 本文档 + REQUIREMENTS.md
└── CMakeLists.txt
```

### 2.2 统一 codec 接口（适配层核心，SW-CODEC-001）

```c
/* src/codec/codec.h */
#include "omnic.h"

typedef struct omnic_codec_ops {
    const char   *name;              /* "zstd" —— CLI --codec 参数 */
    omnic_fmt_id  fmt;               /* OMNIC_FMT_ZSTD —— 解压端格式标识 */
    const char   *native_ext;        /* "zst" —— 输出文件后缀 */
    bool          can_compress;
    bool          can_decompress;
    int           min_level, max_level;   /* 级别域，供决策表使用 */
    bool          has_threads;            /* zstd MT (SYS-PERF-002) */

    /* 均为流式：blockio 提供 4MB 块，adapter 内部维护 codec 流状态 */
    omnic_status (*compress_init)(omnic_cstream **out, int level, int threads);
    omnic_status (*compress_block)(omnic_cstream *s, const void *in, size_t n);
    omnic_status (*compress_finish)(omnic_cstream *s);
    omnic_status (*decompress_init)(omnic_dstream **out, const uint8_t *header, size_t n);
    omnic_status (*decompress_block)(omnic_dstream *s, const void *in, size_t n);
    omnic_status (*decompress_finish)(omnic_dstream *s);
    void         (*stream_free)(void *s);

    /* 压缩输出 bound，用于输出文件预分配/防溢出 */
    size_t        (*bound)(size_t n, int level);
} omnic_codec_ops;

/* 注册表（每个 adapter 一个非动态初始化的静态注册项，符合无全局动态对象约束） */
void omnic_reg_zstd(void);   /* 由 registry 统一在 omnic_registry_init() 中调用 */
```

要点：

- **零全局动态对象**：所有注册项为 `static const` POD 结构或函数内静态；注册表是固定大小数组 + 编译期计数，不用 STL/堆初始化（规避 `__GLOBAL__sub_I_*` 类启动开销与静态检查问题）。
- **级别归一**：各库级别域不同（lz4 fast 1-17、zstd 1-22、brotli 0-11、libdeflate 1-12），决策表直接存 (codec 名, 原生级别)，不做全局归一化映射，避免语义损失。
- **错误映射**（SW-CODEC-003）：底层返回码 → `omnic_status` 枚举 + `omnic_error_ctx {file, byte_offset, lib_code}`。

### 2.3 探测器（detect）

**解压端**（SW-DET-001）：

```c
typedef struct { const uint8_t magic[16]; uint8_t len; omnic_fmt_id fmt; } omnic_magic_entry;
/* 有序表：长前缀优先（先匹配 6 字节的 xz，再 4 字节的 zstd…） */
omnic_fmt_id omnic_detect_by_magic(const uint8_t *head, size_t n);
omnic_fmt_id omnic_detect_by_ext (const char *path);      /* 后缀兜底 */
omnic_fmt_id omnic_detect(const char *path, const uint8_t *head, size_t n);
```

无 magic 格式（brotli/lzav/lzham/raw-deflate）识别顺序：精确 magic → 后缀表 →（可选）保守试解 64KB。

**压缩端**（SW-DET-002/003）：

```c
typedef enum {
    OMNIC_CLS_TEXT, OMNIC_CLS_EXEC, OMNIC_CLS_STRUCTURED,
    OMNIC_CLS_MEDIA_RAW, OMNIC_CLS_ALREADY_COMPRESSED,
    OMNIC_CLS_RANDOM, OMNIC_CLS_UNKNOWN
} omnic_class;                       /* 7 类 */

omnic_class omnic_classify(const char *ext, const uint8_t *sample, size_t n);
double      omnic_entropy(const uint8_t *sample, size_t n);  /* 0-order, bits/byte */
```

后缀表由 `filetype.tbl`（数据文件：`ext|class`）在构建时转成 C 数组，便于不重编译核心即可扩充。

### 2.4 决策器（policy，SW-POL）

```c
typedef struct {
    omnic_class klass;      /* 输入：数据类型 */
    uint8_t     entropy_band;  /* 输入：熵分档 0..4（<6.0 / 6.0-6.5 / 6.5-7.0 / 7.0-7.5 / ≥7.5） */
    omnic_profile profile;  /* 输入：偏好 */
} omnic_policy_key;

typedef struct {
    char     codec[16];     /* 输出：codec 名 */
    int      level;         /* 输出：原生级别 */
    uint16_t alt_count;     /* 备选数（--explain 用） */
    /* ...备选与理由由生成表自带 */
} omnic_policy_choice;

omnic_policy_choice omnic_decide(omnic_policy_key k);   /* 纯查表，确定性 (SW-POL-005) */
```

一期内置表初值（来自 lzbench 2.4 Silesia 实测，训练管线上线后自动替换）：

| 类别 \ profile | fast_compress | fast_decompress | balanced | best_ratio |
|---|---|---|---|---|
| 文本/代码 | lz4 -3 (620MB/s) | zstd -3 | zstd -8 | xz -9 (23.0%) |
| 可执行 | lz4 -3 | zstd -3 | zstd -8 | xz -9 |
| 结构化二进制 | lz4 -3 | zstd -3 | zstd -8 | xz -9 |
| 未压缩媒体 | zstd --fast=-3 | zstd -3 | zstd -8 | zstd -15 |
| 已压缩 | 跳过 (SYS-CMP-003) | 跳过 | 跳过 | 跳过 |
| 随机/高熵 | 跳过 | 跳过 | 跳过 | 跳过 |
| 未知 | zstd --fast=-1 | zstd -3 | zstd -6 | zstd -19 |

### 2.5 调度流水线（dispatcher）

**压缩**（SYS-CMP-001）：

```
open(in) ─► 读 4MB 采样 ─► classify+entropy ─► [已压缩? ─ 是 ─► 警告+跳过/exit 2]
                                        └─ 否 ─► omnic_decide(klass, band, profile)
              ─► registry lookup(codec) ─► 临时文件 <out>.omnic-tmp
              ─► 循环 compress_block ─► fsync ─► rename ─► 删源(除非 --keep) ─► 报告
```

**解压**（SYS-DEC-001）：

```
open(in) ─► 读 16B 头 ─► detect(magic→ext) ─► fmt→codec 反查注册表
         ─► 临时文件 ─► 循环 decompress_block ─► 校验(格式内建) ─► rename ─► 删源
```

原子性与失败清理统一在 dispatcher 实现，adapter 不接触文件系统（SW-CMP-005/006）。

### 2.6 libomnic 公共 API（INT-LIB-001）

```c
/* src/include/omnic.h */
typedef enum { OMNIC_OK=0, OMNIC_E_USAGE=1, OMNIC_E_DATA=2, OMNIC_E_SYSTEM=3 } omnic_status;

typedef struct {
    omnic_profile profile;
    const char   *codec;      /* NULL = 自动 */
    int           level;      /* 0 = 该 codec 默认 */
    int           threads;    /* 0 = 单线程 */
    bool          force, keep, quiet, explain;
    const char   *output_dir;
} omnic_options;

typedef struct {
    char     codec[16]; int level; double seconds;
    uint64_t in_bytes, out_bytes; double ratio, mbps;
    omnic_class klass; char reason[128];
} omnic_report;

omnic_status omnic_compress  (const char *path, const omnic_options *, omnic_report *);
omnic_status omnic_decompress(const char *path, const omnic_options *, omnic_report *);
omnic_status omnic_analyze   (const char *path, omnic_report *);
```

### 2.7 训练管线（tools/train，离线 Python）

```
corpora/ ─► bench.py ─► results/*.csv ─► fit.py ─► src/policy/policy_table.gen.c
             │                                        │
             │  codec×level 矩阵实测                    └─► 重新编译 omnic 即生效
             └─► 复用 omnic 自身 CLI 做测量（吃自己的狗粮）
```

- `bench.py` 调用 omnic CLI + 官方 CLI 计时（交叉验证性能基准 TST-PERF-001 同源）。
- `fit.py`：对每 (class, profile) 在 (ratio, c_speed, d_speed) 上按 profile 加权选 Pareto 最优：
  - fast_compress：max c_speed，ratio 硬约束 ≤ ratio(zstd -1)
  - fast_decompress：max d_speed
  - best_ratio：min ratio
  - balanced：max (归一化 c_speed × d_speed / ratio)
- CSV 入库，表可再生成（SW-TRAIN-004）。

### 2.8 构建与 CI（SW-INF）

- CMake ≥3.16；`OMNIC_CODEC_ZSTD=ON...` 每库开关，核心 10 默认 ON。
- 编译器：MSVC 19.29+ / GCC 11+ / Clang 14+；`-Wall -Wextra`，CI 加 `-Werror`。
- CI（GitHub Actions）矩阵：`{win-latest(msvc), ubuntu-22.04(gcc,clang), macos-14(arm64), macos-13(x64)}` × `{build, test, crosscheck}`。
- release：三平台静态链接单二进制 + man 页。

---

## 3. 数据流与内存策略

- 块大小 4MB（与 lzbench 多线程表一致的经验值），单块编解码，buffer 全程复用（R8）。
- 决策仅消耗 4MB 采样，之后采样 buffer 释放。
- 峰值内存 ≈ 4MB 输入块 + 4MB 输出 bound + codec 工作区（zstd -22 约 ≤128MB，其余 ≤33MB）→ 满足 ≤256MB。

## 4. 二期规划接口预留

| 二期特性 | 一期预留点 |
|---|---|
| 归档（zip/7z/tar/rar） | `omnic_fmt_id` 枚举位段预留 16-bit；dispatcher 对容器格式先解归档再逐流传入现有解压路径 |
| GUI 薄壳（webview） | 全部能力已收敛到 libomnic C API；CLI 不持有独占逻辑 |
| AI 决策器 | `omnic_decide()` 是唯一决策入口，替换 `policy_table.gen.c` 即升级模型，接口不变 |
| 多线程分块压缩 | blockio 已按块设计，zstd 已走原生 MT（SYS-PERF-002） |
| GPL 扩展包 | CMake `OMNIC_ENABLE_GPL` 开关位已定义（ADR-7） |

## 5. 需求覆盖对照（架构 → 需求）

| 架构组件 | 覆盖需求 |
|---|---|
| detector (magic/filetype/entropy) | SYS-DEC-001, SW-DET-001..004 |
| policy + policy_table.gen.c | SYS-CMP-002, SW-POL-001..005, SW-TRAIN |
| dispatcher + blockio | SYS-CMP-001/003/005, SW-CMP-005/006/007, SYS-DEC-003/004 |
| codec registry + ac_* | SYS-ARCH-001, SW-CODEC-001..005 |
| cli + libomnic | INT-CLI-001, INT-LIB-001, SW-CLI-001..003 |
| third_party + notices | SYS-CODEC-001..004, SW-INF-002 |
| tools/train | SW-TRAIN-001..005 |
