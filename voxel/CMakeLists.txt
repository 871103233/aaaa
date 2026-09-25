# 体素世界层（本项目核心）
#
# 当前仅有类型与常量定义（header-only）；V0.2 起加入 Chunk/Section 实现，
# 届时把 .cpp 加到这里并从 INTERFACE 改为 STATIC。
add_library(voxel_world INTERFACE)

target_include_directories(voxel_world INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(voxel_world
    INTERFACE voxel_build_options
)
