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
    dispatcher.c (497)     ★ 决策心脏：decide() 五步链 + 试压（2026-11-28 实测）
    archive.c   (3213)    ★ .omnic 归档：pack/unpack/索引/扩展区/镜像/四个线程池（2026-11-28 实测）
    registry.c   (75)     codec 注册表（新增 codec = 数组加一行）
    fs.c        (1085)    目录遍历/跨平台/链接/属主与 xattr 采集（2026-11-28 实测）
    blockio.c   (113)     reader/writer 抽象
    util.c/arc_sync.h      时间/错误/线程原语
  detect/                  magic.c / feature.c(8 维特征+γ 细分) / entropy.c / filetype.c
  policy/                   policy_table.gen.c(训练生成的 12 类×4 档表) / policy.c
  codec/                   12 个适配层 ac_*.c（88~245 行/个），契约见 codec.h
tests/                     test_core.c(1434，654 checks) + test_archive.c(2569，363 checks)（2026-11-28 实测）
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
        → solid 块(64MiB 上限) 压缩池并行写出 → 尾部索引(zstd)
        → 扩展区(属主/xattr，有数据才写) → Footer 32B
        → 回填 n_blocks / 扩展区偏移 / data_off(带 --mirror 时)
unpack: Header → Footer → 尾部索引(坏了且有镜像则退回头部镜像 = 降级模式)
        → 解码池逐块 decompress + 块 CRC → 条目 CRC → 写盘池
        → 硬链接 → 符号链接/联接 → 目录权限 → 属主/xattr(降级模式没有)
        → --file 三模式过滤 / 坏块隔离 / store 块逐条目抢救，退出码 2
产物布局: [Header 32][镜像(可选)][数据区 块×N][索引 zstd][扩展区(可选)][Footer 32B]
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
| 分类 cls_worker | archive.c:786 | 8（-T 调） | pack，≥32 文件，非 explain |
| 压缩 cmp_worker | archive.c:868 | 同分类 | pack 写块，压缩与写盘重叠 |
| 解码 dec_worker | archive.c:2284 | 8（-T 调） | unpack 逐块解码 |
| 写盘 writer_main | archive.c:2401 | 8（-T 调） | unpack 写文件 |
| zstd/xz MT | ac_zstd.c:92 / ac_xz.c | -T 显式 | c/pack 且 -T>1 |
（行号为 2026-11-28 实测；代码演进后先重读再引用）

## 纪律（本项目血泪换的）

- **数字纪律**：一切性能数字同机同窗口实测；无实测不报数；公开口径与
  README 零漂移；输面照录（7-Zip 对照的输赢都印在 README）
- **新增并发路径**：回归 + 变异验收 + TSan 三件套，缺一不可（zstd -T>1
  截断在 v0.3.1 带伤上线的教训——457 checks 全绿照样漏）
- **门禁要先验证机制存在**：写过 RETRY_FAIL（ctest 无此属性，从未生效）。
  写"自动重跑"类承诺前 `cmake --help-property-list` 查存在 + 看一次真实触发。
  同理，改 CI 脚本后用 `C:\Program Files\Git\bin\bash.exe -n` 过语法，再拿一份
  仿真的 test.log 干跑一遍——"dump" 步骤曾因 grep 无匹配而自己先失败，什么都没发出来
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
- **归档格式改动**：FORMAT.md 的"兼容性承诺"是硬约束——不改既有记录的布局、
  不加新 flag 位；新元数据进扩展区（tag 自带，v2 版本号不变）；头部 24..32
  已给扩展区，没有空闲保留字节。`data_off` 目前唯一的非 32 用途是 `--mirror`
- **`--mirror` 预留按索引明文上界算**（写入前必须定大小），压缩索引只占其中
  一部分、其余为零——游戏树上 +0.87%。改索引记录的长度时重算这个比例
- **归档以 `"wb"` 打开，只写句柄读不了**：别在 pack 里从归档文件读回数据，
  要用就趁缓冲区还在（见索引镜像的写入）
- **`arc_index_parse` 失败时 `ix` 是半初始化的**：`nent` 已设、`ent` 可能为
  NULL 或部分填充。清理走 `arc_index_mem_free`，它已处理 `ent == NULL`；
  `arc_open_index` 入口先清零 `ix`（调用方传来的未初始化）
- **store 块 vs 压缩块的测试数据**：想让文件直通进 store 块，给文件加已压缩
  格式的魔数（如 PNG 头）。LCG/"看起来随机"的数据会被 zstd 压成几百字节，
  进的是压缩块——别靠猜熵，先用 `--list` 看它进了哪种块
- **测试输出目录含目录联接时不能 `rd /s /q`**：它会顺着联接走进目标目录，
  留下半删除的树（`build\omnic_arc_lk_out` 那次）。用 `rm_tree_deep`
- **测试从 `build\` 直接跑可能撞残留**：从干净目录跑 `test_archive.exe`
  （v1 样本路径已由 CMake 以 `OMNIC_SOURCE_DIR` 注入，不依赖工作目录）

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

## 查 CI（2026-11-28 实测）

- **没有 API 令牌**：推送走 SSH 密钥，那只管推拉代码。日志（`jobs/{id}/logs`）和
  产物下载要令牌，会 401/403。**能匿名读的**：运行与各腿的成败、以及每个
  check-run 的 annotations。别再去找令牌，用下面的办法拿失败原因。
- 查询：`actions/runs?head_sha=<完整 40 位 SHA>`（短 SHA 返回 0 个运行，会误判为
  没触发）→ `jobs_url` → `check-runs/{job_id}/annotations`。匿名配额 60 次/小时，
  一个回合控制在 10 次内；等结果用定时任务回来查，不要轮询。
- **失败原因在 annotations 里**：`ci.yml` 的 "dump test failures as annotations"
  会发三类行——`FAILED-CHECK <行号>: <条件>`（测试程序在总结行之后重打的清单，
  不受 ctest 输出截断影响）、`N checks, M failed`、ctest 的 `Test #k` 总结。
- 还看不出来时，**让 CI 吐答案**：在出问题的测试里去掉 `OMNIC_F_QUIET`、
  `fprintf(stderr, "...err='%s'", omnic_last_error())`，标 `TEMP`，推上去，
  下一个提交撤掉。
- **耗时基线**：CI 绿的时候 test 步骤本来就要 500–900 s（bench_smoke 450–850 s，
  它是计时测试，慢是正常的）。看到"十几分钟"不是卡死，先和历史比。
- **ctest 超时**（CMakeLists.txt 末尾）：core/archive 900 s，bench_smoke 1800 s，
  都按 CI 实测定。新增测试要先量再设，别凭感觉填数。
- **本地是 Windows，CI 有四个 POSIX 腿**：POSIX 上每个条目都有属主 → 每个归档都
  带扩展区，Windows 上永远没有。所以"Windows 绿"对扩展区、xattr、属主这些路径
  什么都不证明。套件里的 `test_area_index_rewrite` 用注入扩展区的方式在所有平台
  上覆盖了"带扩展区 + 重写索引"；改这类路径时照它写，先在本地验证再推。

## 本机环境备忘（AI 用）

- 语料：Silesia 12 文件标准形态 = `build\silesia12`（211,938,580B，无扩展名
  的 12 个）；`build\silesia` 是带扩展名的 24 个副本（成对重复）。AGENTS 旧版
  写的 `%TEMP%\opencode\7zcmp\silesia12` **实测为空，勿用**。游戏树解包 =
  `%TEMP%\opencode\gametree\tree`（8,586 文件 / 152,644,069B）；
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
- 文中行号基于 b82a973，带"2026-11-28 实测"标注的除外；代码演进后先重读再引用
