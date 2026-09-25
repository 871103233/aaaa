#include "platform/window.hpp"
#include "render/triangle_renderer.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>

namespace {

/// 定位构建期产出的 Shader 目录。
/// 可执行文件在 <build>/bin/，Shader 在 <build>/assets/shaders/，因此需要向上找。
[[nodiscard]] std::filesystem::path resolve_shader_dir(const char* argv0) {
    const std::filesystem::path exe_dir = std::filesystem::path(argv0).parent_path();

    const std::filesystem::path candidates[] = {
        exe_dir / "assets" / "shaders",
        exe_dir.parent_path() / "assets" / "shaders",
        exe_dir.parent_path().parent_path() / "assets" / "shaders",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "triangle.vert.spv")) {
            return candidate;
        }
    }
    return candidates[1];  // 回退到最可能的位置，让报错信息更有指向性
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path shader_dir = resolve_shader_dir(argv[0]);
    if (argc > 1) {
        shader_dir = argv[1];
    }

    try {
        vx::Window window("Voxel Engine - SDL3_gpu smoke test", 1280, 720);
        vx::TriangleRenderer renderer(window.device(), window.handle(), shader_dir);

        std::cout << "[ok] SDL3_gpu 设备已创建，管线已就绪；渲染循环开始。\n"
                  << "     渲染后端实际由 SDL3_gpu 选择（Windows 上优先 Vulkan）。\n";

        while (window.pump_events()) {
            renderer.render_frame();
        }
    }
    catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
