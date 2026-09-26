#include "render/mesh_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
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

/// Shader 声明的各类资源数量（SDL3_gpu 在创建 Shader 时必须显式给出）。
struct ShaderResourceCounts {
    std::uint32_t samplers        = 0;
    std::uint32_t storageTextures = 0;
    std::uint32_t storageBuffers  = 0;
    std::uint32_t uniformBuffers  = 0;
};

[[nodiscard]] SDL_GPUShader* create_shader_from_file(SDL_GPUDevice* device,
                                                     const std::filesystem::path& path,
                                                     SDL_GPUShaderStage stage,
                                                     SDL_GPUShaderFormat format,
                                                     const ShaderResourceCounts& counts) {
    std::vector<std::uint8_t> code = read_binary_file(path);

    SDL_GPUShaderCreateInfo info {};
    info.code_size            = code.size();
    info.code                 = code.data();
    info.entrypoint           = "main";
    info.format               = format;
    info.stage                = stage;
    info.num_samplers         = counts.samplers;
    info.num_uniform_buffers  = counts.uniformBuffers;
    info.num_storage_buffers  = counts.storageBuffers;
    info.num_storage_textures = counts.storageTextures;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        throw std::runtime_error("SDL_CreateGPUShader 失败（" + path.string() + "）：" + SDL_GetError());
    }
    return shader;
}

/// 同步创建一个 GPU 缓冲并把 `data` 上传进去。
///
/// 只在网格上传这样的**非热路径**调用：内部会向 GPU 提交并把等待收敛到一次 fence。
/// 失败时抛 `std::runtime_error`（启动 / 加载期允许异常）。
[[nodiscard]] SDL_GPUBuffer* create_and_upload_buffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage,
                                                      const void* data, std::uint32_t size) {
    SDL_GPUBufferCreateInfo bufferInfo {};
    bufferInfo.usage = usage;
    bufferInfo.size  = size;

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &bufferInfo);
    if (buffer == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateGPUBuffer 失败：") + SDL_GetError());
    }

    SDL_GPUTransferBufferCreateInfo transferInfo {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size  = size;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (transfer == nullptr) {
        SDL_ReleaseGPUBuffer(device, buffer);
        throw std::runtime_error(std::string("SDL_CreateGPUTransferBuffer 失败：") + SDL_GetError());
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, /*cycle=*/false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        throw std::runtime_error(std::string("SDL_MapGPUTransferBuffer 失败：") + SDL_GetError());
    }
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        throw std::runtime_error(std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError());
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTransferBufferLocation source { transfer, 0 };
    SDL_GPUBufferRegion           destination { buffer, 0, size };
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    SDL_EndGPUCopyPass(copyPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(device, /*wait_all=*/true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }

    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return buffer;
}

}  // namespace

MeshRenderer::MeshRenderer(SDL_GPUDevice* device, SDL_Window* window, std::filesystem::path shader_dir,
                           std::string shader_name)
    : m_device(device), m_window(window) {
    const ShaderArtifact artifact = select_shader_artifact(m_device);
    const std::string    extension = artifact.extension;

    // 顶点着色器：1 个只读 storage buffer（相机常量）。
    SDL_GPUShader* vertexShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".vert" + extension),
                                SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                ShaderResourceCounts { 0, 0, 1, 0 });
    // 片元着色器：2 个采样纹理数组（albedo / 法线）+ 1 个 uniform 块（材质参数）。
    SDL_GPUShader* fragmentShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".frag" + extension),
                                SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                ShaderResourceCounts { /*samplers=*/2, 0, 0, /*uniformBuffers=*/1 });

    // 顶点布局：与 MeshVertex 一一对应（pitch = 单个顶点大小，stride 连续）。
    SDL_GPUVertexBufferDescription vertexBufferDescription {};
    vertexBufferDescription.slot              = 0;
    vertexBufferDescription.pitch             = static_cast<Uint32>(sizeof(MeshVertex));
    vertexBufferDescription.input_rate        = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertexBufferDescription.instance_step_rate = 0;

    const SDL_GPUVertexAttribute attributes[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(MeshVertex, position)) },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(MeshVertex, normal)) },
    };

    SDL_GPUVertexInputState vertexInput {};
    vertexInput.vertex_buffer_descriptions = &vertexBufferDescription;
    vertexInput.num_vertex_buffers         = 1;
    vertexInput.vertex_attributes          = attributes;
    vertexInput.num_vertex_attributes      = static_cast<Uint32>(std::size(attributes));

    SDL_GPUColorTargetDescription colorTargetDescription {};
    colorTargetDescription.format = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

    SDL_GPUGraphicsPipelineCreateInfo info {};
    info.vertex_shader   = vertexShader;
    info.fragment_shader = fragmentShader;
    info.vertex_input_state = vertexInput;
    info.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;

    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    info.depth_stencil_state.compare_op         = SDL_GPU_COMPAREOP_LESS;
    info.depth_stencil_state.enable_depth_test  = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.depth_stencil_state.enable_stencil_test = false;

    info.target_info.color_target_descriptions = &colorTargetDescription;
    info.target_info.num_color_targets         = 1;
    info.target_info.depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target  = true;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);

    // 管线创建后即可释放 Shader 对象
    SDL_ReleaseGPUShader(m_device, vertexShader);
    SDL_ReleaseGPUShader(m_device, fragmentShader);

    if (m_pipeline == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateGPUGraphicsPipeline 失败：") + SDL_GetError());
    }

    // 每帧相机常量：一个能被顶点着色器读取的 GPU 缓冲 + 一个复用的上传缓冲。
    SDL_GPUBufferCreateInfo uniformBufferInfo {};
    uniformBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    uniformBufferInfo.size  = static_cast<Uint32>(sizeof(CameraUniform));
    m_cameraUniformBuffer   = SDL_CreateGPUBuffer(m_device, &uniformBufferInfo);
    if (m_cameraUniformBuffer == nullptr) {
        throw std::runtime_error(std::string("创建相机常量缓冲失败：") + SDL_GetError());
    }

    SDL_GPUTransferBufferCreateInfo transferInfo {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size  = static_cast<Uint32>(sizeof(CameraUniform));
    m_cameraTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_cameraTransferBuffer == nullptr) {
        throw std::runtime_error(std::string("创建相机常量上传缓冲失败：") + SDL_GetError());
    }

    // 纹理数组共用的采样器：repeat 寻址（地表平铺）+ 线性过滤 + mipmap 线性。
    // 引擎不关心纹理内容（世界层决定），只固定"平铺且带 mip"这一通用采样行为。
    SDL_GPUSamplerCreateInfo samplerInfo {};
    samplerInfo.min_filter     = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter     = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.min_lod        = 0.0F;
    samplerInfo.max_lod        = 1000.0F;
    m_layerSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_layerSampler == nullptr) {
        throw std::runtime_error(std::string("创建纹理采样器失败：") + SDL_GetError());
    }
}

MeshRenderer::~MeshRenderer() {
    for (const MeshResources& resources : m_meshes) {
        if (resources.vertexBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(m_device, resources.vertexBuffer);
        }
        if (resources.indexBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(m_device, resources.indexBuffer);
        }
    }
    if (m_depthTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_depthTexture);
    }
    if (m_cameraTransferBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_cameraTransferBuffer);
    }
    if (m_vertexStagingBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_vertexStagingBuffer);
    }
    if (m_cameraUniformBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, m_cameraUniformBuffer);
    }
    for (SDL_GPUTexture* texture : m_textureArrays) {
        if (texture != nullptr) {
            SDL_ReleaseGPUTexture(m_device, texture);
        }
    }
    if (m_layerSampler != nullptr) {
        SDL_ReleaseGPUSampler(m_device, m_layerSampler);
    }
    if (m_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    }
}

MeshHandle MeshRenderer::UploadMesh(const MeshData& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return MeshHandle {};
    }

    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(MeshVertex));
    const std::uint32_t indexBytes  = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));

    MeshResources resources;
    resources.vertexBuffer =
        create_and_upload_buffer(m_device, SDL_GPU_BUFFERUSAGE_VERTEX, mesh.vertices.data(), vertexBytes);
    resources.indexBuffer =
        create_and_upload_buffer(m_device, SDL_GPU_BUFFERUSAGE_INDEX, mesh.indices.data(), indexBytes);
    resources.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    resources.indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    std::uint32_t slot = 0;
    if (!m_freeSlots.empty()) {
        slot                 = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_meshes[slot]       = resources;
    } else {
        slot = static_cast<std::uint32_t>(m_meshes.size());
        m_meshes.push_back(resources);
    }
    return MeshHandle { slot + 1 };
}

bool MeshRenderer::UpdateMeshVertices(MeshHandle handle, const std::vector<MeshVertex>& vertices) {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return false;
    }
    const MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return false;
    }
    // 顶点数必须与上传时一致，否则既有的 GPU 顶点缓冲装不下（本方法只做就地刷新）。
    if (vertices.size() != static_cast<std::size_t>(resources.vertexCount)) {
        return false;
    }

    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(vertices.size() * sizeof(MeshVertex));
    if (m_vertexStagingBuffer == nullptr || m_vertexStagingCapacity < vertexBytes) {
        // 扩容只在首次或网格变大时发生，稳态（定长动态网格）下不触发，故不构成每帧分配。
        if (m_vertexStagingBuffer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(m_device, m_vertexStagingBuffer);
            m_vertexStagingBuffer = nullptr;
        }
        SDL_GPUTransferBufferCreateInfo stagingInfo {};
        stagingInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        stagingInfo.size  = vertexBytes;
        m_vertexStagingBuffer = SDL_CreateGPUTransferBuffer(m_device, &stagingInfo);
        if (m_vertexStagingBuffer == nullptr) {
            m_vertexStagingCapacity = 0;
            return false;
        }
        m_vertexStagingCapacity = vertexBytes;
    }

    // cycle = true：即便该暂存缓冲仍被上一帧的命令缓冲引用，也可安全复用（SDL 内部换名）。
    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_vertexStagingBuffer, /*cycle=*/true);
    if (mapped == nullptr) {
        return false;
    }
    std::memcpy(mapped, vertices.data(), vertexBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexStagingBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        return false;
    }
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTransferBufferLocation source { m_vertexStagingBuffer, 0 };
    SDL_GPUBufferRegion           destination { resources.vertexBuffer, 0, vertexBytes };
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    return true;
}

void MeshRenderer::ReleaseMesh(MeshHandle handle) noexcept {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return;
    }

    const std::uint32_t slot = handle.id - 1;
    MeshResources&      resources = m_meshes[slot];
    if (resources.vertexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, resources.vertexBuffer);
    }
    if (resources.indexBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, resources.indexBuffer);
    }
    resources = MeshResources {};
    m_freeSlots.push_back(slot);
}

TextureArrayHandle MeshRenderer::CreateTextureArray(const TextureArrayDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.layerCount == 0 || desc.pixels == nullptr) {
        throw std::runtime_error("TextureArrayDesc 非法（宽 / 高 / 层数须 ≥ 1 且 pixels 非空）");
    }

    // 完整 mip 链长度 = floor(log2(max(w, h))) + 1。
    std::uint32_t levels = 1;
    for (std::uint32_t extent = std::max(desc.width, desc.height); extent > 1; extent >>= 1) {
        ++levels;
    }

    SDL_GPUTextureCreateInfo textureInfo {};
    textureInfo.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    textureInfo.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // COLOR_TARGET 是 SDL_GenerateMipmapsForGPUTexture 的硬性要求（生成 mip 时把各级当作渲染目标）。
    textureInfo.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    textureInfo.width                = desc.width;
    textureInfo.height               = desc.height;
    textureInfo.layer_count_or_depth = desc.layerCount;
    textureInfo.num_levels           = levels;
    textureInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (texture == nullptr) {
        throw std::runtime_error(std::string("创建纹理数组失败：") + SDL_GetError());
    }

    const std::uint32_t layerBytes = desc.width * desc.height * 4U;
    const std::uint32_t totalBytes = layerBytes * desc.layerCount;

    SDL_GPUTransferBufferCreateInfo transferInfo {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size  = totalBytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transfer == nullptr) {
        SDL_ReleaseGPUTexture(m_device, texture);
        throw std::runtime_error(std::string("创建纹理上传缓冲失败：") + SDL_GetError());
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, transfer, /*cycle=*/false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, transfer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throw std::runtime_error(std::string("映射纹理上传缓冲失败：") + SDL_GetError());
    }
    std::memcpy(mapped, desc.pixels, totalBytes);
    SDL_UnmapGPUTransferBuffer(m_device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, transfer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throw std::runtime_error(std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError());
    }

    // 逐层上传第 0 级（显式给 layer 与 d=1，避免 2D 数组的层 / 深度歧义）。
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    for (std::uint32_t layer = 0; layer < desc.layerCount; ++layer) {
        SDL_GPUTextureTransferInfo source { transfer, layer * layerBytes, 0, 0 };
        SDL_GPUTextureRegion       destination { texture, /*mip_level=*/0, /*layer=*/layer, 0, 0, 0,
                                                 desc.width, desc.height, /*d=*/1 };
        SDL_UploadToGPUTexture(copyPass, &source, &destination, /*cycle=*/false);
    }
    SDL_EndGPUCopyPass(copyPass);

    // mip 链由 SDL 从第 0 级生成（须在 pass 之外调用）。
    if (levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(commandBuffer, texture);
    }

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(m_device, /*wait_all=*/true, &fence, 1);
        SDL_ReleaseGPUFence(m_device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(m_device, transfer);

    std::uint32_t slot = 0;
    if (!m_freeTextureSlots.empty()) {
        slot = m_freeTextureSlots.back();
        m_freeTextureSlots.pop_back();
        m_textureArrays[slot] = texture;
    } else {
        slot = static_cast<std::uint32_t>(m_textureArrays.size());
        m_textureArrays.push_back(texture);
    }
    return TextureArrayHandle { slot + 1 };
}

void MeshRenderer::ReleaseTextureArray(TextureArrayHandle handle) noexcept {
    if (!handle.IsValid() || handle.id > m_textureArrays.size()) {
        return;
    }
    const std::uint32_t slot = handle.id - 1;
    if (m_textureArrays[slot] != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_textureArrays[slot]);
        m_textureArrays[slot] = nullptr;
    }
    if (m_albedoTexture.id == handle.id) {
        m_albedoTexture = TextureArrayHandle {};
    }
    if (m_normalTexture.id == handle.id) {
        m_normalTexture = TextureArrayHandle {};
    }
    m_freeTextureSlots.push_back(slot);
}

void MeshRenderer::SetSampledTextureArrays(TextureArrayHandle albedo, TextureArrayHandle normal) noexcept {
    m_albedoTexture = albedo;
    m_normalTexture = normal;
}

void MeshRenderer::SetMaterialUniform(const void* data, std::size_t size) noexcept {
    if (data == nullptr || size > kMaxMaterialUniformBytes) {
        return;
    }
    std::memcpy(m_materialUniform.data(), data, size);
    m_materialUniformSize = size;
}

void MeshRenderer::SetCamera(const CameraView& camera) noexcept {
    m_cameraUniform.viewProjection = camera.viewProjection;
}

void MeshRenderer::EnsureDepthTarget(std::uint32_t width, std::uint32_t height) {
    if (m_depthTexture != nullptr && m_depthWidth == width && m_depthHeight == height) {
        return;
    }
    if (m_depthTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_depthTexture);
        m_depthTexture = nullptr;
    }

    SDL_GPUTextureCreateInfo info {};
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    m_depthTexture = SDL_CreateGPUTexture(m_device, &info);
    if (m_depthTexture == nullptr) {
        throw std::runtime_error(std::string("创建深度目标失败：") + SDL_GetError());
    }
    m_depthWidth  = width;
    m_depthHeight = height;
}

void MeshRenderer::UploadCameraUniform(SDL_GPUCommandBuffer* commandBuffer) {
    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_cameraTransferBuffer, /*cycle=*/true);
    if (mapped == nullptr) {
        throw std::runtime_error(std::string("映射相机常量上传缓冲失败：") + SDL_GetError());
    }
    std::memcpy(mapped, &m_cameraUniform, sizeof(CameraUniform));
    SDL_UnmapGPUTransferBuffer(m_device, m_cameraTransferBuffer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTransferBufferLocation source { m_cameraTransferBuffer, 0 };
    SDL_GPUBufferRegion           destination { m_cameraUniformBuffer, 0,
                                      static_cast<Uint32>(sizeof(CameraUniform)) };
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    SDL_EndGPUCopyPass(copyPass);
}

bool MeshRenderer::RenderFrame(const MeshHandle* meshes, std::size_t meshCount, const SDL_FColor& clearColor,
                               IRenderOverlay* overlay) {
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        throw std::runtime_error(std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError());
    }

    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t   width     = 0;
    std::uint32_t   height    = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, m_window, &swapchain, &width, &height)) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        throw std::runtime_error(std::string("获取交换链纹理失败：") + SDL_GetError());
    }
    if (swapchain == nullptr) {
        // 窗口最小化等情形：本帧没有可渲染目标
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    EnsureDepthTarget(width, height);
    UploadCameraUniform(commandBuffer);

    SDL_GPUColorTargetInfo colorTarget {};
    colorTarget.texture     = swapchain;
    colorTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op    = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = clearColor;

    SDL_GPUDepthStencilTargetInfo depthTarget {};
    depthTarget.texture           = m_depthTexture;
    depthTarget.clear_depth       = 1.0F;
    depthTarget.load_op           = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op          = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.stencil_load_op   = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op  = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.cycle             = false;
    depthTarget.mip_level         = 0;
    depthTarget.layer             = 0;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, &depthTarget);
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);

    SDL_GPUBuffer* cameraBuffers[1] = { m_cameraUniformBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, cameraBuffers, 1);

    // 片元资源：纹理数组（槽 0 = albedo、槽 1 = 法线）与材质 uniform（槽 0）。
    // 两者都只在调用方设置过时才绑定 / 推送；引擎不解释其内容。
    if (m_albedoTexture.IsValid() && m_normalTexture.IsValid() &&
        m_albedoTexture.id <= m_textureArrays.size() && m_normalTexture.id <= m_textureArrays.size()) {
        SDL_GPUTextureSamplerBinding bindings[2] = {};
        bindings[0].texture = m_textureArrays[m_albedoTexture.id - 1];
        bindings[0].sampler = m_layerSampler;
        bindings[1].texture = m_textureArrays[m_normalTexture.id - 1];
        bindings[1].sampler = m_layerSampler;
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
    }
    if (m_materialUniformSize > 0) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, m_materialUniform.data(),
                                       static_cast<Uint32>(m_materialUniformSize));
    }

    if (meshes != nullptr) {
        for (std::size_t i = 0; i < meshCount; ++i) {
            const MeshHandle handle = meshes[i];
            if (!handle.IsValid() || handle.id > m_meshes.size()) {
                continue;
            }
            const MeshResources& resources = m_meshes[handle.id - 1];
            if (resources.vertexBuffer == nullptr || resources.indexBuffer == nullptr) {
                continue;
            }

            SDL_GPUBufferBinding vertexBinding { resources.vertexBuffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

            SDL_GPUBufferBinding indexBinding { resources.indexBuffer, 0 };
            SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

            SDL_DrawGPUIndexedPrimitives(pass, resources.indexCount, /*num_instances=*/1, /*first_index=*/0,
                                         /*vertex_offset=*/0, /*first_instance=*/0);
        }
    }

    SDL_EndGPURenderPass(pass);

    // 叠加层：与 3D 通道共用本命令缓冲（交换链纹理只在获取它的命令缓冲里有效）。
    if (overlay != nullptr) {
        overlay->DrawOverlay(commandBuffer, swapchain, width, height);
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
    return true;
}

}  // namespace vx
