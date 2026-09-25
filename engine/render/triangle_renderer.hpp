#pragma once

#include <SDL3/SDL.h>

#include <filesystem>

namespace vx {

/// 最小渲染路径：加载构建期产出的 Shader 字节码、建立图形管线、绘制一个三角形。
///
/// 存在意义是**验证风险假设**，而不是提供渲染能力：
///   1. SDL3_gpu 能否在本机创建设备并拿到交换链；
///   2. 构建期产出的 **SPIR-V + DXIL 双格式**（见 ADR 0002）能否被 SDL3_gpu 接受。
/// 验证通过后，本类会被真正的 ChunkRenderer 取代。
class TriangleRenderer {
public:
    TriangleRenderer(SDL_GPUDevice* device, SDL_Window* window, std::filesystem::path shader_dir);
    ~TriangleRenderer();

    TriangleRenderer(const TriangleRenderer&) = delete;
    TriangleRenderer& operator=(const TriangleRenderer&) = delete;

    /// 渲染一帧。返回 false 表示本帧拿不到交换链纹理（如窗口最小化），可跳过。
    bool render_frame();

private:
    SDL_GPUDevice*           m_device = nullptr;
    SDL_Window*              m_window = nullptr;
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    std::filesystem::path    m_shader_dir;
};

}  // namespace vx
