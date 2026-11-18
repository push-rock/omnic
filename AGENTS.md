# AGENTS.md — omnic 工程说明（AI 协作者必读）

> 本文件由 AI 实测生成（2026-11-18，基于 `b82a973`），随仓库演进维护。
> 给 AI 的入口文档：结构、调用链、纪律、坑。人看 README.md。

## 项目是什么

omnic：C11 零依赖通用压缩器。16 codec / 14 解压格式，逐文件自动选算法，
自带 .omnic 归档格式（坏块隔离）。性能与二进制体积优先，单二进制静态分发。

## 仓库结构（行数为实测）

```
src/
  include/omnic.h          公共 API 唯一头文件（CLI 与未来 GUI 共用）
  cli/                     薄壳：main.c 分发 / argp.c 参数 / report.c 打印
  core/
    api.c        (82)      omnic_* 入口 → dispatcher 转发
    dispatcher.c (456)     ★ 决策心脏：decide() 五步链 + 试压
    archive.c   (1518)     ★ .omnic 归档：pack/unpack/索引/两个线程池
    registry.c   (75)     codec 注册表（新增 codec = 数组加一行）
    fs.c        (385)     目录遍历/跨平台
    blockio.c   (113)     reader/writer 抽象
    util.c/arc_sync.h      时间/错误/线程原语
  detect/                  magic.c / feature.c(8 维特征+γ 细分) / entropy.c / filetype.c
  policy/                   policy_table.gen.c(训练生成的 12 类×4 档表) / policy.c
  codec/                   12 个适配层 ac_*.c（88~245 行/个），契约见 codec.h
tests/                     test_core.c(1,173) + test_archive.c(819)，475+108 checks
tools/                     bench.c(822) / gamepack.c(367) / fuzz_smoke.py / 训练脚本
third_party/               10 个 vendored 库（zstd xz brotli lz4 lzfse snappy
                           zlib zlib-ng libdeflate bzip2），全静态链接
docs/                      本地文档，gitignored，不入公开仓库
.github/workflows/         ci.yml（6 腿）+ release.yml（tag 自动发布）
```

## 调用链

单文件（`omnic c/d/a`）：
```
main → api.c → dispatcher
  压缩: decide() = 扩展名初判 → 三段采样(头/中/尾 256KiB) → magic 优先(可推翻
        扩展名，熵护栏双向 <7.5/≥7.5) → 8 维特征 γ 细分 → policy_lookup
        (12类×4档) → FC档且≥32MiB: 256KiB 试压双候选比 ρ
        → ops->compress(level, threads)
  解压: magic → fmt → ops->decompress（无决策）
```

归档（`omnic pack/unpack`）：
```
pack:   fs_walk → 决策池并行(8 cls_worker，≥32文件才开池；-T1/explain/小树
        串行，产物与并行逐字节相同——CI 门禁锁定) → 按 codec 聚桶 + store 桶
        → solid 块(64MiB 上限) ops->compress → 尾部索引(zstd) + Footer32B
unpack: Footer → 索引 → 主线程逐块 decompress + CRC → 8-writer 写盘池
        → --file 三模式过滤 / 坏块隔离(块 CRC 挂只废该块，退出码 2)
产物布局: [数据区 块×N][索引 zstd][Footer 32B]
```

## codec 契约（动 codec 前必读）

- ops = {name, fmt, ext, compress(reader,writer,level,threads),
  decompress(reader,writer)}。**解压侧无状态**——归档解包每块从零建 ctx，
  这是 per-file 解压 21.5 MiB/s 的病灶（T1 候选：归档层持 ctx 池 + ops 钩子）
- 吃 threads 的仅两个：zstd（ZSTD_c_nbWorkers）、xz（liblzma MT，2GiB 内存帽
  自动降线程）。新增并行路径必须配 MT roundtrip 回归（语料产物跨 4,210,688B
  缓冲区边界）+ 变异验收 + TSan
- MT codec 只承诺解码等价（-T2 与 -T8 产物可不同字节）；分类池承诺逐字节相同

## 线程清单

| 池 | 位置 | 规模 | 触发 |
|---|---|---|---|
| 分类 cls_worker | archive.c:582 | 8（-T 调） | pack，≥32 文件，非 explain |
| 解压写盘 | archive.c:1403 | 8（-T 调） | unpack 默认 |
| zstd/xz MT | ac_zstd.c:31 / ac_xz.c:112 | -T 显式 | c/pack 且 -T>1 |

## 纪律（本项目血泪换的）

- **数字纪律**：一切性能数字同机同窗口实测；无实测不报数；公开口径与
  README 零漂移；输面照录（7-Zip 对照的输赢都印在 README）
- **新增并发路径**：回归 + 变异验收 + TSan 三件套，缺一不可（zstd -T>1
  截断在 v0.3.1 带伤上线的教训——457 checks 全绿照样漏）
- **门禁要先验证机制存在**：写过 RETRY_FAIL（ctest 无此属性，从未生效）。
  写"自动重跑"类承诺前 `cmake --help-property-list` 查存在 + 看一次真实触发
- **插档下时序断言失效**：TSan 豁免用 `__has_feature` 编译期，且全库扫同类
  （时间断言指纹 `ms <|sec <|clock\(`），豁免范围进 CHANGELOG
- **vendored 三方库对 -fsanitize 有自我保护**（xz 拒绝时报错自带正解
  -DXZ_SANDBOX=no）；configure 死亡先完整读 CMake Error
- **C11 禁全局动态初始化对象**；RTTI/异常关闭；注释要瘦——只写代码看不出
  的"为什么"，不逐行解释
- commit 一行式：`[模块:类型] 摘要`，模块 Core/Codec/CLI/Infra/Doc，
  类型 Feature/Bugfix/Perf/Refact/Doc/Test/Chore/Release
- docs/ gitignored（内部文档/推广物料/凭据不入公开仓）
- **编辑锚点**：oldString 从刚读到的原文逐字复制（凭记忆抄必被拦）；缩进
  存疑用字节级替换。行号前缀显示会误导目测缩进

## 构建/测试/发布

```
工具链: D:\toolchain\mingw64\bin\cmake.exe（cmake 不在 PATH）
本地:   cmake --build build -j && build\test_core.exe && build\test_archive.exe
CI:     ci.yml 六腿（ubuntu gcc/clang/clang+TSan, macos, win cl/MinGW）
        失败重跑 --rerun-failed（单次）；TSan 腿跑 core+archive 豁免两时序断言
发布:   release.yml 监听 tag v* → 三平台构建+测试 → CHANGELOG 自动抽段
        作 release 正文 → 自动建 Release。**CHANGELOG 即 release notes，
        写的顺序就是发布页顺序**（重打包警告放段首）
版本号: src/include/omnic.h OMNIC_VERSION + CHANGELOG [Unreleased]→[版本]
tag:    v0.3.1 起用附注标签（annotated：git tag -a vX.Y.Z -m "..."）
```

## 本机环境备忘（AI 用）

- 语料：Silesia 12 文件 = `%TEMP%\opencode\7zcmp\silesia12`（211,938,580B，
  7zr.exe 同目录）；游戏树解包 = `%TEMP%\opencode\gametree\tree`；
  本地语料 `build\gamepack.tar`（152,644,069B）**未删勿再误诊**
- 回归基线：Silesia pack 28.97%/61,402,164B；dickens xz-6 SHA-256
  `7C54B2024B21F4D3ECBF182E113B5F5152D9B3F925B557D008C61D396D311595`；
  MT xz -T8 于 202MB tar 输出 49,502,620B 且 -T8≡-T16
- PS 5.1 坑（反复咬人）：无 `&&`；`-Raw`/`-TotalCount` 不兼容；单行命令输出
  的 `[0]` 是 Char（用 `@(...)[0]`）；`-Include` 需配 `-Recurse` 或用 Where-Object；
  合成参数传原生 exe 被拒需拆分；后台任务跨小时会静默死——当日收工前前台
  直查 CI 终态
- 网络天气（2026-11 以来）：GitHub API 间歇超时；诊断小路 = check-runs
  annotations API（几 KB 拿 FAIL 行，100KB 日志拉不动时用）
- 文中所有行号基于 b82a973；代码演进后先重读再引用
