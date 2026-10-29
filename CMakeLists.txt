cmake_minimum_required(VERSION 3.16)
project(omnic C)

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
  # 关闭 xz 上游自带的测试注册（include(CTest) 默认 BUILD_TESTING=ON），
  # 不影响我们自己的 add_test
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  foreach(_t XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS)
    set(${_t} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory(third_party/xz liblzma EXCLUDE_FROM_ALL)
endif()

# ---- libomnic（INT-LIB-001） ----
set(OMNIC_SRC
  src/core/api.c
  src/core/blockio.c
  src/core/dispatcher.c
  src/core/registry.c
  src/core/util.c
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
if(OMNIC_LIBS)
  target_link_libraries(omnic_core PUBLIC ${OMNIC_LIBS})
endif()
# log2 等数学函数在 Linux 上位于独立的 libm；Windows (CRT) 与 macOS (libSystem) 自动链接
target_link_libraries(omnic_core PUBLIC $<$<PLATFORM_ID:Linux>:m>)

# ---- CLI ----
add_executable(omnic src/cli/argp.c src/cli/main.c src/cli/report.c)
target_link_libraries(omnic PRIVATE omnic_core)

# ---- tests（SW-INF-004） ----
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE omnic_core)
add_test(NAME core COMMAND test_core)

# omnic 自身代码按严格 C11 编译（C_EXTENSIONS OFF）；第三方库走编译器默认（gnu 模式）
foreach(_t omnic omnic_core test_core)
  set_target_properties(${_t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()
