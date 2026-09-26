cmake_minimum_required(VERSION 3.21)

project(voxel_engine
    VERSION 0.1.0
    DESCRIPTION "自研体素引擎与类 MC 游戏"
    LANGUAGES CXX
)

# ============================================================
# 选项
# ============================================================
option(VOXEL_WARNINGS_AS_ERRORS "把编译警告当作错误" ON)
option(VOXEL_ENABLE_ASAN        "启用 ASAN + UBSAN（与 TSan 互斥）" OFF)
option(VOXEL_ENABLE_TSAN        "启用 TSan（仅 Linux/Clang 可用）" OFF)

if(VOXEL_ENABLE_ASAN AND VOXEL_ENABLE_TSAN)
    message(FATAL_ERROR "ASAN 与 TSan 互斥，不能同时开启（见 docs/tech-plan-v1.3.md §3.4）")
endif()

# ============================================================
# 语言标准与全局设置
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # clang-tidy 与 IDE 需要

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# 统一编译选项（所有目标都必须链接 voxel_build_options）
# ============================================================
add_library(voxel_build_options INTERFACE)

if(MSVC)
    target_compile_options(voxel_build_options INTERFACE
        /W4            # 高警告等级
        /permissive-   # 严格符合标准
        /utf-8         # 源码与执行字符集统一为 UTF-8（中文注释必需）
        /Zc:__cplusplus
        /EHsc
    )
    if(VOXEL_WARNINGS_AS_ERRORS)
        target_compile_options(voxel_build_options INTERFACE /WX)
    endif()
else()
    target_compile_options(voxel_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow
    )
    if(VOXEL_WARNINGS_AS_ERRORS)
        target_compile_options(voxel_build_options INTERFACE -Werror)
    endif()
endif()

# ---- Sanitizer ----
if(VOXEL_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(voxel_build_options INTERFACE /fsanitize=address)
        target_link_options(voxel_build_options INTERFACE /fsanitize=address)
    else()
        target_compile_options(voxel_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(voxel_build_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

if(VOXEL_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "MSVC 不支持 ThreadSanitizer；TSan 只在 Linux/Clang 配置下启用（见 docs/tech-plan-v1.3.md §3.4）")
    endif()
    target_compile_options(voxel_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(voxel_build_options INTERFACE -fsanitize=thread)
endif()

# ============================================================
# 依赖
# ============================================================
find_package(SDL3 CONFIG REQUIRED)
find_package(glm  CONFIG REQUIRED)

# ============================================================
# Shader 辅助函数
# ============================================================
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Shaders)

# ============================================================
# 模块
# ============================================================
add_subdirectory(engine)
add_subdirectory(world)
add_subdirectory(game)

# ============================================================
# 测试
# ============================================================
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
