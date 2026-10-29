# -*- coding: utf-8 -*-
"""Generate omnic requirements matrix xlsx from REQUIREMENTS.md baseline v0.1."""
import openpyxl
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

FONT = "Microsoft YaHei"
HDR_FILL = PatternFill("solid", start_color="1F4E79")
ALT_FILL = PatternFill("solid", start_color="EFF3F9")
TITLE_FONT = Font(name=FONT, size=14, bold=True, color="1F4E79")
HDR_FONT = Font(name=FONT, size=10, bold=True, color="FFFFFF")
BODY_FONT = Font(name=FONT, size=10)
BOLD_FONT = Font(name=FONT, size=10, bold=True)
THIN = Border(*[Side(style="thin", color="C5D3E8")] * 4)
WRAP = Alignment(wrap_text=True, vertical="top")
CENTER = Alignment(horizontal="center", vertical="top")

# (编号, 层级, 模块, 描述, 量化指标, 父需求, 验证方法, 优先级, 阶段, 假设/备注)
REQS = [
    ("BUS-001", "BUS", "业务", "系统 shall 在用户压缩文件时自动选择最合适的算法与级别，无需用户具备压缩算法知识", "—", "—", "D", "Must", "一期", ""),
    ("BUS-002", "BUS", "业务", "系统 shall 提供三种用户偏好（压缩更快/解压更快/压缩率更好）及均衡默认档", "4 种偏好", "—", "D", "Must", "一期", ""),
    ("BUS-003", "BUS", "业务", "压缩产物 shall 采用算法原生格式与后缀，可被市面主流工具直接解开", "—", "—", "T", "Must", "一期", ""),
    ("BUS-004", "BUS", "业务", "解压端 shall 覆盖市面上大多数压缩流格式", "一期 16 种/二期含归档", "—", "T", "Must", "一期", ""),
    ("BUS-005", "BUS", "业务", "项目 shall 以开源形式发布于 GitHub，三平台提供预编译单二进制", "3 平台", "—", "T", "Must", "一期", ""),
    ("SYS-CMP-001", "SYS", "压缩", "当用户压缩一个文件时，系统 shall 依次执行类型识别→决策器选择→codec 执行→输出原生格式文件", "—", "BUS-001", "T", "Must", "一期", ""),
    ("SYS-CMP-002", "SYS", "压缩", "决策器 shall 在特征提取完成后给出决策（查表≤1ms，特征提取含 4MB 采样分析≤50ms，不含磁盘 I/O）", "≤50ms + ≤1ms", "BUS-001", "T", "Must", "一期", "假设 A6"),
    ("SYS-CMP-003", "SYS", "压缩", "当输入被判定为已压缩数据时，系统 shall 默认跳过压缩并警告；--force 后 shall 执行压缩", "—", "BUS-001", "T", "Must", "一期", "假设 A1"),
    ("SYS-CMP-004", "SYS", "压缩", "系统 shall 报告压缩结果：算法、级别、决策依据、耗时、原始/压缩大小、压缩率、吞吐", "—", "BUS-002", "D", "Must", "一期", ""),
    ("SYS-CMP-005", "SYS", "压缩", "压缩 shall 采用流式处理，内存占用与文件大小无关", "≤256MB", "BUS-001", "T", "Must", "一期", ""),
    ("SYS-DEC-001", "SYS", "解压", "当用户解压文件时，系统 shall 以 magic number 优先识别格式，后缀兜底，冲突时以 magic 为准并提示", "识别准确率 100%", "BUS-004", "T", "Must", "一期", ""),
    ("SYS-DEC-002", "SYS", "解压", "当格式无法识别时，系统 shall 以非零退出码退出并打印支持格式清单", "—", "BUS-004", "T", "Must", "一期", ""),
    ("SYS-DEC-003", "SYS", "解压", "当压缩流损坏或截断时，系统 shall 在首个错误字节报告错误并以非零退出码退出，不得崩溃", "零崩溃（fuzz）", "BUS-004", "T", "Must", "一期", ""),
    ("SYS-DEC-004", "SYS", "解压", "解压 shall 采用流式处理", "≤256MB", "BUS-004", "T", "Must", "一期", ""),
    ("SYS-DEC-005", "SYS", "解压", "完整性校验 shall 透传原生格式内建校验（gzip CRC32、xz CRC64、zstd XXH64 等），不额外封装", "—", "BUS-003", "R", "Must", "一期", "假设 A3"),
    ("SYS-FMT-001", "SYS", "格式", "omnic 压缩的产物 shall 能被对应官方参考实现 CLI 解开（交叉验证）", "—", "BUS-003", "T", "Must", "一期", ""),
    ("SYS-FMT-002", "SYS", "格式", "系统 shall 解压官方参考实现 CLI 产出的文件（反向交叉验证）", "—", "BUS-004", "T", "Must", "一期", ""),
    ("SYS-CODEC-001", "SYS", "算法准入", "codec 准入 shall 同时满足：宽松许可证、维护中、纯 C/C++ 工具链、三平台可编译", "4 条标准", "BUS-005", "R", "Must", "一期", ""),
    ("SYS-CODEC-002", "SYS", "算法准入", "当候选库仅提供 GPL/LGPL 许可（lzo/lzlib/bzip3/lizard）时，系统 shall 不默认集成，须 OMNIC_ENABLE_GPL 隔离构建", "—", "BUS-005", "R", "Must", "一期", "假设 A5"),
    ("SYS-CODEC-003", "SYS", "算法准入", "所有 vendored 库 shall 在 THIRD_PARTY_NOTICES.md 登记来源、版本、许可证原文", "—", "BUS-005", "R", "Must", "一期", ""),
    ("SYS-CODEC-004", "SYS", "算法准入", "当上游库出现无补丁安全公告时，系统 shall 在一个发布周期内移除或替换该 codec", "—", "BUS-005", "R", "Should", "一期", ""),
    ("SYS-PORT-001", "SYS", "可移植", "系统 shall 运行于 Windows 10+ x64、Ubuntu 22.04+ x64、macOS 12+（x64/arm64），CI 三平台全绿", "3 平台×2 编译器", "BUS-005", "T", "Must", "一期", ""),
    ("SYS-PERF-001", "SYS", "性能", "适配层引入的吞吐损耗 shall 不超过裸库基准的 5%（同机同文件对比官方 CLI）", "≥95% 裸库吞吐", "BUS-001", "T", "Must", "一期", "假设 A7"),
    ("SYS-PERF-002", "SYS", "性能", "zstd codec shall 支持原生多线程压缩（frame 内多 job）；其余 codec 一期单线程", "加速比≥0.7×线程数", "SYS-PERF-001", "T", "Should", "一期", ""),
    ("SYS-ARCH-001", "SYS", "架构", "codec shall 以注册表模式接入：新增 codec 不修改核心调度代码", "≤2 文件改动", "BUS-001", "R", "Must", "一期", ""),
    ("SW-CMP-001", "SW", "压缩", "软件 shall 支持 omnic c <file...> 自动压缩，输出 <file>.<ext>", "—", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-CMP-002", "SW", "压缩", "软件 shall 支持 --profile fast_compress|fast_decompress|best_ratio|balanced，默认 balanced", "4 档", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-CMP-003", "SW", "压缩", "软件 shall 支持 --codec <name> --level <n> 手动覆盖自动决策", "—", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-CMP-004", "SW", "压缩", "软件 shall 支持 --keep 保留源文件；默认压缩成功后删除源文件，失败时保留", "—", "SYS-CMP-001", "T", "Should", "一期", "假设 A8"),
    ("SW-CMP-005", "SW", "压缩", "软件 shall 以临时文件+原子 rename 落盘，压缩失败不得留下半成品", "—", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-CMP-006", "SW", "压缩", "当输出文件已存在时，软件 shall 报错退出，除非 --force", "—", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-CMP-007", "SW", "压缩", "软件 shall 支持 -（stdin→stdout）管道模式，决策基于 stdin 前 4MB 采样", "—", "SYS-CMP-001", "T", "Should", "一期", ""),
    ("SW-CMP-008", "SW", "压缩", "软件 shall 在 tty 附加时显示进度（stderr），--quiet 关闭", "—", "SYS-CMP-004", "D", "Should", "一期", ""),
    ("SW-DEC-001", "SW", "解压", "软件 shall 支持 omnic d <file...> 自动识别格式并解压，输出文件名=去掉压缩后缀", "—", "SYS-DEC-001", "T", "Must", "一期", "假设 A2"),
    ("SW-DEC-002", "SW", "解压", "软件 shall 支持 omnic list 打印全部支持格式（压缩/解压分列）", "—", "SYS-DEC-002", "D", "Must", "一期", ""),
    ("SW-DEC-003", "SW", "解压", "软件 shall 支持同批混合格式输入，逐文件独立处理，单文件失败不影响后续，退出码取最大错误级", "—", "SYS-DEC-003", "T", "Must", "一期", ""),
    ("SW-DET-001", "SW", "探测", "软件 shall 内置 magic 表对文件头≤16 字节做前缀匹配，优先级：精确 magic>后缀>拒绝", "16 格式", "SYS-DEC-001", "T", "Must", "一期", ""),
    ("SW-DET-002", "SW", "探测", "软件 shall 建立后缀→7 类数据类型映射表并以 4MB 采样熵计算兜底", "≥200 后缀", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-DET-003", "SW", "探测", "当采样熵≥7.5bits/byte 且无匹配后缀时，软件 shall 判定不可压缩并触发跳过路径", "熵阈值 7.5", "SYS-CMP-003", "T", "Must", "一期", "假设 A1"),
    ("SW-DET-004", "SW", "探测", "软件 shall 提供 omnic analyze：打印类型分类、熵、决策结果与理由，不执行压缩", "—", "SYS-CMP-004", "D", "Must", "一期", ""),
    ("SW-POL-001", "SW", "决策器", "决策器 shall 输入(文件类型,熵,profile)输出(codec,level,决策理由)，实现为编译期查表", "—", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-POL-002", "SW", "决策器", "一期内置决策表 shall 由 lzbench Pareto 数据初始化，覆盖 7 类数据类型×4 profile", "28 决策单元", "SYS-CMP-001", "T", "Must", "一期", ""),
    ("SW-POL-003", "SW", "决策器", "软件 shall 支持 --explain 打印决策依据（类型、规则命中项、备选方案）", "—", "SYS-CMP-004", "D", "Should", "一期", ""),
    ("SW-POL-004", "SW", "决策器", "决策表 shall 由 tools/train 离线生成 C 源文件 policy_table.gen.c 编入二进制，运行时零外部依赖", "—", "SYS-CMP-002", "R", "Must", "一期", ""),
    ("SW-POL-005", "SW", "决策器", "决策器 shall 保证确定性：相同(类型,熵区间,profile)永远输出相同决策", "—", "SYS-CMP-002", "T", "Must", "一期", ""),
    ("SW-CODEC-001", "SW", "适配层", "软件 shall 定义统一 codec ops 接口（名称、格式 ID、级别域、流式函数指针、bound），全部 adapter 实现该接口", "—", "SYS-ARCH-001", "R", "Must", "一期", ""),
    ("SW-CODEC-002", "SW", "适配层", "软件 shall 维护运行时注册表：codec 名→ops 指针，omnic list --codecs 可查询", "—", "SYS-ARCH-001", "D", "Must", "一期", ""),
    ("SW-CODEC-003", "SW", "适配层", "每个 codec adapter shall 将底层错误码映射为统一错误枚举+上下文消息（文件名、字节偏移）", "—", "SYS-DEC-003", "T", "Must", "一期", ""),
    ("SW-CODEC-004", "SW", "适配层", "每个 codec adapter shall 实现流式分块 I/O（默认 4MB 块），不得整文件载入内存", "4MB 块", "SYS-CMP-005", "T", "Must", "一期", ""),
    ("SW-CODEC-005", "SW", "适配层", "CMake shall 提供每 codec 独立开关 OMNIC_CODEC_<NAME>，核心 10 族默认开", "10 默认开", "SYS-ARCH-001", "R", "Must", "一期", ""),
    ("SW-TRAIN-001", "SW", "训练管线", "管线 shall 提供 bench：对语料库×(codec,level)全矩阵测压缩率/压缩速度/解压速度，输出 CSV", "—", "SW-POL-002", "T", "Must", "一期", ""),
    ("SW-TRAIN-002", "SW", "训练管线", "管线 shall 提供 fit：从 CSV 计算每(数据类型,profile)的 Pareto 最优并生成 policy_table.gen.c", "—", "SW-POL-002", "T", "Must", "一期", ""),
    ("SW-TRAIN-003", "SW", "训练管线", "语料库 shall 按类型组织于 corpora/（7 类，一期≥7类×≥3样本）", "≥21 样本", "SW-TRAIN-001", "R", "Must", "一期", ""),
    ("SW-TRAIN-004", "SW", "训练管线", "实验数据 CSV shall 入库保留，决策表可由数据重新生成（可复现性）", "—", "SW-POL-004", "R", "Must", "一期", ""),
    ("SW-TRAIN-005", "SW", "训练管线", "二期管线 shall 支持以(类型特征向量,profile)训练决策模型（决策树/GBDT 导出 C 查表）替代手工规则表", "—", "SW-POL-002", "T", "Could", "二期", ""),
    ("SW-CLI-001", "SW", "CLI", "子命令：c/d/analyze/list/version/help；全局选项 --profile --codec --level --threads --force --keep --quiet --explain --output-dir", "6 子命令", "INT-CLI-001", "D", "Must", "一期", ""),
    ("SW-CLI-002", "SW", "CLI", "退出码：0 成功；1 使用错误；2 数据错误；3 系统错误", "4 级", "SYS-DEC-002", "T", "Must", "一期", ""),
    ("SW-CLI-003", "SW", "CLI", "--help 输出遵循 POSIX 风格，man 手册页随发布提供", "—", "INT-CLI-001", "R", "Should", "一期", ""),
    ("SW-INF-001", "SW", "构建", "构建：CMake≥3.16，C11；-Wall -Wextra（CI 加 -Werror）；MSVC 与 GCC/Clang 双编译器", "—", "SYS-PORT-001", "T", "Must", "一期", ""),
    ("SW-INF-002", "SW", "构建", "第三方库 vendored 进仓库（不依赖系统库），保证可复现构建", "—", "SYS-CODEC-003", "R", "Must", "一期", ""),
    ("SW-INF-003", "SW", "构建", "GitHub Actions：三平台构建+测试+release 静态单二进制产物", "—", "SYS-PORT-001", "T", "Must", "一期", ""),
    ("SW-INF-004", "SW", "构建", "测试框架：C 内置极简断言宏+语料回归脚本；无第三方测试依赖", "—", "TST-CODEC-001", "R", "Must", "一期", ""),
    ("INT-CLI-001", "INT", "人机接口", "CLI 参数全集及语义见 SW-CLI-001，一期冻结后变更须保持向后兼容", "—", "BUS-001, BUS-002", "R", "Must", "一期", ""),
    ("INT-LIB-001", "INT", "库 API", "核心能力 shall 以 libomnic C API 暴露（omnic_compress/decompress/analyze），CLI 是薄封装；二期 GUI 复用同一 API", "—", "SYS-ARCH-001", "R", "Must", "一期", ""),
    ("TST-FMT-001", "TST", "交叉验证", "交叉验证：omnic 压缩产物→官方 CLI 解压→与源文件逐字节一致", "—", "SYS-FMT-001", "T", "Must", "一期", ""),
    ("TST-FMT-002", "TST", "交叉验证", "反向交叉：官方 CLI 压缩→omnic 解压→逐字节一致", "—", "SYS-FMT-002", "T", "Must", "一期", ""),
    ("TST-CODEC-001", "TST", "回归", "每 codec roundtrip：语料集全样本压缩→自解压→逐字节一致", "—", "SW-CODEC-001", "T", "Must", "一期", ""),
    ("TST-DEC-001", "TST", "fuzz", "16 种格式×截断/位翻转 fuzz：只许报错，不许崩溃", "—", "SYS-DEC-003", "T", "Must", "一期", ""),
    ("TST-POL-001", "TST", "决策", "决策冒烟：每(类型,profile)断言输出落在决策表期望值", "28 单元", "SW-POL-002", "T", "Must", "一期", ""),
    ("TST-PERF-001", "TST", "性能", "性能回归：zstd -6 基准文件吞吐≥裸库 95%（CI 报告，非硬门禁）", "≥95%", "SYS-PERF-001", "T", "Should", "一期", ""),
    ("TST-PORT-001", "TST", "平台", "三平台 CI 矩阵全绿", "—", "SYS-PORT-001", "T", "Must", "一期", ""),
    ("TST-NEG-001", "TST", "异常", "异常路径：目录输入（提示归档二期）、已存在输出、空文件、单字节文件、不可识别 magic", "—", "SYS-DEC-002", "T", "Must", "一期", "假设 A4"),
    ("TST-LIC-001", "TST", "许可", "许可证扫描：CI 脚本核对 vendored 库 license 与 THIRD_PARTY_NOTICES 一致", "—", "SYS-CODEC-003", "R", "Must", "一期", ""),
]

ASSUMPTIONS = [
    ("A1", "已压缩输入默认跳过压缩并警告，--force 才强压", "SYS-CMP-003, SW-DET-003"),
    ("A2", "解压产物命名 = 去掉压缩后缀（原生格式不含原文件名，与 gzip/zstd 惯例一致）", "SW-DEC-001"),
    ("A3", "完整性校验完全依赖原生格式内建校验，不自加封装", "SYS-DEC-005"),
    ("A4", "一期不做多文件/目录归档，目录输入明确报错提示二期", "TST-NEG-001"),
    ("A5", "GPL/LGPL 库（lzo/lzlib/bzip3/lizard）一期不集成", "SYS-CODEC-002"),
    ("A6", "决策延迟预算：特征提取≤50ms（4MB 采样）+ 查表≤1ms", "SYS-CMP-002"),
    ("A7", "适配层吞吐损耗验收线≤5%", "SYS-PERF-001"),
    ("A8", "默认压缩成功后删除源文件（--keep 保留），与 gzip/zstd 默认行为一致", "SW-CMP-004"),
]

RISKS = [
    ("R1", "GPL 库误链导致发布许可证污染", "中", "高", "准入标准+CI 许可证扫描（TST-LIC-001）"),
    ("R2", "无 magic 格式（brotli/lzav/lzham/raw-deflate）误识别", "中", "中", "后缀强提示+试解优先级链；文档明示"),
    ("R3", "vendored 12 库导致构建慢、仓库膨胀", "高", "中", "每 codec CMake 开关；核心 10 族默认；浅 vendor 裁剪"),
    ("R4", "内置决策表对真实数据类型过拟合", "中", "中", "一期表仅作初值；训练管线持续迭代；--explain 透明化"),
    ("R5", "三平台 CI 维护成本", "中", "中", "GitHub Actions 矩阵；MSVC/MinGW 双编译 Windows"),
    ("R6", "上游库安全公告", "低", "高", "SYS-CODEC-004 一个发布周期内移除机制"),
    ("R7", "lzham (C++) 与 C 核心链接/ABI 问题", "低", "低", "adapter 层以 extern \"C\" 隔离"),
    ("R8", "大文件性能不达标（适配层拷贝过多）", "低", "中", "统一块 buffer 复用；TST-PERF-001 回归"),
]

WBS = [
    ("WP1", "仓库脚手架", "CMake、CI 矩阵、目录、LICENSE、NOTICES", "—", 1),
    ("WP2", "codec 抽象层", "ops 接口、注册表、错误映射、块 I/O 框架", "WP1", 2),
    ("WP3", "核心 codec ×10", "zstd/lz4/zlib/zlib-ng/libdeflate/brotli/xz/bzip2/snappy/lzfse", "WP2", 3),
    ("WP4", "探测层", "magic 表生成器、后缀分类表、熵采样", "WP1", 1.5),
    ("WP5", "决策器一期", "内置表、查表引擎、--explain", "WP4", 1.5),
    ("WP6", "CLI + libomnic API", "子命令、退出码、报告输出、原子写", "WP2, WP5", 2),
    ("WP7", "训练管线 MVP", "bench/fit 工具、语料组织、生成 policy_table.gen.c", "WP3, WP4", 3),
    ("WP8", "解压路径", "16 格式接入验证、lzip 容器、自研 LZW", "WP3", 2),
    ("WP9", "可选 codec ×2", "lzham、lzav（含 extern \"C\" 隔离）", "WP2", 1),
    ("WP10", "测试与回归", "交叉/反向/fuzz/性能/异常用例全量", "WP3-8", 3),
    ("WP11", "文档与发布", "README、man、NOTICES、release", "全部", 1),
]

DOD = [
    ("1", "三平台 CI 绿：构建+TST-CODEC/FMT/DEC/POL/NEG 全通过"),
    ("2", "omnic c README.md 输出 README.md.zst（balanced 档期望 zstd），zstd -d 可解，逐字节一致"),
    ("3", "omnic c --profile best_ratio README.md 期望 xz，xz -d 可解"),
    ("4", "omnic d 对 16 种格式官方 CLI 产物全部可解"),
    ("5", "对 .jpg 输入默认跳过并警告；--force 后照常压缩"),
    ("6", "决策延迟≤50ms（1GB 文件，采样路径）"),
    ("7", "omnic analyze demo.json 输出类型/熵/决策及理由"),
]


def style_sheet(ws, title, headers, widths):
    ws.cell(1, 1, title).font = TITLE_FONT
    ws.merge_cells(start_row=1, start_column=1, end_row=1, end_column=len(headers))
    for c, h in enumerate(headers, 1):
        cell = ws.cell(3, c, h)
        cell.font = HDR_FONT
        cell.fill = HDR_FILL
        cell.alignment = Alignment(horizontal="center", vertical="center", wrap_text=True)
        cell.border = THIN
        ws.column_dimensions[get_column_letter(c)].width = widths[c - 1]
    ws.freeze_panes = "A4"


def fill_rows(ws, rows, ncols, start=4):
    for i, row in enumerate(rows):
        r = start + i
        for c in range(ncols):
            cell = ws.cell(r, c + 1, row[c] if c < len(row) else "")
            cell.font = BODY_FONT
            cell.alignment = WRAP if c in (2, 3, ncols - 1) else CENTER
            cell.border = THIN
            if i % 2 == 1:
                cell.fill = ALT_FILL
        ws.row_dimensions[r].height = None


wb = openpyxl.Workbook()

# ---- Sheet 1: 需求矩阵 ----
ws = wb.active
ws.title = "需求矩阵"
hdrs = ["需求编号", "层级", "模块", "需求描述（EARS）", "量化指标", "父需求", "验证方法", "优先级", "阶段", "假设/备注"]
style_sheet(ws, "omnic 需求矩阵 v0.1（2026-10-29）", hdrs, [14, 7, 10, 60, 16, 14, 9, 9, 7, 14])
fill_rows(ws, REQS, len(hdrs))
ws.auto_filter.ref = f"A3:J{3 + len(REQS)}"

# 验证方法图例
legend_r = 3 + len(REQS) + 2
ws.cell(legend_r, 1, "验证方法代码：T=测试 / R=审查 / A=分析 / D=演示；优先级：MoSCoW").font = Font(name=FONT, size=9, italic=True, color="666666")

# ---- Sheet 2: 汇总统计（公式驱动） ----
ws2 = wb.create_sheet("汇总统计")
style_sheet(ws2, "需求汇总统计（公式自动计算）", ["统计维度", "取值", "条数"], [22, 14, 10])
mrows = []
for lv in ["BUS", "SYS", "SW", "INT", "TST"]:
    mrows.append(("层级", lv, f'=COUNTIF(需求矩阵!$B$4:$B${3 + len(REQS)},B{4 + len(mrows)})'))
for pr in ["Must", "Should", "Could"]:
    mrows.append(("优先级", pr, f'=COUNTIF(需求矩阵!$H$4:$H${3 + len(REQS)},B{4 + len(mrows)})'))
for ph in ["一期", "二期"]:
    mrows.append(("阶段", ph, f'=COUNTIF(需求矩阵!$I$4:$I${3 + len(REQS)},B{4 + len(mrows)})'))
mrows.append(("合计", "全部需求", f'=COUNTA(需求矩阵!$A$4:$A${3 + len(REQS)})'))
fill_rows(ws2, mrows, 3)
note = 4 + len(mrows) + 1
ws2.cell(note, 1, "数据源：'需求矩阵'工作表 A4 起的全部条目；公式随矩阵增删自动更新").font = Font(name=FONT, size=9, italic=True, color="666666")

# ---- Sheet 3: 假设清单 ----
ws3 = wb.create_sheet("假设清单")
style_sheet(ws3, "显式假设（待用户确认）", ["编号", "假设内容", "影响需求"], [8, 78, 22])
fill_rows(ws3, ASSUMPTIONS, 3)

# ---- Sheet 4: 风险清单 ----
ws4 = wb.create_sheet("风险清单")
style_sheet(ws4, "风险清单", ["编号", "风险描述", "概率", "影响", "缓解措施"], [8, 40, 8, 8, 40])
fill_rows(ws4, RISKS, 5)

# ---- Sheet 5: WBS ----
ws5 = wb.create_sheet("WBS")
style_sheet(ws5, "一期工作分解结构（合计 21 人日）", ["编号", "工作包", "内容", "依赖", "估算(人日)"], [8, 18, 52, 12, 12])
fill_rows(ws5, WBS, 5)
tot_r = 4 + len(WBS)
c = ws5.cell(tot_r, 5, f"=SUM(E4:E{tot_r - 1})")
c.font = BOLD_FONT
c.alignment = CENTER
c.border = THIN
ws5.cell(tot_r, 4, "合计").font = BOLD_FONT
ws5.cell(tot_r, 4).border = THIN

# ---- Sheet 6: 一期验收标准 ----
ws6 = wb.create_sheet("验收标准")
style_sheet(ws6, "一期验收标准（Definition of Done）", ["#", "验收项"], [6, 90])
fill_rows(ws6, DOD, 2)

out = r"D:\omnic\docs\omnic-requirements-matrix.xlsx"
wb.save(out)
print("saved:", out, "| requirements:", len(REQS))
