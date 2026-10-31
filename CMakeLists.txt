cmake_minimum_required(VERSION 3.16)
# snappy 是 C++ 编译的库（提供 C 接口 snappy-c），需要 CXX 参与链接
project(omnic C CXX)

# 注意：不要设置全局 CMAKE_C_STANDARD。目录级变量会泄漏进 third_party 子目录，
# -std=c11 的 __STRICT_ANSI__ 会让 glibc 隐藏 POSIX 函数声明（futimens/fcntl 等），
# xz/zstd 在 Linux 上因此编译失败。omnic 自身的 target 在文件末尾用 target 级属性约束。
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---- codec 开关（SW-CODEC-005，一期核心族默认开） ----
option(OMNIC_CODEC_ZSTD "Enable zstd codec" ON)
option(OMNIC_CODEC_LZ4 "Enable lz4 codec" ON)
option(OMNIC_CODEC_XZ "Enable xz codec" ON)
option(OMNIC_CODEC_ZLIB "Enable zlib codec" ON)
option(OMNIC_CODEC_ZLIB_NG "Enable zlib-ng codec" ON)
option(OMNIC_CODEC_LIBDEFLATE "Enable libdeflate codec" ON)
option(OMNIC_CODEC_BROTLI "Enable brotli codec" ON)
option(OMNIC_CODEC_SNAPPY "Enable snappy codec" ON)
option(OMNIC_CODEC_LZFSE "Enable lzfse codec" ON)
option(OMNIC_CODEC_BZIP2 "Enable bzip2 decompression" ON)
option(OMNIC_CODEC_COMPRESSZ "Enable compress(.Z) decompression" ON)

# 关闭所有第三方子项目的测试注册（zlib-ng/xz 等会 include(CTest)），
# 不影响我们自己的 add_test
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# ---- vendored third-party（SW-INF-002） ----
if(OMNIC_CODEC_ZSTD)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
  set(ZSTD_MULTITHREAD_SUPPORT ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/zstd/build/cmake libzstd EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_LZ4)
  set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/lz4/build/cmake liblz4 EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_XZ)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(XZ_NLS OFF CACHE BOOL "" FORCE)
  # 关闭 xz 上游自带的测试注册（tests.cmake 里 include(CTest) 默认 BUILD_TESTING=ON）
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  foreach(_t XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS)
    set(${_t} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory(third_party/xz liblzma EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_ZLIB)
  # zlib 官方 CMake 用自有选项（不吃 BUILD_SHARED_LIBS），target 名 zlibstatic；
  # 自有测试开关 ZLIB_BUILD_TESTING 默认 ON，其 find_package 测试需要已安装的 zlib
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/zlib zlib_up EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_ZLIB_NG)
  # 非兼容模式：头文件 zlib-ng.h、符号 zng_ 前缀，可与 zlib 静态共存；
  # 关闭 ZLIB_ALIASES 避免它创建的 zlib/zlibstatic ALIAS 与真 zlib target 撞名
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
  set(ZLIB_ALIASES OFF CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/zlib-ng zlibng_up EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_LIBDEFLATE)
  set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
  set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
  set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/libdeflate libdeflate_up EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_BROTLI)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/brotli brotli_up EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_SNAPPY)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  # 预置检测结果：阻止 snappy 探测并链上 toolchain 携带的动态 libz.dll
  set(HAVE_LIBZ OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/snappy snappy_up EXCLUDE_FROM_ALL)
endif()

if(OMNIC_CODEC_LZFSE)
  # lzfse 无官方 CMake，直接编库源（lzfse_main.c 是 CLI demo，排除）
  add_library(lzfse_static STATIC
    third_party/lzfse-heads-master/src/lzfse_decode.c
    third_party/lzfse-heads-master/src/lzfse_decode_base.c
    third_party/lzfse-heads-master/src/lzfse_encode.c
    third_party/lzfse-heads-master/src/lzfse_encode_base.c
    third_party/lzfse-heads-master/src/lzfse_fse.c
    third_party/lzfse-heads-master/src/lzvn_decode_base.c
    third_party/lzfse-heads-master/src/lzvn_encode_base.c)
  target_include_directories(lzfse_static PUBLIC third_party/lzfse-heads-master/src)
endif()

if(OMNIC_CODEC_BZIP2)
  # bzip2 1.0.8 仅集成解码半边（bzlib_dec.c 为上游 bzlib.c 的裁剪，见文件头）
  add_library(bzip2dec STATIC
    third_party/bzip2/bzlib_dec.c
    third_party/bzip2/decompress.c
    third_party/bzip2/huffman.c
    third_party/bzip2/crctable.c
    third_party/bzip2/randtable.c)
  target_include_directories(bzip2dec PUBLIC third_party/bzip2)
endif()

# ---- libomnic（INT-LIB-001） ----
set(OMNIC_SRC
  src/core/api.c
  src/core/blockio.c
  src/core/dispatcher.c
  src/core/registry.c
  src/core/util.c
  src/codec/codec_util.c
  src/detect/entropy.c
  src/detect/filetype.c
  src/detect/magic.c
  src/policy/policy.c
  src/policy/policy_table.gen.c
)
set(OMNIC_DEFS "")
set(OMNIC_LIBS "")
if(OMNIC_CODEC_ZSTD)
  list(APPEND OMNIC_SRC src/codec/ac_zstd.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_ZSTD)
  list(APPEND OMNIC_LIBS libzstd_static)
endif()
if(OMNIC_CODEC_LZ4)
  list(APPEND OMNIC_SRC src/codec/ac_lz4.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_LZ4)
  list(APPEND OMNIC_LIBS lz4_static)
endif()
if(OMNIC_CODEC_XZ)
  list(APPEND OMNIC_SRC src/codec/ac_xz.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_XZ)
  list(APPEND OMNIC_LIBS liblzma)
endif()
if(OMNIC_CODEC_ZLIB)
  list(APPEND OMNIC_SRC src/codec/ac_zlib.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_ZLIB)
  list(APPEND OMNIC_LIBS zlibstatic)
endif()
if(OMNIC_CODEC_ZLIB_NG)
  list(APPEND OMNIC_SRC src/codec/ac_zlibng.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_ZLIB_NG)
  list(APPEND OMNIC_LIBS zlib-ng)
endif()
if(OMNIC_CODEC_LIBDEFLATE)
  list(APPEND OMNIC_SRC src/codec/ac_libdeflate.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_LIBDEFLATE)
  list(APPEND OMNIC_LIBS libdeflate_static)
endif()
if(OMNIC_CODEC_BROTLI)
  list(APPEND OMNIC_SRC src/codec/ac_brotli.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_BROTLI)
  # BUILD_SHARED_LIBS=OFF 时上游直接产出静态库（-static 后缀 target 仅 package 模式存在）
  list(APPEND OMNIC_LIBS brotlienc brotlidec brotlicommon)
endif()
if(OMNIC_CODEC_SNAPPY)
  list(APPEND OMNIC_SRC src/codec/ac_snappy.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_SNAPPY)
  list(APPEND OMNIC_LIBS snappy)
  # CRC32C 硬件路径（SSE4.2）：MSVC x64 恒可用，GCC/Clang 需 per-file 开启；
  # 仅 x86 目标加 flag（arm64 走 __ARM_FEATURE_CRC32 或表驱动回退）
  if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    set_source_files_properties(src/codec/ac_snappy.c PROPERTIES COMPILE_OPTIONS "-msse4.2")
  endif()
endif()
if(OMNIC_CODEC_LZFSE)
  list(APPEND OMNIC_SRC src/codec/ac_lzfse.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_LZFSE)
  list(APPEND OMNIC_LIBS lzfse_static)
endif()
if(OMNIC_CODEC_BZIP2)
  list(APPEND OMNIC_SRC src/codec/ac_bzip2.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_BZIP2)
  list(APPEND OMNIC_LIBS bzip2dec)
endif()
if(OMNIC_CODEC_COMPRESSZ)
  # .Z 解码器自研（语义对齐 ncompress 5.0），无第三方库依赖
  list(APPEND OMNIC_SRC src/codec/ac_compressz.c)
  list(APPEND OMNIC_DEFS OMNIC_CODEC_COMPRESSZ)
endif()

add_library(omnic_core STATIC ${OMNIC_SRC})
target_include_directories(omnic_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)
if(OMNIC_DEFS)
  target_compile_definitions(omnic_core PRIVATE ${OMNIC_DEFS})
endif()
if(OMNIC_CODEC_XZ)
  # xz 上游的 include 是 PRIVATE；adapter 编译需要显式补头文件路径
  target_include_directories(omnic_core PUBLIC third_party/xz/src/liblzma/api)
endif()
if(OMNIC_CODEC_BROTLI)
  # brotli 上游用 APPEND PROPERTY 挂 INTERFACE include，传播不可靠，显式补
  target_include_directories(omnic_core PUBLIC third_party/brotli/c/include)
endif()
if(OMNIC_LIBS)
  target_link_libraries(omnic_core PUBLIC ${OMNIC_LIBS})
endif()
# log2 等数学函数在 Linux 上位于独立的 libm；Windows (CRT) 与 macOS (libSystem) 自动链接
target_link_libraries(omnic_core PUBLIC $<$<PLATFORM_ID:Linux>:m>)

# ---- CLI ----
add_executable(omnic src/cli/argp.c src/cli/main.c src/cli/report.c)
target_link_libraries(omnic PRIVATE omnic_core)

# ---- bench（A7 / TST-PERF-001：适配层损耗基准 + README 数据源） ----
add_executable(omnic_bench tools/bench.c tools/bench_zng.c)
target_link_libraries(omnic_bench PRIVATE omnic_core)
target_compile_definitions(omnic_bench PRIVATE ${OMNIC_DEFS})
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
  target_compile_options(omnic_bench PRIVATE -msse4.2) # 基线 CRC32C 与适配层同硬件路径
endif()

# ---- tests（SW-INF-004） ----
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE omnic_core)
add_test(NAME core COMMAND test_core)

# TST-PERF-001 冒烟：16MB×3 轮中位 + 量级线（40%）。小语料下 reader/writer
# 初始化等固定成本占比过高（mac 4MB 实测 zstd 低档虚高 ~18%），16MB 稀释到
# 个位数；40% 线容纳 CI 共享 runner 抖动，专抓量级回归（如 read_all 翻倍类），
# 紧验收线由本地正式跑数把关
add_test(NAME bench_smoke COMMAND omnic_bench --size 16 --rounds 3 --check 40)

# TST-DEC-001 fuzz 冒烟：损坏输入（截断/位翻转/边界）只报错、不崩溃不挂死；
# raw-deflate 无校验和，静默错解为格式固有（python zlib 同），脚本内豁免
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND OMNIC_CODEC_LZFSE)
  add_executable(fuzz_lzvn_gen tools/fuzz_lzvn_gen.c)
  target_link_libraries(fuzz_lzvn_gen PRIVATE lzfse_static)
  add_test(NAME fuzz_smoke
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fuzz_smoke.py --quick
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# MinGW：全静态链接 runtime（libgcc/libstdc++/winpthread，snappy 引入 C++ 与
# zstd MT 引入 pthread），保证单二进制分发（BUS-005）
if(WIN32 AND NOT MSVC)
  foreach(_t omnic omnic_bench test_core)
    target_link_options(${_t} PRIVATE -static)
  endforeach()
endif()

# omnic 自身代码按严格 C11 编译（C_EXTENSIONS OFF）；第三方库走编译器默认（gnu 模式）
foreach(_t omnic omnic_bench omnic_core test_core)
  set_target_properties(${_t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()
