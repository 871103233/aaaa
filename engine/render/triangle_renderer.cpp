#include "render/triangle_renderer.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vx {
namespace {

[[nodiscard]] std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("无法打开 Shader 产物：" + path.string() +
                                 "（是否未安装 Shader 工具链导致构建期未编译？）");
    }
    const std::streamsize size = file.tellg();
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// SDL3_gpu 要求的 Shader 格式随后端而变（Vulkan 用 SPIR-V，D3D12 用 DXIL），
/// 因此需要按设备能力选择构建期产出的哪一种，否则会触发
/// "Incompatible shader format for GPU backend"。
struct ShaderArtifact {
    SDL_GPUShaderFormat format;
    const char*         extension;
};

[[nodiscard]] ShaderArtifact select_shader_artifact(SDL_GPUDevice* device) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        return ShaderArtifact { SDL_GPU_SHADERFORMAT_DXIL, ".dxil" };
    }
    if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        return ShaderArtifact { SDL_GPU_SHADERFORMAT_SPIRV, ".spv" };
    }

    throw std::runtime_error("当前 GPU 后端既不支持 DXIL 也不支持 SPIR-V，无法加载 Shader");
}

[[nodiscard]] SDL_GPUShader* create_shader_from_file(SDL_GPUDevice* device,
                                                     const std::filesystem::path& path,
                                                     SDL_GPUShaderStage stage,
                                                     SDL_GPUShaderFormat format) {
    std::vector<std::uint8_t> code = read_binary_file(path);

    SDL_GPUShaderCreateInfo info {};
    info.code_size            = code.size();
    info.code                 = code.data();
    info.entrypoint           = "main";
    info.format               = format;
    info.stage                = stage;
    info.num_samplers         = 0;
    info.num_uniform_buffers  = 0;
    info.num_storage_buffers  = 0;
    info.num_storage_textures = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        throw std::runtime_error("SDL_CreateGPUShader 失败（" + path.string() + "）：" + SDL_GetError());
    }
    return shader;
}

}  // namespace

TriangleRenderer::TriangleRenderer(SDL_GPUDevice* device, SDL_Window* window,
                                   std::filesystem::path shader_dir)
    : m_device(device), m_window(window), m_shader_dir(std::move(shader_dir)) {
    const ShaderArtifact artifact = select_shader_artifact(m_device);
    const std::string    extension = artifact.extension;

    SDL_GPUShader* vert = create_shader_from_file(
        m_device, m_shader_dir / ("triangle.vert" + extension), SDL_GPU_SHADERSTAGE_VERTEX,
        artifact.format);
    SDL_GPUShader* frag = create_shader_from_file(
        m_device, m_shader_dir / ("triangle.frag" + extension), SDL_GPU_SHADERSTAGE_FRAGMENT,
        artifact.format);

    // PoC 不提交顶点缓冲：顶点位置写死在 Vertex Shader 内，
    // 目的是隔离出"设备 + 管线 + 换链"这条链路是否可用。
    SDL_GPUVertexInputState vertex_input {};
    vertex_input.num_vertex_buffers    = 0;
    vertex_input.num_vertex_attributes = 0;
    vertex_input.vertex_buffer_descriptions = nullptr;
    vertex_input.vertex_attributes          = nullptr;

    SDL_GPUColorTargetDescription color_target_desc {};
    color_target_desc.format = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

    SDL_GPUGraphicsPipelineCreateInfo info {};
    info.vertex_shader   = vert;
    info.fragment_shader = frag;
    info.vertex_input_state   = vertex_input;
    info.primitive_type       = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.num_color_targets      = 1;
    info.target_info.color_target_descriptions = &color_target_desc;
    info.target_info.has_depth_stencil_target = false;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);

    // 管线创建后即可释放 Shader 对象
    SDL_ReleaseGPUShader(m_device, vert);
    SDL_ReleaseGPUShader(m_device, frag);

    if (m_pipeline == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateGPUGraphicsPipeline 失败：") + SDL_GetError());
    }
}

TriangleRenderer::~TriangleRenderer() {
    if (m_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    }
}

bool TriangleRenderer::render_frame() {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_device);
    if (cmd == nullptr) {
        throw std::runtime_error(std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError());
    }

    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t   width     = 0;
    std::uint32_t   height    = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, m_window, &swapchain, &width, &height)) {
        SDL_CancelGPUCommandBuffer(cmd);
        throw std::runtime_error(std::string("获取交换链纹理失败：") + SDL_GetError());
    }

    if (swapchain == nullptr) {
        // 窗口最小化等情形：本帧没有可渲染目标
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }

    SDL_GPUColorTargetInfo color_target {};
    color_target.texture     = swapchain;
    color_target.load_op     = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op    = SDL_GPU_STOREOP_STORE;
    color_target.clear_color = SDL_FColor { 0.05F, 0.07F, 0.10F, 1.0F };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);
    SDL_DrawGPUPrimitives(pass, /*num_vertices=*/3, /*num_instances=*/1, /*first_vertex=*/0,
                          /*first_instance=*/0);
    SDL_EndGPURenderPass(pass);

    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

}  // namespace vx
