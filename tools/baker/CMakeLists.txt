# 离线烘焙 CLI（阶段 W / W2-S2b）：读地图预设 TOML → 调 world/premade 的烘焙库 → 写预制地图文件。
#
# 依赖方向：tools → world → engine，符合分层；本目标**不进游戏产物**，只供离线生成预制数据。
add_executable(voxel_bake main.cpp)

target_link_libraries(voxel_bake
    PRIVATE voxel_build_options
    PRIVATE voxel_world   # PUBLIC 已带 voxel_engine，故 engine 层（日志 / 命令行）也可用
)
