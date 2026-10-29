cmake_minimum_required(VERSION 3.16)
project(omnic C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
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
  foreach(_t XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS)
    set(${_t} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory(third_party/xz liblzma EXCLUDE_FROM_ALL)
  # xz 上游 include 为 PRIVATE，补 INTERFACE 供 adapter 使用
  target_include_directories(liblzma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xz/src/liblzma/api)
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
if(OMNIC_LIBS)
  target_link_libraries(omnic_core PUBLIC ${OMNIC_LIBS})
endif()

# ---- CLI ----
add_executable(omnic src/cli/argp.c src/cli/main.c src/cli/report.c)
target_link_libraries(omnic PRIVATE omnic_core)

# ---- tests（SW-INF-004） ----
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE omnic_core)
add_test(NAME core COMMAND test_core)
