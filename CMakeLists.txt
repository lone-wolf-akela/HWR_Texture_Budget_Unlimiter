cmake_minimum_required(VERSION 3.20)
project(se9ccn_texture_budget LANGUAGES CXX)

# 目标游戏是 32 位进程，本 DLL 必须也是 x86。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "se9ccn_texture_budget 必须是 32 位(x86)构建。"
        " 请用 32 位 MSVC 环境（vcvarsall.bat x86）或 -A Win32。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(se9ccn_texture_budget SHARED
    src/se9ccn_texture_budget.cpp
)

target_compile_definitions(se9ccn_texture_budget PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(se9ccn_texture_budget PRIVATE
    /utf-8 /W4 /permissive- /EHsc
)
target_compile_options(se9ccn_texture_budget PRIVATE $<$<CONFIG:Release>:/O2>)

# 静态 CRT（/MT）：不依赖目标机器上的 VC 运行时。
set_target_properties(se9ccn_texture_budget PROPERTIES
    PREFIX ""
    OUTPUT_NAME "se9ccn_texture_budget"
    SUFFIX ".dll"
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

# 输出到 bin/，方便直接复制到游戏目录。
set_target_properties(se9ccn_texture_budget PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)
