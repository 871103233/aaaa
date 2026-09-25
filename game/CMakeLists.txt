# 可执行游戏目标
#
# 当前为 SDL3_gpu 冒烟测试（hello triangle），用于验证两个高风险假设：
#   1. SDL3_gpu 能在本机创建设备并拿到交换链；
#   2. 离线编译的 SPIR-V 能被 SDL3_gpu 接受。
# 验证通过后，main.cpp 会被真正的固定时间步长主循环取代。

add_executable(voxel_game main.cpp)

target_link_libraries(voxel_game
    PRIVATE voxel_engine
    PRIVATE voxel_world
)

# Shader 在构建期编译为 SPIR-V，产物位于 <build>/assets/shaders/
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.vert)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.frag)
