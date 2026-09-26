# 可执行游戏目标
#
# 当前为 V0.1 地表世界的运行闭环：窗口 + 高度场 tile 生成 / 网格化 + Jolt 角色胶囊物理 +
# 第三人称相机 + 固定时间步长主循环 + 笔刷挖堆 + ImGui 调试面板。
# PoC 冒烟路径（TriangleRenderer）保留但不再使用。
#
# Shader 在构建期编译为 SPIR-V 与 DXIL **双格式**（见 ADR 0002 与 cmake/Shaders.cmake）：
#   triangle.* —— 冒烟路径（TriangleRenderer）用；
#   mesh.*     —— 真正的网格渲染路径（MeshRenderer）用；
#   shadow.vert/.frag —— 阴影深度通道（T21b / ADR 0010 P1）用（纯深度写入，片元为空入口）；
#   tonemap.*  —— HDR 色调映射通道（T20 / ADR 0010 P0）用。
#   （T27 的光球自发光走 `mesh.frag` 的片元 uniform 槽 3，不新增着色器文件。）

# ImGui：vcpkg 端口已把 SDL3 / SDLGPU3 后端的 .cpp 编进 imgui 库本体，
# 因此这里只需链接 imgui::imgui（头文件 imgui_impl_sdl3.h / imgui_impl_sdlgpu3.h 由端口提供），
# 无需再把后端源文件加入本目标（详见交付说明）。
find_package(imgui CONFIG REQUIRED)

add_executable(voxel_game
    main.cpp
    debug_overlay.cpp
    system_panel.cpp
    ui_font.cpp
    ui_theme.cpp
)

# 让运行期能定位仓库内已提交的资产（材质表等），与 tests/CMakeLists.txt 口径一致。
target_compile_definitions(voxel_game PRIVATE VOXEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(voxel_game
    PRIVATE voxel_engine
    PRIVATE voxel_world
    PRIVATE imgui::imgui
)

# Shader 产物输出到 <build>/assets/shaders/
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.vert)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.frag)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/mesh.frag)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.vert)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.frag)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/tonemap.vert)
add_shader(voxel_game ${CMAKE_SOURCE_DIR}/assets/shaders/tonemap.frag)
