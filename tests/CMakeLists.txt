# 单元测试（GoogleTest + CTest）
#
# 必测四类（见 docs/tech-plan-v1.3.md §9.3）：
#   1. 生成确定性        —— V0.2 起
#   2. Section/Palette 编解码往返 —— V0.2 起
#   3. 存档序列化往返 + 版本迁移  —— V0.3 起
#   4. 光照 BFS 跨界 与 贪婪合并边界 —— V0.3 起
# 当前已有：区块状态机与跨区块依赖判据（第 4 类的前置）。

find_package(GTest CONFIG REQUIRED)

add_executable(voxel_tests
    chunk_state_test.cpp
    fixed_step_test.cpp
    input_map_test.cpp
    render_camera_test.cpp
)

target_link_libraries(voxel_tests
    PRIVATE voxel_build_options
    PRIVATE voxel_engine
    PRIVATE voxel_world
    PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(voxel_tests)
