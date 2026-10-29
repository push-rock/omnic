# omnic

**通用压缩器** —— 一个二进制，自动为每个文件挑选最合适的压缩算法，并且能解开市面上绝大多数压缩格式。

[English](README.md) | [简体中文](README.zh-CN.md)

```
$ omnic c report.json
report.json: zstd | balanced | text | level 8
    1048576 -> 91234 bytes (8.70%), 0.091s, 11.5 MB/s

$ omnic c --profile best_ratio report.json
report.json: xz | best_ratio | text | level 9
    1048576 -> 61042 bytes (5.82%), 0.322s, 3.3 MB/s

$ omnic d report.json.xz
report.json.xz: xz
    61042 -> 1048576 bytes, 0.019s, 55.2 MB/s
```

## 为什么做这个项目

lzbench 基准测试收录了 50 多种压缩器，但没有任何主流工具在它们之间做路由。`zstd --adapt` 只在单一算法内部自适应级别；7-Zip 不关心输入是 JSON 还是位图。omnic 补上这一环：

- **压缩**：先识别输入（后缀 + 4MB 熵采样），再查决策表 —— *7 种数据类型 × 4 种偏好模式* —— 得到 `(算法, 级别)`；
- **偏好**：`--profile fast_compress | fast_decompress | best_ratio | balanced`（快压 / 快解 / 高压缩率 / 均衡）；
- **解压**：魔数（magic number）优先识别格式、后缀兜底。输出只用各算法原生格式，任何第三方工具都能解开；
- **跳过不可压缩数据**：已压缩输入（jpeg/zip/…）和高熵数据（≥7.5 bits/byte）默认跳过并警告；`--force` 强制压缩。

## 构建

需要 CMake ≥ 3.16 和 C11 编译器（MSVC / GCC / Clang）。所有算法库均已内置（vendored），无系统依赖。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/omnic list
ctest --test-dir build
```

## 用法

```
omnic c   [选项] <文件...>     压缩（默认自动选算法，-c 可指定）
omnic d   [选项] <文件...>     解压（魔数/后缀自动识别格式）
omnic analyze <文件...>        只分析类型/熵/决策，不产生副作用
omnic list                     列出算法与支持的格式

-p --profile <v>   fast_compress | fast_decompress | best_ratio | balanced
-c --codec <名>    指定算法                  -l --level <n>  算法原生级别
-T --threads <n>   压缩线程数（zstd）        -o --output-dir <目录>
-f --force    覆盖已存在输出 / 强制压缩不可压缩数据
-k --keep     保留源文件（默认：成功后删除）
-x --explain  打印决策依据                   -q --quiet
```

## 算法清单

| 算法 | 格式 | 级别 | 说明 |
|---|---|---|---|
| zstd 1.5.7 | .zst | -5..22 | 支持多线程 |
| lz4 1.10.0 | .lz4 | 0..12 | 级别 0 = fast 模式，1..12 = LZ4HC |
| xz 5.8.4 | .xz | 0..9 | CRC64 校验 |

更多算法（zlib / zlib-ng / libdeflate / brotli / bzip2 / snappy / lzfse / lzham / lzav）正基于同一套适配器接口陆续接入 —— 完整的准入清单（按许可证筛选）见 `docs/REQUIREMENTS.md` 第 4.5 节。解压端可识别的格式：gzip、zlib、deflate、zstd、xz、lzma、lzip、bzip2、lz4、brotli、snappy、compress(.Z)、lzfse、lzvn。

## 决策是怎么做的

决策表编译进二进制（`src/policy/policy_table.gen.c`），初值来自 lzbench 实测数据，并可由离线训练管线（`tools/train`，Python：`bench.py` → CSV → `fit.py` → C 决策表）用真实数据重新生成。决策过程就是查表 —— 微秒级，无任何模型运行时依赖。

## 文档

- `docs/REQUIREMENTS.md` —— 需求基线（73 条受控需求）
- `docs/ARCHITECTURE.md` —— 架构设计、codec 接口、决策管线

## 许可证

MIT。内置的各算法库保留其原始许可证，见 `THIRD_PARTY_NOTICES.md`。默认不链接任何 GPL/LGPL 代码（`OMNIC_ENABLE_GPL` 预留给可选构建）。
