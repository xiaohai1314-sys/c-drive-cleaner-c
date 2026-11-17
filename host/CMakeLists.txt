# host/CMakeLists.txt — WebView2 宿主（切片 5）。
#
# 顶层 CMake（切片 1）用 add_subdirectory(host) 接入，并提供：
#   CDC_WEBVIEW2_INCLUDE_DIR — WebView2 SDK 的 include 目录
#     （Microsoft.Web.WebView2 NuGet 包里的 build/native/include，
#      含 WebView2.h；本机交叉编译时也可用该目录）。
#   CDC_WEBVIEW2_LOADER_LIB  — 可选，WebView2Loader.lib（MSVC 静态链接用；
#     MinGW 下不需要，桥用的是 SDK 自带的动态加载）。
# 若两者都没给，这里只报 WARNING，编译会在缺 WebView2.h 时失败——
# 报错信息里会写明去 NuGet 取包。
#
# 注意：host 源码含中文宽字符串字面量（窗口标题、路径），MSVC 构建
# 必须开 /utf-8；MinGW 默认按 UTF-8 读源码，不用额外处理。

cmake_minimum_required(VERSION 3.16)

add_library(cdc_host STATIC
  bridge.cpp
  host.cpp
)

target_include_directories(cdc_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED CDC_WEBVIEW2_INCLUDE_DIR)
  target_include_directories(cdc_host PRIVATE ${CDC_WEBVIEW2_INCLUDE_DIR})
else()
  message(WARNING "CDC_WEBVIEW2_INCLUDE_DIR not set: WebView2.h must be findable (Microsoft.Web.WebView2 NuGet package, build/native/include)")
endif()

target_compile_features(cdc_host PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(cdc_host PRIVATE /utf-8 /W4)
else()
  target_compile_options(cdc_host PRIVATE -Wall -Wextra)
endif()

# WebView2 运行时（WebView2Loader.dll）由用户机器的 Edge WebView2
# Runtime 提供，不随程序分发；链接期只需要 ole32 / user32 / shlwapi。
target_link_libraries(cdc_host PUBLIC ole32 user32 shlwapi)

if(DEFINED CDC_WEBVIEW2_LOADER_LIB)
  target_link_libraries(cdc_host PRIVATE ${CDC_WEBVIEW2_LOADER_LIB})
endif()
