# 引擎核心层：平台抽象 + 渲染封装
#
# 注意：engine/ 内不得出现体素或游戏专有类型（Chunk / BlockId / Biome 等），
# 见技能规范 §2「分层与依赖方向」。

add_library(voxel_engine STATIC
    platform/window.cpp
    render/triangle_renderer.cpp
)

target_include_directories(voxel_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(voxel_engine
    PUBLIC  voxel_build_options
    PUBLIC  SDL3::SDL3          # 公共头文件暴露 SDL 类型，故为 PUBLIC
    PUBLIC  glm::glm
)
