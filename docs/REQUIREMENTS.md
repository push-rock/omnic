# omnic 需求规格说明书

| 项目 | 内容 |
|---|---|
| 项目名 | omnic — Universal Compressor |
| 版本 | v0.1 (一期需求基线) |
| 日期 | 2026-10-29 |
| 状态 | 待评审 |
| 目标仓库 | github.com/<owner>/omnic，本地 `D:\omnic` |

---

## 1. 项目概述

### 1.1 一句话需求（用户原始输入）

> 基于 lzbench 项目的压缩解压算法清单，做一个通用压缩器：根据文件后缀类型自动选择最合适的压缩算法，并允许用户选择"压缩更快 / 解压更快 / 压缩率更好"三种偏好；解压端根据后缀识别并支持市面上大多数压缩算法。

### 1.2 产品定位

**压缩端智能路由 + 解压端万能钥匙**。

- 压缩端：市面上没有成熟工具做"按数据类型 + 用户偏好在多个算法间路由"（zstd `--adapt` 只在单一算法内自适应级别，7-Zip 不按数据类型路由）——这是本项目核心差异化。
- 解压端：一个二进制解开 ~16 种市面常见压缩流格式，识别采用 **magic number 优先、后缀兜底**。

### 1.3 目标与非目标

| | 内容 |
|---|---|
| **目标（一期）** | C11 核心库 + CLI；12 个压缩算法族；16 种解压格式；决策器（内置规则表）；Python 训练管线 MVP；三平台 CI |
| **目标（二期）** | 归档格式（zip/7z/tar/rar）；webview 薄壳 GUI；大规模语料训练 + AI 决策器迭代；多线程分块压缩扩展 |
| **非目标** | 训练框架（本项目只做推理）；自定义容器格式（产物必须原生格式，见 SYS-FMT-001）；加密压缩 |

### 1.4 术语表

| 术语 | 定义 |
|---|---|
| codec | 一个压缩算法实现（如 zstd），含压缩与解压能力 |
| 算法族 | 同一格式不同编码器（zlib / zlib-ng / libdeflate 都产出 gzip/deflate 格式，算 3 个 codec、1 个格式） |
| magic number | 文件头魔数，用于识别压缩格式 |
| profile | 用户偏好：fast_compress / fast_decompress / best_ratio / balanced |
| 决策器 | 输入文件特征 + profile，输出 (codec, level) 的策略组件 |
| 原生格式 | 算法官方定义的文件格式与后缀（.zst/.xz/.gz 等），保证第三方工具可解 |
| Pareto 前沿 | 压缩率-速度平面上不被其他方案支配的最优方案集合 |

---

## 2. 需求分层与追溯关系

```
BUS-001..005 (业务需求)
  └─ SYS-* (系统需求: CMP 压缩 / DEC 解压 / FMT 格式 / PORT 可移植 / PERF 性能 / ARCH 架构)
       ├─ SW-* (软件需求: CMP / DEC / POL 决策器 / CODEC 适配层 / DET 探测 / CLI / TRAIN 训练 / INF 构建)
       └─ INT-* (接口需求: CLI 人机接口 / LIB 库 API)
            └─ TST-* (测试需求, 每条可验证需求至少一条)
```

编号规则：`<层级>-<模块码>-<序号>`。模块码：CMP 压缩、DEC 解压、POL 决策、CODEC 适配、DET 探测、CLI 命令行、TRAIN 训练、INF 构建、FMT 格式、PORT 可移植、PERF 性能、ARCH 架构。

---

## 3. 业务需求 (BUS)

| 编号 | 需求描述 | 优先级 |
|---|---|---|
| BUS-001 | 用户压缩文件时无需了解压缩算法知识，系统自动选择当前文件最合适的算法与级别 | Must |
| BUS-002 | 用户可通过三种偏好影响选择：压缩更快、解压更快、压缩率更好（外加均衡默认档） | Must |
| BUS-003 | 压缩产物采用算法原生格式与后缀，可被市面主流工具（zstd CLI、xz CLI、gzip、7-Zip 等）直接解开 | Must |
| BUS-004 | 解压端覆盖市面上大多数压缩流格式（一期纯流式 ~16 种，二期含归档格式） | Must |
| BUS-005 | 项目以开源形式发布于 GitHub，Windows/Linux/macOS 三平台提供预编译单二进制 | Must |

---

## 4. 系统需求 (SYS)

### 4.1 压缩路径 (SYS-CMP)

| 编号 | 需求描述 | 量化指标 | 父需求 |
|---|---|---|---|
| SYS-CMP-001 | 当用户压缩一个文件时，系统 shall 按顺序执行：文件类型识别 → 决策器选择 (codec, level) → 调用对应 codec → 输出原生格式文件，输出后缀为该算法官方后缀 | — | BUS-001 |
| SYS-CMP-002 | 决策器 shall 在文件特征提取完成后 50ms 内给出决策（查表 ≤1ms，特征提取含 4MB 采样分析 ≤50ms，不含磁盘 I/O） | ≤50ms | BUS-001 |
| SYS-CMP-003 | 当输入文件被判定为已压缩数据（媒体文件、归档、压缩流，见 SW-DET-002）时，系统 shall 默认跳过压缩并输出警告；用户 `--force` 后 shall 执行压缩 | — | BUS-001 |
| SYS-CMP-004 | 系统 shall 报告压缩结果：选中算法、级别、决策依据（数据类型）、耗时、原始/压缩大小、压缩率、吞吐量 | — | BUS-002 |
| SYS-CMP-005 | 压缩过程 shall 采用流式处理，内存占用与文件大小无关，上限 256MB（含 codec 工作区） | ≤256MB 常数 | BUS-001 |

### 4.2 解压路径 (SYS-DEC)

| 编号 | 需求描述 | 量化指标 | 父需求 |
|---|---|---|---|
| SYS-DEC-001 | 当用户解压一个文件时，系统 shall 以 magic number 识别格式；magic 无法判定时以后缀判定；两者冲突时以 magic 为准并提示 | 识别准确率 100%（对第 4.4 节清单内的合法文件） | BUS-004 |
| SYS-DEC-002 | 当格式无法识别时，系统 shall 以非零退出码退出，并打印当前支持的格式清单 | — | BUS-004 |
| SYS-DEC-003 | 当压缩流损坏或截断时，系统 shall 在首个错误字节处报告错误并以非零退出码退出，不得崩溃 | 零崩溃（fuzz 验证） | BUS-004 |
| SYS-DEC-004 | 解压 shall 采用流式处理，内存上限 256MB | ≤256MB | BUS-004 |
| SYS-DEC-005 | 数据完整性校验 shall 透传原生格式内建校验（gzip CRC32、xz CRC64、zstd XXH64、bzip2 组合 CRC），不额外增加封装层 | — | BUS-003 |

### 4.3 格式兼容 (SYS-FMT)

| 编号 | 需求描述 | 父需求 |
|---|---|---|
| SYS-FMT-001 | 系统 shall 输出且仅输出原生格式：omnic 压缩的产物必须能被对应官方参考实现 CLI 解开（交叉验证，见 TST-FMT-001） | BUS-003 |
| SYS-FMT-002 | 系统 shall 解压"参考实现 CLI 产出的文件"（反向交叉验证，见 TST-FMT-002） | BUS-004 |

### 4.4 一期解压格式清单 (16 种)

| 格式 | 后缀 | magic (hex) | 解码实现 |
|---|---|---|---|
| gzip | .gz | `1F 8B` | zlib |
| zlib stream | .zlib | `78 01/9C/DA/5E...` | zlib |
| raw deflate | .zz | —（后缀判定） | zlib (raw 模式) |
| zstd | .zst | `28 B5 2F FD` | zstd |
| xz | .xz | `FD 37 7A 58 5A 00` | liblzma |
| lzma alone | .lzma | `5D 00 00 ...` | liblzma |
| lzip | .lz | `4C 5A 49 50` ("LZIP") | liblzma (raw LZMA1 + 自研容器解析) |
| bzip2 | .bz2 | `42 5A 68` | bzip2 |
| lz4 frame | .lz4 | `04 22 4D 18` | lz4 |
| brotli | .br | —（无 magic，后缀判定） | brotli |
| snappy framed | .sz/.snappy | `FF 06 00 00 53 4E 50 41` | snappy |
| compress LZW | .Z | `1F 9D` | 自研 LZW 解码器 |
| lzfse | .lzfse | `62 76 78 2D` ("bvx-") | lzfse |
| lzvn | .lzvn | `62 76 6E` ("bvn") | lzfse (lzvn) |
| lzham | .lzham | —（后缀判定） | lzham |
| lzav | .lzav | —（后缀判定） | lzav |

> 备注：zlib-ng / libdeflate / slz 产出即 gzip/deflate 格式，由第 1–3 行统一覆盖。`.lzo`(GPL)、`.bz3`(LGPL)、`.liz`(GPL) 见 4.6 节准入标准。

### 4.5 一期压缩算法族清单 (12 族)

| # | codec | 版本基线 | License | 输出格式 | 语言 | 默认构建 |
|---|---|---|---|---|---|---|
| 1 | zstd | 1.5.7 | BSD-3 | zstd | C | ✅ |
| 2 | lz4/lz4hc | 1.10.0 | BSD-2 | lz4 frame | C | ✅ |
| 3 | zlib | 1.3.2 | zlib | gzip | C | ✅ |
| 4 | zlib-ng | 2.3.3 | zlib | gzip/zlib | C | ✅ |
| 5 | libdeflate | 1.26 | MIT | gzip/zlib/raw | C | ✅ |
| 6 | brotli | 1.2.0 | MIT | brotli | C | ✅ |
| 7 | xz (liblzma) | 5.8.4 | 0BSD | xz | C | ✅ |
| 8 | bzip2 | 1.0.8 | BSD-like | bzip2 | C | ✅ |
| 9 | snappy | 1.3.1 | BSD-3 | snappy framed | C | ✅ |
| 10 | lzfse/lzvn | 1.0 | Apple BSD-like | lzfse | C | ✅ |
| 11 | lzham | 1.0 | Public Domain | lzham | C++ | 可选 |
| 12 | lzav | 5.20 | MIT | lzav raw | C(单头) | 可选 |

Pareto 覆盖验证（lzbench 2.4 / Silesia 实测）：

- 快压档：lz4 --fast-3 (620 MB/s)，zstd --fast=-1 (453 MB/s)
- 快解档：lz4 (3832 MB/s)，zstd -3 (~1257 MB/s)
- 高压缩档：xz -9 (ratio 23.01%)，brotli -11 (23.78%)，zstd -22 (24.67%)
- 均衡档：zstd -6/-8

### 4.6 算法准入标准 (SYS-CODEC)

| 编号 | 需求描述 |
|---|---|
| SYS-CODEC-001 | codec 准入 shall 同时满足：① 许可证为 MIT/BSD/Apache/zlib/0BSD/Public Domain 等宽松许可；② 上游仍在维护或格式已稳定冻结；③ 纯 C/C++ 工具链可构建（不引入 Rust/Go/Zig/CUDA 依赖）；④ 三平台（Win/Linux/macOS）可编译 |
| SYS-CODEC-002 | 当候选库仅提供 GPL/LGPL 许可（如 lzo、lzlib、bzip3、lizard）时，系统 shall 不默认集成；如后续集成须以 `OMNIC_ENABLE_GPL=ON` 独立构建选项隔离，且默认发布二进制不含该代码 |
| SYS-CODEC-003 | 所有 vendored 第三方库 shall 在 `THIRD_PARTY_NOTICES.md` 登记来源、版本、许可证原文 |
| SYS-CODEC-004 | 当上游库出现安全公告且无补丁时，系统 shall 在一个发布周期内移除或替换该 codec |

### 4.7 可移植性 / 性能 / 架构 (SYS-PORT/PERF/ARCH)

| 编号 | 需求描述 | 量化指标 |
|---|---|---|
| SYS-PORT-001 | 系统 shall 运行于 Windows 10+ x64、Ubuntu 22.04+ x64、macOS 12+ (x64/arm64)，CI 三平台全绿 | 3 平台 × 2 编译器 |
| SYS-PERF-001 | 适配层引入的吞吐损耗 shall 不超过裸库基准的 5%（同机同文件对比官方 CLI） | ≥95% 裸库吞吐 |
| SYS-ARCH-001 | codec shall 以注册表模式接入：新增一个 codec 不修改核心调度代码，仅新增一个 adapter 源文件 + 注册项 | 新增 codec 改动 ≤2 个文件 |
| SYS-PERF-002 | zstd codec shall 支持原生多线程压缩（frame 内多 job，格式不变）；其余 codec 一期单线程 | zstd MT 加速比 ≥0.7×线程数 |

---

## 5. 软件需求 (SW)

### 5.1 压缩模块 (SW-CMP)

| 编号 | 需求描述 | 量化指标 | 父需求 | 优先级 |
|---|---|---|---|---|
| SW-CMP-001 | 软件 shall 支持 `omnic c <file...>` 自动压缩，输出 `<file>.<ext>` | — | SYS-CMP-001 | Must |
| SW-CMP-002 | 软件 shall 支持 `--profile fast_compress\|fast_decompress\|best_ratio\|balanced`，默认 balanced | 4 种 | SYS-CMP-001 | Must |
| SW-CMP-003 | 软件 shall 支持 `--codec <name> --level <n>` 手动覆盖自动决策 | — | SYS-CMP-001 | Must |
| SW-CMP-004 | 软件 shall 支持 `--keep`（保留源文件）；默认压缩成功后删除源文件，失败时保留 | — | SYS-CMP-001 | Should |
| SW-CMP-005 | 软件 shall 以临时文件 + 原子 rename 落盘，压缩失败时不得留下半成品文件 | — | SYS-CMP-001 | Must |
| SW-CMP-006 | 当输出文件已存在时，软件 shall 报错退出，除非 `--force` | — | SYS-CMP-001 | Must |
| SW-CMP-007 | 软件 shall 支持 `-` (stdin→stdout) 管道模式，决策基于 stdin 前 4MB 采样 | — | SYS-CMP-001 | Should |
| SW-CMP-008 | 软件 shall 显示进度（stderr，仅 tty 附加时），`--quiet` 关闭 | — | SYS-CMP-004 | Should |

### 5.2 解压模块 (SW-DEC)

| 编号 | 需求描述 | 父需求 | 优先级 |
|---|---|---|---|
| SW-DEC-001 | 软件 shall 支持 `omnic d <file...>` 自动识别格式并解压，输出文件名 = 去掉压缩后缀 | SYS-DEC-001 | Must |
| SW-DEC-002 | 软件 shall 支持 `omnic list` 打印全部支持格式（压缩/解压分列） | SYS-DEC-002 | Must |
| SW-DEC-003 | 软件 shall 支持同批混合格式输入，逐文件独立处理，单文件失败不影响后续文件，最终退出码取最大错误级 | SYS-DEC-003 | Must |

### 5.3 探测模块 (SW-DET)

| 编号 | 需求描述 | 量化指标 | 父需求 |
|---|---|---|---|
| SW-DET-001 | 软件 shall 内置 magic 表（4.4 节），对文件头 ≤16 字节做前缀匹配，优先级：精确 magic > 后缀 > 拒绝 | — | SYS-DEC-001 |
| SW-DET-002 | 软件 shall 建立数据类型分类器：后缀映射表（~200 个常用后缀 → 7 类：文本/可执行/结构化二进制/未压缩媒体/已压缩/随机/未知）+ 4MB 采样熵计算兜底 | 覆盖常用后缀 ≥200 项 | SYS-CMP-001 |
| SW-DET-003 | 当采样熵 ≥7.5 bits/byte 且无匹配后缀时，软件 shall 判定输入不可压缩并触发 SYS-CMP-003 跳过路径 | 熵阈值 7.5 | SYS-CMP-003 |
| SW-DET-004 | 软件 shall 提供 `omnic analyze <file>`：打印类型分类、熵、决策结果与理由，不执行压缩 | — | SYS-CMP-004 |

### 5.4 决策器 (SW-POL)

| 编号 | 需求描述 | 父需求 | 优先级 |
|---|---|---|---|
| SW-POL-001 | 决策器 shall 输入 (文件类型, 熵, profile)，输出 (codec, level, 决策理由)，实现为编译期查表 | SYS-CMP-001 | Must |
| SW-POL-002 | 一期内置决策表 shall 由 4.5 节 Pareto 数据初始化，覆盖 7 类数据类型 × 4 profile = 28 个决策单元 | SYS-CMP-001 | Must |
| SW-POL-003 | 软件 shall 支持 `--explain` 打印决策依据（类型、规则命中项、备选方案） | SYS-CMP-004 | Should |
| SW-POL-004 | 决策表 shall 由 `tools/train` 离线生成 C 源文件 `policy_table.gen.c`，构建时编入，运行时零外部依赖 | SYS-CMP-002 | Must |
| SW-POL-005 | 决策器 shall 保证确定性：相同 (类型, 熵区间, profile) 永远输出相同决策 | SYS-CMP-002 | Must |

### 5.5 适配层 (SW-CODEC)

| 编号 | 需求描述 | 父需求 | 优先级 |
|---|---|---|---|
| SW-CODEC-001 | 软件 shall 定义统一 codec 接口（ops 结构体：名称、格式 ID、级别域、压缩/解压函数指针、bound 计算），全部 adapter 实现该接口 | SYS-ARCH-001 | Must |
| SW-CODEC-002 | 软件 shall 维护运行时注册表：codec 名 → ops 指针，`omnic list --codecs` 可查询 | SYS-ARCH-001 | Must |
| SW-CODEC-003 | 每个 codec adapter shall 将底层错误码映射为统一错误枚举 + 上下文消息（文件名、字节偏移） | SYS-DEC-003 | Must |
| SW-CODEC-004 | 每个 codec adapter shall 实现流式分块 I/O（默认 4MB 块），不得整文件载入内存 | SYS-CMP-005 | Must |
| SW-CODEC-005 | CMake shall 提供每 codec 独立开关 `OMNIC_CODEC_<NAME>`，核心 10 族默认开 | SYS-ARCH-001 | Must |

### 5.6 训练管线 (SW-TRAIN，Python，离线)

| 编号 | 需求描述 | 父需求 | 优先级 |
|---|---|---|---|
| SW-TRAIN-001 | 管线 shall 提供 `bench`：对语料库 × (codec, level) 全矩阵测压缩率/压缩速度/解压速度，输出 CSV | SW-POL-002 | Must |
| SW-TRAIN-002 | 管线 shall 提供 `fit`：从 CSV 计算每 (数据类型, profile) 的 Pareto 最优 (codec, level)，生成 `policy_table.gen.c` | SW-POL-002 | Must |
| SW-TRAIN-003 | 语料库 shall 按类型组织于 `corpora/`（文本/JSON/代码/可执行/未压缩媒体/已压缩/随机，一期 ≥7 类 × ≥3 样本） | SW-TRAIN-001 | Must |
| SW-TRAIN-004 | 实验数据 CSV shall 入库保留，决策表可由数据重新生成（可复现性） | SW-POL-004 | Must |
| SW-TRAIN-005 | 二期：管线 shall 支持以 (类型特征向量, profile) 训练决策模型（决策树/GBDT，导出 C 查表），替代手工规则表 | — | Could |

### 5.7 CLI (SW-CLI) 与构建 (SW-INF)

| 编号 | 需求描述 | 优先级 |
|---|---|---|
| SW-CLI-001 | 子命令：`c`(compress) / `d`(decompress) / `analyze` / `list` / `version` / `help`；全局选项 `--profile --codec --level --threads --force --keep --quiet --explain --output-dir` | Must |
| SW-CLI-002 | 退出码：0 成功；1 使用错误；2 数据错误（损坏/不可识别）；3 系统错误（I/O 等） | Must |
| SW-CLI-003 | `--help` 输出遵循 POSIX 风格，`man` 手册页随发布提供 | Should |
| SW-INF-001 | 构建：CMake ≥3.16，C11；`-Wall -Wextra -Werror`（CI）；MSVC 与 GCC/Clang 双编译器 | Must |
| SW-INF-002 | 第三方库 vendored 进仓库（不依赖系统库），保证可复现构建 | Must |
| SW-INF-003 | GitHub Actions：三平台构建 + 测试 + release 静态单二进制产物 | Must |
| SW-INF-004 | 测试框架：C 内置极简断言宏 + 语料回归脚本；无第三方测试依赖 | Must |

---

## 6. 接口需求 (INT)

| 编号 | 需求描述 | 父需求 |
|---|---|---|
| INT-CLI-001 | 人机接口参数全集及语义见 SW-CLI-001，参数变更须保持向后兼容（一期冻结后） | BUS-001/002 |
| INT-LIB-001 | 核心能力 shall 以 C API `libomnic` 暴露（`omnic_compress(path, opts, report*)`, `omnic_decompress`, `omnic_analyze`），CLI 是其薄封装；二期 GUI 复用同一 API，不重复实现 | SYS-ARCH-001 |

---

## 7. 测试需求 (TST)

| 编号 | 需求描述 | 验证方法 | 父需求 |
|---|---|---|---|
| TST-FMT-001 | 交叉验证：omnic 压缩产物 → 官方 CLI 解压 → 与源文件逐字节一致 | 测试 | SYS-FMT-001 |
| TST-FMT-002 | 反向交叉：官方 CLI 压缩 → omnic 解压 → 逐字节一致 | 测试 | SYS-FMT-002 |
| TST-CODEC-001 | 每 codec roundtrip：语料集全样本压缩→自解压→逐字节一致 | 测试 | SW-CODEC-001 |
| TST-DEC-001 | 16 种格式 × 截断/位翻转 fuzz：只许报错，不许崩溃 | 测试 | SYS-DEC-003 |
| TST-POL-001 | 决策冒烟：每 (类型, profile) 断言输出落在决策表期望值 | 测试 | SW-POL-002 |
| TST-PERF-001 | 性能回归：zstd -6 基准文件吞吐不低于裸库 95%（CI 报告，不做硬门禁） | 测试 | SYS-PERF-001 |
| TST-PORT-001 | 三平台 CI 矩阵全绿 | 测试 | SYS-PORT-001 |
| TST-NEG-001 | 异常路径：目录输入（提示归档二期）、已存在输出、空文件、单字节文件、不可识别 magic | 测试 | SYS-DEC-002 |
| TST-LIC-001 | 许可证扫描：CI 脚本核对 vendored 库 license 与 THIRD_PARTY_NOTICES 一致 | 审查 | SYS-CODEC-003 |

---

## 8. 显式假设（待用户确认）

> 以下决策未与用户逐条确认，按工程惯例假设；如有异议请指出，将更新矩阵。

| # | 假设内容 | 影响范围 |
|---|---|---|
| A1 | 已压缩输入默认**跳过**压缩并警告，`--force` 才强压 | SYS-CMP-003, SW-DET-003 |
| A2 | 解压产物命名 = 去掉压缩后缀（原生格式不含原文件名，与 gzip/zstd 惯例一致） | SW-DEC-001 |
| A3 | 完整性校验完全依赖原生格式内建校验，不自加封装 | SYS-DEC-005 |
| A4 | 一期不做多文件/目录归档，目录输入明确报错提示二期 | TST-NEG-001 |
| A5 | GPL/LGPL 库（lzo/lzlib/bzip3/lizard）一期不集成 | SYS-CODEC-002 |
| A6 | 决策延迟预算：特征提取 ≤50ms（4MB 采样）+ 查表 ≤1ms | SYS-CMP-002 |
| A7 | 适配层吞吐损耗验收线 ≤5% | SYS-PERF-001 |
| A8 | 默认压缩成功后删除源文件（`--keep` 保留）——与 gzip/zstd 默认行为一致 | SW-CMP-004 |

## 9. 风险清单

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | GPL 库误链导致发布许可证污染 | 中 | 高 | 准入标准 + CI 许可证扫描 (TST-LIC-001) |
| R2 | 无 magic 格式（brotli/lzav/lzham/raw-deflate）误识别 | 中 | 中 | 后缀强提示 + 试解优先级链；文档明示 |
| R3 | vendored 12 库导致构建慢、仓库膨胀 | 高 | 中 | 每 codec CMake 开关；核心 10 族默认；浅 vendor（裁剪文档/测试） |
| R4 | 内置决策表对真实数据类型过拟合 | 中 | 中 | 一期表仅作初值；训练管线持续迭代；`--explain` 透明化 |
| R5 | 三平台 CI 维护成本 | 中 | 中 | GitHub Actions 矩阵；MSVC/MinGW 双编译 Windows |
| R6 | 上游库安全公告 | 低 | 高 | SYS-CODEC-004 移除机制 |
| R7 | lzham (C++) 与 C 核心链接/ABI 问题 | 低 | 低 | adapter 层以 `extern "C"` 隔离 |
| R8 | 大文件性能不达标（适配层拷贝过多） | 低 | 中 | 统一块 buffer 复用；TST-PERF-001 回归 |

## 10. WBS（一期）

| WP | 工作包 | 内容 | 依赖 | 估算(人日) |
|---|---|---|---|---|
| WP1 | 仓库脚手架 | CMake、CI 矩阵、目录、LICENSE、NOTICES | — | 1 |
| WP2 | codec 抽象层 | ops 接口、注册表、错误映射、块 I/O 框架 | WP1 | 2 |
| WP3 | 核心 codec ×10 | zstd/lz4/zlib/zlib-ng/libdeflate/brotli/xz/bzip2/snappy/lzfse | WP2 | 3 |
| WP4 | 探测层 | magic 表生成器、后缀分类表、熵采样 | WP1 | 1.5 |
| WP5 | 决策器一期 | 内置表、查表引擎、--explain | WP4 | 1.5 |
| WP6 | CLI + libomnic API | 子命令、退出码、报告输出、原子写 | WP2,WP5 | 2 |
| WP7 | 训练管线 MVP | bench/fit 工具、语料组织、生成 policy_table.gen.c | WP3,WP4 | 3 |
| WP8 | 解压路径 | 16 格式接入验证、lzip 容器、自研 LZW | WP3 | 2 |
| WP9 | 可选 codec ×2 | lzham、lzav（含 extern "C" 隔离） | WP2 | 1 |
| WP10 | 测试与回归 | 交叉/反向/fuzz/性能/异常用例全量 | WP3-8 | 3 |
| WP11 | 文档与发布 | README、man、THIRD_PARTY_NOTICES、release | 全部 | 1 |

**一期合计约 21 人日。关键路径：WP1→WP2→WP3→WP6→WP10。**

## 11. 一期验收标准 (Definition of Done)

1. 三平台 CI 绿：构建 + TST-CODEC/FMT/DEC/POL/NEG 全通过。
2. `omnic c README.md` 输出 `README.md.zst`（balanced 档期望 zstd），`zstd -d` 可解，逐字节一致。
3. `omnic c --profile best_ratio README.md` 期望 xz，`xz -d` 可解。
4. `omnic d` 对 16 种格式官方 CLI 产物全部可解。
5. 对 .jpg 输入默认跳过并警告；`--force` 后照常压缩。
6. 决策延迟 ≤50ms（1GB 文件，采样路径）。
7. `omnic analyze demo.json` 输出类型/熵/决策及理由。
