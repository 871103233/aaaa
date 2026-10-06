# 世界层（本项目核心）：分层混合世界的实现，取代旧的方块体素世界层。
#
# 目录职责（见 docs/tech-plan-v2.0.md §9.1）：
#   terrain/    地表高度场 tile：数据布局、生成、网格化、法线、材质 splat 混合、物理碰撞体提供者
#   generation/ 多层噪声、子系统种子派生、分块确定性
#   dig/        笔刷挖掘 / 堆建与局部重网格调度、可挖区域标记与可挖体积（SDF + Surface Nets）
#
# 依赖方向：只依赖 engine（platform → engine → world → game），不直接调用平台 API。
# 第三方：FastNoiseLite（vendored，生成用，PRIVATE）；toml++（配置表，PRIVATE）。

find_package(tomlplusplus CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_library(voxel_world STATIC
    dig/collapse_table.cpp
    dig/destruction_table.cpp
    dig/dig_region.cpp
    dig/dig_volume.cpp
    dig/projectile_table.cpp
    dig/terrain_brush.cpp
    dig/volume_collapse.cpp
    dig/volume_collision.cpp
    dig/volume_mesher.cpp
    generation/level_manifest.cpp
    generation/map_preset.cpp
    generation/terrain_noise.cpp
    generation/terrain_params.cpp
    object/object_layer.cpp
    premade/premade_bake.cpp
    premade/premade_map.cpp
    premade/premade_terrain_source.cpp
    save/world_instance_save.cpp
    shell/surface_shell.cpp
    shell/surface_shell_residency.cpp
    streaming/dig_volume_residency.cpp
    streaming/shell_block_build_pipeline.cpp
    streaming/terrain_tile_build_pipeline.cpp
    streaming/terrain_tile_residency.cpp
    streaming/volume_build_pipeline.cpp
    terrain/material_blender.cpp
    terrain/material_table.cpp
    terrain/material_textures.cpp
    terrain/terrain_collision.cpp
    terrain/terrain_mesher.cpp
    terrain/terrain_tile.cpp
    terrain/terrain_world.cpp
    terrain/world_bounds.cpp
    water/river.cpp
)

target_include_directories(voxel_world
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
)

# zstd 只在 premade_map.cpp 内出现（公共头不泄漏）⇒ PRIVATE（vcpkg 提供的目标名 = `zstd::libzstd`）。
target_link_libraries(voxel_world
    PUBLIC  voxel_build_options
    PUBLIC  voxel_engine              # 公共头暴露 MeshVertex / ITerrainQuery，故为 PUBLIC
    PRIVATE tomlplusplus::tomlplusplus
    PRIVATE zstd::libzstd
    PRIVATE EnTT::EnTT
)
