# 单元测试（GoogleTest + CTest）
#
# 现行必测四类（见 docs/tech-plan-v2.0.md §9.3）：
#   1. 生成确定性        —— TerrainGeneration.*
#   2. 边界与共享采样    —— TerrainBoundary.*（相邻地表 tile 无裂缝）
#   3. 标记合并语义      —— V0.2 起（可挖区域表）
#   4. 存档往返与迁移    —— V0.3 起
#
# 另含：固定步长主循环、输入层、第三人称相机、地表材质、笔刷挖堆与脏 tile 粒度、
# Jolt 角色物理（高度场落地 / 上台阶，无窗口无 GPU）；
# 以及缺陷 B2/B3 回归：笔刷长按下的几何不变量、堆土时角色被顶回地表、相机退化防护、
# 跳跃初速由身高派生、空格按下在零逻辑步帧的帧级锁存。

find_package(GTest CONFIG REQUIRED)

add_executable(voxel_tests
    character_jump_test.cpp
    character_test.cpp
    fixed_step_test.cpp
    frame_limiter_test.cpp
    gameplay_input_test.cpp
    input_map_test.cpp
    map_preset_test.cpp
    mouse_capture_test.cpp
    system_settings_test.cpp
    physics_test.cpp
    render_camera_test.cpp
    render_stats_test.cpp
    terrain_brush_hold_test.cpp
    terrain_brush_test.cpp
    terrain_material_test.cpp
    terrain_material_texture_test.cpp
    terrain_tile_test.cpp
    ui_font_test.cpp
    ui_text_test.cpp
    world_bounds_test.cpp
)

# 让测试能定位仓库内已提交的资产（如 assets/config/materials.toml）。
target_compile_definitions(voxel_tests PRIVATE VOXEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# game/ 的头文件（移动纯函数、主角可视网格）也纳入测试，用于锁定其语义与尺寸。
target_include_directories(voxel_tests PRIVATE ${CMAKE_SOURCE_DIR}/game)

target_link_libraries(voxel_tests
    PRIVATE voxel_build_options
    PRIVATE voxel_engine
    PRIVATE voxel_world
    PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(voxel_tests)
