# 世界层（本项目核心）：分层混合世界的实现，取代旧的方块体素世界层。
#
# 目录职责（见 docs/tech-plan-v2.0.md §9.1）：
#   terrain/    地表高度场 tile：数据布局、生成、网格化、法线、材质 splat 混合、物理碰撞体提供者
#   generation/ 多层噪声、子系统种子派生、分块确定性
#   dig/        笔刷挖掘 / 堆建与局部重网格调度（可挖体积块于 T8 加入）
#
# 依赖方向：只依赖 engine（platform → engine → world → game），不直接调用平台 API。
# 第三方：FastNoiseLite（vendored，生成用，PRIVATE）；toml++（配置表，PRIVATE）。

find_package(tomlplusplus CONFIG REQUIRED)

add_library(voxel_world STATIC
    dig/terrain_brush.cpp
    generation/terrain_noise.cpp
    terrain/material_blender.cpp
    terrain/material_table.cpp
    terrain/terrain_collision.cpp
    terrain/terrain_mesher.cpp
    terrain/terrain_tile.cpp
    terrain/terrain_world.cpp
)

target_include_directories(voxel_world
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
)

target_link_libraries(voxel_world
    PUBLIC  voxel_build_options
    PUBLIC  voxel_engine              # 公共头暴露 MeshVertex / ITerrainQuery，故为 PUBLIC
    PRIVATE tomlplusplus::tomlplusplus
)
