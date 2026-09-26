# 单元测试（GoogleTest + CTest）
#
# 现行必测四类（见 docs/tech-plan-v2.0.md §9.3）：
#   1. 生成确定性        —— TerrainGeneration.*
#   2. 边界与共享采样    —— TerrainBoundary.*（相邻地表 tile 无裂缝）
#   3. 标记合并语义      —— V0.2 起（可挖区域表）
#   4. 存档往返与迁移    —— V0.3 起
#
# 另含：固定步长主循环、输入层、第三人称相机、地表材质、笔刷挖堆与脏 tile 粒度、
# Jolt 角色物理（高度场落地 / 上台阶，无窗口无 GPU）。

find_package(GTest CONFIG REQUIRED)

add_executable(voxel_tests
    fixed_step_test.cpp
    input_map_test.cpp
    physics_test.cpp
    render_camera_test.cpp
    terrain_brush_test.cpp
    terrain_material_test.cpp
    terrain_tile_test.cpp
)

# 让测试能定位仓库内已提交的资产（如 assets/config/materials.toml）。
target_compile_definitions(voxel_tests PRIVATE VOXEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(voxel_tests
    PRIVATE voxel_build_options
    PRIVATE voxel_engine
    PRIVATE voxel_world
    PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(voxel_tests)
