# c-drive-cleaner-c — 顶层构建
#
#   本机 Linux 原生：  cmake --preset native   && cmake --build --preset native
#   Windows 交叉：     cmake --preset mingw64  && cmake --build --preset mingw64
#   用户机器 MSVC：    cmake -S . -B out/build/msvc -G "Visual Studio 17 2022" -A x64
#                      cmake --build out/build/msvc --config Release
#
# 约定（docs/ARCHITECTURE.md §4）：C11，-Wall -Wextra -Werror（MSVC：/W4 /WX）。
# -Werror 只加给自家目标；vendored 第三方库（sqlite3 / blake3 / cjson）
# 只开 -Wall -Wextra，不开 -Werror，避免上游源码的警告拖垮构建。
cmake_minimum_required(VERSION 3.16)
project(c-drive-cleaner-c C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(CheckCCompilerFlag)
include(CTest)

find_package(Threads REQUIRED)

# 自家目标统一加 -Werror（MSVC：/WX）。
function(cdc_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /WX)
  else()
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# ---------------- 基础层：util ----------------
add_library(cdc_util STATIC src/util.c)
target_include_directories(cdc_util PUBLIC src)
# util.c 的日志互斥量用 pthread（POSIX）/ SRWLOCK（Windows）。
target_link_libraries(cdc_util PRIVATE Threads::Threads)
cdc_strict(cdc_util)

# ---------------- 试点 CLI ----------------
# 入口 + 索引 + 真哈希管线（与 Makefile 的 SRCS / MFT_NATIVE_OBJ 对齐，
# 2026-11-17 补：fileindex / indextask / hashcache / scan / fastio / mft；
# 临时桩 hashcache_scan_stub.c 已删，不再列入）。
add_executable(cdc-c-pilot
  src/main.c
  src/mft_util.c
  src/bigfile.c
  src/mft_raw.c
  src/fileindex.c
  src/indextask.c
  src/hashcache.c
  src/scan.c
  src/fastio.c
  src/mft.c
)
# hashcache.c 用 cJSON 做 v2 JSON 缓存迁移；scan.c 用 BLAKE3；fileindex /
# hashcache 用 SQLite。PUBLIC include 目录随链接自动带入。
target_link_libraries(cdc-c-pilot PRIVATE cdc_util sqlite3 blake3 cjson)
if(WIN32)
  # indextask.c（计划任务 SID）与 mft.c（卷操作）要这两个系统库。
  target_link_libraries(cdc-c-pilot PRIVATE advapi32 shell32)
endif()
# mft.c 在非 Windows 下要 POSIX 声明（lstat 等），与 Makefile 一致。
if(NOT WIN32)
  set_source_files_properties(src/mft.c PROPERTIES
    COMPILE_DEFINITIONS "_POSIX_C_SOURCE=200809L;_DEFAULT_SOURCE=1")
endif()
cdc_strict(cdc-c-pilot)

# ---------------- util 自测 ----------------
add_executable(test_util tests/test_util.c)
target_link_libraries(test_util PRIVATE cdc_util)
cdc_strict(test_util)
add_test(NAME test_util COMMAND test_util)

# ---------------- 第三方库（vendored，见 third_party/README.md） ----------------

# SQLite amalgamation：公有领域，只编 sqlite3.c。
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)

# cJSON：MIT，只编 cJSON.c（cJSON_Utils 未取用）。
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# BLAKE3 官方 C 实现：内联函数（intrinsics）版，不用汇编版。
# 分档逻辑照抄上游 c/CMakeLists.txt（x86-intrinsics / neon-intrinsics / none）。
add_library(blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c
)
target_include_directories(blake3 PUBLIC third_party/blake3)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86|[Xx]86)$")
  if(MSVC)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(BLAKE3_FLAG_SSE2 "") # x64 下 SSE2 是基线，无需开关
      set(BLAKE3_FLAG_SSE41 "/arch:AVX") # MSVC 没有单独的 SSE4.1 开关
    else()
      set(BLAKE3_FLAG_SSE2 "/arch:SSE2")
      set(BLAKE3_FLAG_SSE41 "/arch:AVX")
    endif()
    set(BLAKE3_FLAG_AVX2 "/arch:AVX2")
    set(BLAKE3_FLAG_AVX512 "/arch:AVX512")
  else()
    set(BLAKE3_FLAG_SSE2 "-msse2")
    set(BLAKE3_FLAG_SSE41 "-msse4.1")
    set(BLAKE3_FLAG_AVX2 "-mavx2")
    set(BLAKE3_FLAG_AVX512 "-mavx512f -mavx512vl")
  endif()

  # AVX-512：编译器不支持时退回 BLAKE3_NO_AVX512（dispatch 自动降级）。
  if(NOT BLAKE3_FLAG_AVX512 STREQUAL "")
    check_c_compiler_flag("${BLAKE3_FLAG_AVX512}" CDC_HAVE_AVX512)
  endif()
  if(CDC_HAVE_AVX512)
    target_sources(blake3 PRIVATE third_party/blake3/blake3_avx512.c)
    # 注意：必须用 COMPILE_FLAGS（字符串属性）而非 COMPILE_OPTIONS（列表
    # 属性），后者会把 "-mavx512f -mavx512vl" 当成单个参数传给编译器而报错。
    set_source_files_properties(third_party/blake3/blake3_avx512.c
      PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_AVX512}")
  else()
    target_compile_definitions(blake3 PRIVATE BLAKE3_NO_AVX512)
  endif()

  target_sources(blake3 PRIVATE
    third_party/blake3/blake3_sse2.c
    third_party/blake3/blake3_sse41.c
    third_party/blake3/blake3_avx2.c
  )
  if(NOT BLAKE3_FLAG_SSE2 STREQUAL "")
    set_source_files_properties(third_party/blake3/blake3_sse2.c
      PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_SSE2}")
  endif()
  set_source_files_properties(third_party/blake3/blake3_sse41.c
    PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_SSE41}")
  set_source_files_properties(third_party/blake3/blake3_avx2.c
    PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_AVX2}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
  target_sources(blake3 PRIVATE third_party/blake3/blake3_neon.c)
  target_compile_definitions(blake3 PRIVATE BLAKE3_USE_NEON=1)
else()
  # 其他架构：纯 portable 实现。
  target_compile_definitions(blake3 PRIVATE
    BLAKE3_USE_NEON=0
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
  )
endif()

# ---------------- WebView2 宿主（仅 Windows，切片 5） ----------------
# host/ 由切片 5 提供：原生 COM 的 WebView2 宿主 + 消息桥（cdc_host 静态库，
# bridge.cpp + host.cpp），顶层只负责接入，不改 host/ 里的任何文件。
#
# CDC_WEBVIEW2_INCLUDE_DIR：WebView2 SDK 的 include 目录
# （Microsoft.Web.WebView2 NuGet 包的 build/native/include，含 WebView2.h），
# 由用户以 -DCDC_WEBVIEW2_INCLUDE_DIR=... 传入；cache 变量在子目录可见，
# 所以这里不用显式传参。未传入时 host/CMakeLists.txt 只报 WARNING，
# 编译会在缺 WebView2.h 时失败（报错里写明去 NuGet 取包）。
# 注意：不设空默认值——空字符串会让 host/ 的 if(DEFINED ...) 走错分支；
# 若用户传了空值，在这里 unset 回未定义状态。
if(DEFINED CDC_WEBVIEW2_INCLUDE_DIR AND CDC_WEBVIEW2_INCLUDE_DIR STREQUAL "")
  unset(CDC_WEBVIEW2_INCLUDE_DIR CACHE)
endif()
# CDC_WEBVIEW2_LOADER_LIB（可选，MSVC 静态链接 WebView2Loader.lib 用）
# 同样由 -D 传入，host/CMakeLists.txt 自行读取。

option(CDC_BUILD_HOST "Build the WebView2 host library (Windows only)" ON)
if(WIN32 AND CDC_BUILD_HOST)
  add_subdirectory(host)
else()
  message(STATUS "host: skipped (WebView2 host is Windows-only)")
endif()
