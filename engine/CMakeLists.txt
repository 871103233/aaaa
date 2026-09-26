# 引擎核心层：平台抽象 + 核心设施（时钟 / 固定步长 / 日志）+ 输入抽象 + 渲染封装 + 物理封装
#
# 注意：engine/ 内不得出现体素或游戏专有类型（Chunk / BlockId / Biome 等），
# 见技能规范 §2「分层与依赖方向」。物理封装同理：只提供通用碰撞体与角色控制接口，
# Jolt 类型全部隐藏在 engine/physics/physics_world.cpp 内（pimpl）。

find_package(Jolt CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

add_library(voxel_engine STATIC
    core/clock.cpp
    core/fixed_step.cpp
    core/frame_limiter.cpp
    core/log.cpp
    input/input_map.cpp
    physics/physics_world.cpp
    platform/settings.cpp
    platform/window.cpp
    render/camera.cpp
    render/lighting_table.cpp
    render/mesh_renderer.cpp
    render/shadow_cascade.cpp
    render/triangle_renderer.cpp
)

target_include_directories(voxel_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(voxel_engine
    PUBLIC  voxel_build_options
    PUBLIC  SDL3::SDL3              # 公共头文件暴露 SDL 类型，故为 PUBLIC
    PUBLIC  glm::glm
    PRIVATE Jolt::Jolt              # 只在 physics_world.cpp 内出现 Jolt 头，故为 PRIVATE
    PRIVATE tomlplusplus::tomlplusplus  # 只在 settings.cpp 内出现 toml++ 头，故为 PRIVATE
)
