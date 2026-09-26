#include "render/mesh_renderer.hpp"

#include "core/log.hpp"

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

/// 把生效 MSAA 档位（1 / 2 / 4 / 8）映射为 SDL 的采样数枚举。非合法档位一律按 1 处理（防御）。
[[nodiscard]] SDL_GPUSampleCount ToSdlSampleCount(std::uint32_t samples) noexcept {
    switch (samples) {
        case 2:
            return SDL_GPU_SAMPLECOUNT_2;
        case 4:
            return SDL_GPU_SAMPLECOUNT_4;
        case 8:
            return SDL_GPU_SAMPLECOUNT_8;
        default:
            return SDL_GPU_SAMPLECOUNT_1;
    }
}

/// 把请求的 MSAA 档位归一为**当前设备实际支持**的最高档（≤ 请求值），并只保留 1 / 2 / 4 / 8。
///
/// 为什么不能只信请求值：SDL_gpu 的图形管线采样数**必须与渲进的目标一致**，否则
/// `SDL_BeginGPURenderPass` 会失败。虽然 `SDL_CreateGPUTexture` 会把不支持的采样数自动降级，
/// 但降级后的目标会与管线（按请求值烘焙）不一致。故这里先查
/// `SDL_GPUTextureSupportsSampleCount`（颜色 `R16G16B16A16_FLOAT` 与深度 `D32_FLOAT` 都要支持），
/// 取二者都支持的最高档；引擎**不定义档位口径**（那是配置层的职责），这里只做硬件能力适配。
[[nodiscard]] std::uint32_t ResolveSupportedSampleCount(SDL_GPUDevice* device, std::uint32_t requested) noexcept {
    for (std::uint32_t tier = 8; tier >= 1; tier >>= 1) {
        if (tier > requested) {
            continue;
        }
        const SDL_GPUSampleCount count = ToSdlSampleCount(tier);
        if (SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, count) &&
            SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, count)) {
            return tier;
        }
    }
    return 1;  // 单采样是所有后端的最低保证
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
    // 注意：T23 起把两个 Shader **持有到析构**（而非创建管线后立即释放）——MSAA 档位变化时
    // 需按新的采样数**重建主通道管线**，重建要复用同一批 Shader 对象，避免重新读盘。
    m_meshVertexShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".vert" + extension),
                                SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                ShaderResourceCounts { 0, 0, 1, 0 });
    // 片元着色器：6 个采样纹理（slot 0..4 = albedo / normal / roughness / AO / macro，slot 5 = 阴影深度数组）
    //              + 3 个 uniform 块（slot 0 材质、slot 1 光照、slot 2 阴影）。
    // SDL_gpu 每阶段采样器上限为 16（MAX_TEXTURE_SAMPLERS_PER_STAGE），6 个无需把 roughness/AO 打包进通道。
    m_meshFragmentShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".frag" + extension),
                                SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                ShaderResourceCounts { /*samplers=*/6, 0, 0, /*uniformBuffers=*/3 });

    // 主通道管线：先按单采样创建（`SetMsaaSampleCount` 通常在构造之后调用；档位变化时
    // `EnsureMainPipeline` 用同一批 Shader 重建），保证构造期即验证"设备 + 管线 + Shader"链路。
    CreateMainPipeline(1);

    // ---- 色调映射管线（T20 / ADR 0010 P0）：HDR 目标 → 交换链 ----
    // 全屏三角形（顶点缓冲为空，位置由 gl_VertexIndex 生成）；无深度目标、sample_count = 1、不剔除。
    {
        SDL_GPUShader* tonemapVertex =
            create_shader_from_file(m_device, shader_dir / ("tonemap.vert" + extension),
                                    SDL_GPU_SHADERSTAGE_VERTEX, artifact.format, ShaderResourceCounts {});
        SDL_GPUShader* tonemapFragment =
            create_shader_from_file(m_device, shader_dir / ("tonemap.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/1, 0, 0, /*uniformBuffers=*/1 });

        SDL_GPUColorTargetDescription tonemapColorTarget {};
        tonemapColorTarget.format = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

        SDL_GPUGraphicsPipelineCreateInfo tonemapInfo {};
        tonemapInfo.vertex_shader   = tonemapVertex;
        tonemapInfo.fragment_shader = tonemapFragment;
        // 无顶点输入（顶点缓冲与属性均为空）。
        tonemapInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        tonemapInfo.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
        tonemapInfo.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_NONE;
        tonemapInfo.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        tonemapInfo.rasterizer_state.enable_depth_clip = false;

        tonemapInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        tonemapInfo.target_info.color_target_descriptions = &tonemapColorTarget;
        tonemapInfo.target_info.num_color_targets         = 1;
        tonemapInfo.target_info.has_depth_stencil_target  = false;

        m_tonemapPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &tonemapInfo);
        SDL_ReleaseGPUShader(m_device, tonemapVertex);
        SDL_ReleaseGPUShader(m_device, tonemapFragment);

        if (m_tonemapPipeline == nullptr) {
            throw std::runtime_error(std::string("创建色调映射管线失败：") + SDL_GetError());
        }
    }

    // ---- 阴影深度管线（T21b / ADR 0010 P1）：无颜色目标，仅写深度 ----
    {
        SDL_GPUShader* shadowVertex =
            create_shader_from_file(m_device, shader_dir / ("shadow.vert" + extension),
                                    SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/0, 0, /*storageBuffers=*/1, 0 });
        // SDL3_gpu 的 SDL_CreateGPUGraphicsPipeline 断言片元着色器非空（'!"Fragment shader cannot be NULL!"'），
        // 故即使 num_color_targets = 0 也必须挂一个空入口（shadow.frag 不做任何计算）。
        SDL_GPUShader* shadowFragment =
            create_shader_from_file(m_device, shader_dir / ("shadow.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format, ShaderResourceCounts {});

        // 只需要位置属性（法线不参与深度写入）；布局与 MeshVertex 一致，故可直接复用同一批顶点缓冲。
        SDL_GPUVertexBufferDescription shadowVertexBuffer {};
        shadowVertexBuffer.slot               = 0;
        shadowVertexBuffer.pitch              = static_cast<Uint32>(sizeof(MeshVertex));
        shadowVertexBuffer.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        shadowVertexBuffer.instance_step_rate = 0;

        const SDL_GPUVertexAttribute shadowAttribute[] = {
            { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(MeshVertex, position)) },
        };

        SDL_GPUVertexInputState shadowVertexInput {};
        shadowVertexInput.vertex_buffer_descriptions = &shadowVertexBuffer;
        shadowVertexInput.num_vertex_buffers         = 1;
        shadowVertexInput.vertex_attributes          = shadowAttribute;
        shadowVertexInput.num_vertex_attributes      = static_cast<Uint32>(std::size(shadowAttribute));

        SDL_GPUGraphicsPipelineCreateInfo shadowInfo {};
        shadowInfo.vertex_shader   = shadowVertex;
        // 空片元入口（SDL_gpu 不允许 nullptr）：无输出、无计算，深度写入由光栅化阶段完成。
        shadowInfo.fragment_shader = shadowFragment;
        shadowInfo.vertex_input_state = shadowVertexInput;
        shadowInfo.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        shadowInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        // 剔除策略 = BACK（保持与主通道一致的 CCW 正面约定），而非 FRONT：
        // 地表是**单面**高度场外壳（只有朝上的那一层几何，不是封闭实体），
        // 从太阳方向看，受光面正是正面——若剔除 FRONT，整个受光地形都不会写入深度，
        // 阴影图会变成一片空白、**完全没有阴影**。故必须保留正面、剔除背面，
        // 再靠 normal_offset + depth_bias 消除同一表面上的自遮挡（acne）。
        shadowInfo.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_BACK;
        shadowInfo.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        shadowInfo.rasterizer_state.enable_depth_clip = true;

        shadowInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        shadowInfo.depth_stencil_state.compare_op          = SDL_GPU_COMPAREOP_LESS;
        shadowInfo.depth_stencil_state.enable_depth_test   = true;
        shadowInfo.depth_stencil_state.enable_depth_write  = true;
        shadowInfo.depth_stencil_state.enable_stencil_test = false;

        shadowInfo.target_info.color_target_descriptions = nullptr;
        shadowInfo.target_info.num_color_targets         = 0;  // 纯深度通道
        shadowInfo.target_info.depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        shadowInfo.target_info.has_depth_stencil_target  = true;

        m_shadowPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &shadowInfo);
        SDL_ReleaseGPUShader(m_device, shadowVertex);
        SDL_ReleaseGPUShader(m_device, shadowFragment);

        if (m_shadowPipeline == nullptr) {
            throw std::runtime_error(std::string("创建阴影深度管线失败：") + SDL_GetError());
        }
    }

    // 色调映射采样 HDR 目标：clamp 寻址（屏幕空间后处理不留接缝）+ 线性过滤，单级纹理。
    SDL_GPUSamplerCreateInfo hdrSamplerInfo {};
    hdrSamplerInfo.min_filter     = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mag_filter     = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    hdrSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSamplerInfo.min_lod        = 0.0F;
    hdrSamplerInfo.max_lod        = 0.0F;
    m_hdrSampler = SDL_CreateGPUSampler(m_device, &hdrSamplerInfo);
    if (m_hdrSampler == nullptr) {
        throw std::runtime_error(std::string("创建 HDR 采样器失败：") + SDL_GetError());
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

    // 阴影深度采样器：clamp 寻址（级联边界处不跨图取到对侧）+ **最近邻** + 不采样 mip。
    // 为什么用最近邻而非线性：深度值是"到光源的距离"，线性插值会造出两个表面之间的**非物理深度**，
    // 使 PCF 的每次比较都不落在真实表面上（结果在"漏光 / 过度自阴影"之间摆动）；
    // 且 D32_FLOAT 在部分后端（Vulkan）不保证支持线性采样，硬件比较采样又需另一条管线。
    // 因此标准做法是最近邻取值 + **手动 3×3 PCF**（见 mesh.frag）。
    SDL_GPUSamplerCreateInfo shadowSamplerInfo {};
    shadowSamplerInfo.min_filter     = SDL_GPU_FILTER_NEAREST;
    shadowSamplerInfo.mag_filter     = SDL_GPU_FILTER_NEAREST;
    shadowSamplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    shadowSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.min_lod        = 0.0F;
    shadowSamplerInfo.max_lod        = 0.0F;
    m_shadowSampler = SDL_CreateGPUSampler(m_device, &shadowSamplerInfo);
    if (m_shadowSampler == nullptr) {
        throw std::runtime_error(std::string("创建阴影采样器失败：") + SDL_GetError());
    }

    // 每级一个只读 storage buffer 存该级光空间矩阵（SDL_gpu 的 storage buffer 绑定不带偏移，
    // 故每级一个缓冲；非纹理资源，不计入 textureBytes）。
    for (SDL_GPUBuffer*& buffer : m_shadowMatrixBuffers) {
        SDL_GPUBufferCreateInfo matrixBufferInfo {};
        matrixBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        matrixBufferInfo.size  = static_cast<Uint32>(sizeof(glm::mat4));
        buffer                 = SDL_CreateGPUBuffer(m_device, &matrixBufferInfo);
        if (buffer == nullptr) {
            throw std::runtime_error(std::string("创建阴影矩阵缓冲失败：") + SDL_GetError());
        }
    }

    SDL_GPUTransferBufferCreateInfo shadowTransferInfo {};
    shadowTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    shadowTransferInfo.size  = static_cast<Uint32>(sizeof(glm::mat4) * kMaxShadowCascades);
    m_shadowMatrixTransfer   = SDL_CreateGPUTransferBuffer(m_device, &shadowTransferInfo);
    if (m_shadowMatrixTransfer == nullptr) {
        throw std::runtime_error(std::string("创建阴影矩阵上传缓冲失败：") + SDL_GetError());
    }
}

void MeshRenderer::CreateMainPipeline(std::uint32_t sampleCount) {
    // 顶点布局：与 MeshVertex 一一对应（pitch = 单个顶点大小，stride 连续）。
    SDL_GPUVertexBufferDescription vertexBufferDescription {};
    vertexBufferDescription.slot               = 0;
    vertexBufferDescription.pitch              = static_cast<Uint32>(sizeof(MeshVertex));
    vertexBufferDescription.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
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
    // T20 / ADR 0010：主通道写**离屏 HDR 目标**（`R16G16B16A16_FLOAT`），不再直接写交换链；
    // 亮度超过 1.0 的高光因此得以保留，交由 tonemap.* 通道做曝光 + 色调映射 + sRGB 编码。
    // MSAA 档位 > 1 时该格式同样是 MSAA 颜色目标的格式（两者必须一致）。
    colorTargetDescription.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_GPUGraphicsPipelineCreateInfo info {};
    info.vertex_shader      = m_meshVertexShader;
    info.fragment_shader    = m_meshFragmentShader;
    info.vertex_input_state = vertexInput;
    info.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;

    // T23 / ADR 0010 P3：采样数**必须与渲进的颜色 / 深度目标一致**，否则 `SDL_BeginGPURenderPass` 报错。
    info.multisample_state.sample_count = ToSdlSampleCount(sampleCount);

    info.depth_stencil_state.compare_op          = SDL_GPU_COMPAREOP_LESS;
    info.depth_stencil_state.enable_depth_test   = true;
    info.depth_stencil_state.enable_depth_write  = true;
    info.depth_stencil_state.enable_stencil_test = false;

    info.target_info.color_target_descriptions = &colorTargetDescription;
    info.target_info.num_color_targets         = 1;
    info.target_info.depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target  = true;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    if (m_pipeline == nullptr) {
        m_pipelineSampleCount = 0;
        throw std::runtime_error(std::string("SDL_CreateGPUGraphicsPipeline 失败：") + SDL_GetError());
    }
    m_pipelineSampleCount = sampleCount;
}

void MeshRenderer::EnsureMainPipeline(std::uint32_t sampleCount) {
    if (m_pipeline != nullptr && m_pipelineSampleCount == sampleCount) {
        return;
    }
    if (m_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
    CreateMainPipeline(sampleCount);
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
    if (m_hdrTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_hdrTexture);
    }
    if (m_msaaColorTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_msaaColorTexture);
    }
    if (m_shadowTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_shadowTexture);
    }
    if (m_cameraTransferBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_cameraTransferBuffer);
    }
    if (m_shadowMatrixTransfer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_shadowMatrixTransfer);
    }
    if (m_vertexStagingBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_vertexStagingBuffer);
    }
    if (m_cameraUniformBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, m_cameraUniformBuffer);
    }
    for (SDL_GPUBuffer* buffer : m_shadowMatrixBuffers) {
        if (buffer != nullptr) {
            SDL_ReleaseGPUBuffer(m_device, buffer);
        }
    }
    for (SDL_GPUTexture* texture : m_textureArrays) {
        if (texture != nullptr) {
            SDL_ReleaseGPUTexture(m_device, texture);
        }
    }
    if (m_layerSampler != nullptr) {
        SDL_ReleaseGPUSampler(m_device, m_layerSampler);
    }
    if (m_hdrSampler != nullptr) {
        SDL_ReleaseGPUSampler(m_device, m_hdrSampler);
    }
    if (m_shadowSampler != nullptr) {
        SDL_ReleaseGPUSampler(m_device, m_shadowSampler);
    }
    if (m_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    }
    if (m_tonemapPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_tonemapPipeline);
    }
    if (m_shadowPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_shadowPipeline);
    }
    // 主通道 Shader 常驻到析构（T23）：必须在**使用它们的管线**销毁之后再释放。
    if (m_meshVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_meshVertexShader);
    }
    if (m_meshFragmentShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_meshFragmentShader);
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
    // 显存记账（ADR 0010）：按**含 mip 链**的估算累加（纯函数 `EstimateTextureArrayBytes`，可单测）。
    const std::uint64_t bytes = EstimateTextureArrayBytes(desc.width, desc.height, desc.layerCount);
    if (!m_freeTextureSlots.empty()) {
        slot                       = m_freeTextureSlots.back();
        m_freeTextureSlots.pop_back();
        m_textureArrays[slot]      = texture;
        m_textureArrayBytes[slot]  = bytes;
    } else {
        slot = static_cast<std::uint32_t>(m_textureArrays.size());
        m_textureArrays.push_back(texture);
        m_textureArrayBytes.push_back(bytes);
    }
    m_stats.textureBytes += bytes;
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
    // 显存记账同步扣减（重复释放时记账字节已为 0，扣 0 无副作用）。
    m_stats.textureBytes -= m_textureArrayBytes[slot];
    m_textureArrayBytes[slot] = 0;
    if (m_albedoTexture.id == handle.id) {
        m_albedoTexture = TextureArrayHandle {};
    }
    if (m_normalTexture.id == handle.id) {
        m_normalTexture = TextureArrayHandle {};
    }
    if (m_roughnessTexture.id == handle.id) {
        m_roughnessTexture = TextureArrayHandle {};
    }
    if (m_aoTexture.id == handle.id) {
        m_aoTexture = TextureArrayHandle {};
    }
    if (m_macroTexture.id == handle.id) {
        m_macroTexture = TextureArrayHandle {};
    }
    m_freeTextureSlots.push_back(slot);
}

void MeshRenderer::SetSampledTextureArrays(TextureArrayHandle albedo, TextureArrayHandle normal,
                                           TextureArrayHandle roughness, TextureArrayHandle ao,
                                           TextureArrayHandle macro) noexcept {
    m_albedoTexture    = albedo;
    m_normalTexture    = normal;
    m_roughnessTexture = roughness;
    m_aoTexture        = ao;
    m_macroTexture     = macro;
}

void MeshRenderer::SetMaterialUniform(const void* data, std::size_t size) noexcept {
    if (data == nullptr || size > kMaxMaterialUniformBytes) {
        return;
    }
    std::memcpy(m_materialUniform.data(), data, size);
    m_materialUniformSize = size;
}

void MeshRenderer::SetLightingUniform(const void* data, std::size_t size) noexcept {
    if (data == nullptr || size > kMaxLightingUniformBytes) {
        return;
    }
    std::memcpy(m_lightingUniform.data(), data, size);
    m_lightingUniformSize = size;
}

void MeshRenderer::SetShadowCascades(const ShadowUniform& uniform, std::uint32_t cascadeCount,
                                     std::uint32_t resolution) noexcept {
    m_shadowUniform      = uniform;
    m_shadowUniformValid = true;
    // 级数钳制到 [1, kMaxShadowCascades]；分辨率钳制到"2 的幂且 ≥ 256"（配置已校验，这里只做防御）。
    m_shadowCascadeCount = std::clamp(cascadeCount, 1U, static_cast<std::uint32_t>(kMaxShadowCascades));
    std::uint32_t clampedResolution = 256;
    while (clampedResolution < resolution) {
        clampedResolution <<= 1U;
    }
    m_shadowResolution = clampedResolution;
}

void MeshRenderer::SetCamera(const CameraView& camera) noexcept {
    m_cameraUniform.viewProjection = camera.viewProjection;
}

void MeshRenderer::SetMsaaSampleCount(std::uint32_t sampleCount) noexcept {
    // 引擎不定义档位口径（来自上层设置），这里只做**硬件能力**归一：取 ≤ 请求值且被设备支持的最高档。
    const std::uint32_t effective = ResolveSupportedSampleCount(m_device, sampleCount);
    if (m_msaaSampleCountLogged && effective == m_msaaSampleCount && sampleCount == m_requestedMsaaSampleCount) {
        return;  // 未变化且已记过日志：不重复打日志（允许调用方每帧调用）
    }
    const std::uint32_t previous = m_msaaSampleCount;
    m_requestedMsaaSampleCount = sampleCount;
    m_msaaSampleCount          = effective;
    m_msaaSampleCountLogged    = true;
    if (effective != sampleCount) {
        VX_LOG_WARN("MSAA：请求 %u× 不被当前设备支持（颜色 R16G16B16A16_FLOAT / 深度 D32_FLOAT），"
                    "已降级为 %u×（生效档位 %u× → %u×）",
                    sampleCount, effective, previous, effective);
    } else {
        VX_LOG_INFO("MSAA：%u×（%s；生效档位 %u× → %u×）", effective,
                    (effective <= 1) ? "关闭，零额外显存开销" : "开启，主通道渲进多采样目标后 resolve 到单采样 HDR 目标",
                    previous, effective);
    }
}

void MeshRenderer::EnsureDepthTarget(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount) {
    if (m_depthTexture != nullptr && m_depthWidth == width && m_depthHeight == height &&
        m_depthSampleCount == sampleCount) {
        return;
    }
    if (m_depthTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_depthTexture);
        m_depthTexture = nullptr;
        m_stats.textureBytes -= m_depthBytes;
        m_depthBytes = 0;
    }

    SDL_GPUTextureCreateInfo info {};
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    // T23：主通道深度目标的采样数与 MSAA 档位一致（档位 = 1 即单采样，回到 P0 行为）。
    info.sample_count         = ToSdlSampleCount(sampleCount);

    m_depthTexture = SDL_CreateGPUTexture(m_device, &info);
    if (m_depthTexture == nullptr) {
        throw std::runtime_error(std::string("创建深度目标失败：") + SDL_GetError());
    }
    m_depthWidth       = width;
    m_depthHeight      = height;
    m_depthSampleCount = sampleCount;
    // 显存记账：D32_FLOAT 按 4 字节 / 像素 × 采样数（ADR 0010 的记账义务；MSAA 深度计入同一项）。
    m_depthBytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL *
                   static_cast<std::uint64_t>(sampleCount);
    m_stats.textureBytes += m_depthBytes;
    m_textureAccountingDirty = true;
}

void MeshRenderer::EnsureMsaaColorTarget(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount) {
    if (sampleCount <= 1) {
        // 档位 = 1：**零开销路径**——不创建 MSAA 纹理；若之前存在（档位由 > 1 降到 1）则释放并退回记账。
        if (m_msaaColorTexture != nullptr) {
            SDL_ReleaseGPUTexture(m_device, m_msaaColorTexture);
            m_msaaColorTexture     = nullptr;
            m_msaaWidth            = 0;
            m_msaaHeight           = 0;
            m_msaaColorSampleCount = 0;
            m_stats.textureBytes -= m_msaaColorBytes;
            m_msaaColorBytes         = 0;
            m_textureAccountingDirty = true;
        }
        return;
    }
    if (m_msaaColorTexture != nullptr && m_msaaWidth == width && m_msaaHeight == height &&
        m_msaaColorSampleCount == sampleCount) {
        return;
    }
    if (m_msaaColorTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_msaaColorTexture);
        m_msaaColorTexture = nullptr;
        m_stats.textureBytes -= m_msaaColorBytes;
        m_msaaColorBytes = 0;
    }

    SDL_GPUTextureCreateInfo info {};
    info.type   = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    // MSAA 纹理**只作颜色目标**：解析目标是单采样 HDR 目标，MSAA 纹理本身不作为采样源，
    // 故 usage 只需 COLOR_TARGET（不加 SAMPLER，省去无用的绑定位）。
    info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = ToSdlSampleCount(sampleCount);

    m_msaaColorTexture = SDL_CreateGPUTexture(m_device, &info);
    if (m_msaaColorTexture == nullptr) {
        throw std::runtime_error(std::string("创建 MSAA 颜色目标失败：") + SDL_GetError());
    }
    m_msaaWidth            = width;
    m_msaaHeight           = height;
    m_msaaColorSampleCount = sampleCount;
    // 显存记账：R16G16B16A16_FLOAT 按 8 字节 / 像素 × 采样数（ADR 0010 的强制记账义务）。
    m_msaaColorBytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 8ULL *
                       static_cast<std::uint64_t>(sampleCount);
    m_stats.textureBytes += m_msaaColorBytes;
    m_textureAccountingDirty = true;
}

void MeshRenderer::EnsureHdrTarget(std::uint32_t width, std::uint32_t height) {
    if (m_hdrTexture != nullptr && m_hdrWidth == width && m_hdrHeight == height) {
        return;
    }
    if (m_hdrTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_hdrTexture);
        m_hdrTexture = nullptr;
        m_stats.textureBytes -= m_hdrBytes;
        m_hdrBytes = 0;
    }

    SDL_GPUTextureCreateInfo info {};
    info.type   = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    // COLOR_TARGET：主通道写入；SAMPLER：色调映射通道读取。
    info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    m_hdrTexture = SDL_CreateGPUTexture(m_device, &info);
    if (m_hdrTexture == nullptr) {
        throw std::runtime_error(std::string("创建 HDR 颜色目标失败：") + SDL_GetError());
    }
    m_hdrWidth  = width;
    m_hdrHeight = height;
    // 显存记账：R16G16B16A16_FLOAT 按 8 字节 / 像素（ADR 0010 的记账义务）。
    m_hdrBytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 8ULL;
    m_stats.textureBytes += m_hdrBytes;
    m_textureAccountingDirty = true;
}

void MeshRenderer::EnsureShadowTarget() {
    if (m_shadowTexture != nullptr && m_shadowTextureResolution == m_shadowResolution &&
        m_shadowTextureCascades == m_shadowCascadeCount) {
        return;
    }
    if (m_shadowTexture != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_shadowTexture);
        m_shadowTexture = nullptr;
        m_stats.textureBytes -= m_shadowTextureBytes;
        m_shadowTextureBytes = 0;
    }

    SDL_GPUTextureCreateInfo info {};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;  // 每级一层：层 i 即级联 i
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    // DEPTH_STENCIL_TARGET：阴影通道写入；SAMPLER：主通道采样比较。
    info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = m_shadowResolution;
    info.height               = m_shadowResolution;
    info.layer_count_or_depth = m_shadowCascadeCount;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    m_shadowTexture = SDL_CreateGPUTexture(m_device, &info);
    if (m_shadowTexture == nullptr) {
        throw std::runtime_error(std::string("创建阴影深度数组失败：") + SDL_GetError());
    }
    m_shadowTextureResolution = m_shadowResolution;
    m_shadowTextureCascades   = m_shadowCascadeCount;
    // 显存记账：D32_FLOAT 按 4 字节 / 像素（ADR 0010 的强制记账义务）。
    m_shadowTextureBytes = static_cast<std::uint64_t>(m_shadowResolution) *
                           static_cast<std::uint64_t>(m_shadowResolution) * 4ULL *
                           static_cast<std::uint64_t>(m_shadowCascadeCount);
    m_stats.textureBytes += m_shadowTextureBytes;
    m_textureAccountingDirty = true;
}

void MeshRenderer::UploadShadowMatrices(SDL_GPUCommandBuffer* commandBuffer) {
    const std::size_t matrixBytes = sizeof(glm::mat4) * static_cast<std::size_t>(kMaxShadowCascades);
    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_shadowMatrixTransfer, /*cycle=*/true);
    if (mapped == nullptr) {
        throw std::runtime_error(std::string("映射阴影矩阵上传缓冲失败：") + SDL_GetError());
    }
    std::memcpy(mapped, m_shadowUniform.lightMatrices, matrixBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_shadowMatrixTransfer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    for (int cascade = 0; cascade < kMaxShadowCascades; ++cascade) {
        const std::uint32_t offset = static_cast<std::uint32_t>(cascade) * static_cast<std::uint32_t>(sizeof(glm::mat4));
        SDL_GPUTransferBufferLocation source { m_shadowMatrixTransfer, offset };
        SDL_GPUBufferRegion destination { m_shadowMatrixBuffers[static_cast<std::size_t>(cascade)], 0,
                                          static_cast<Uint32>(sizeof(glm::mat4)) };
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    }
    SDL_EndGPUCopyPass(copyPass);
}

void MeshRenderer::LogTextureAccounting(std::uint32_t width, std::uint32_t height) const {
    // ADR 0010 的记账义务：把纹理显存**按项**打到日志（首次创建与每次尺寸 / 档位变化各记一次），
    // 这样"预算是否达标"可以直接在日志里核对，不必依赖面板读数或截图。
    // T23 起把 MSAA 颜色目标与 MSAA 深度目标也列出，使"开 / 关 MSAA 的代价"可直接对比。
    std::uint64_t materialBytes = 0;
    for (const std::uint64_t bytes : m_textureArrayBytes) {
        materialBytes += bytes;
    }
    constexpr double kBytesPerMb = 1024.0 * 1024.0;
    // 深度目标按"单采样基准 + MSAA 增量"拆开展示，两项之和恰为 m_depthBytes（不重复计数）。
    const std::uint64_t baseDepthBytes =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL;
    const std::uint64_t msaaDepthBytes = (m_depthBytes > baseDepthBytes) ? (m_depthBytes - baseDepthBytes) : 0ULL;
    const std::uint64_t msaaTotalBytes = m_msaaColorBytes + msaaDepthBytes;
    const double        totalMb        = static_cast<double>(m_stats.textureBytes) / kBytesPerMb;
    const double        shadowShare    = (m_stats.textureBytes > 0)
                                             ? 100.0 * static_cast<double>(m_shadowTextureBytes) /
                                                   static_cast<double>(m_stats.textureBytes)
                                             : 0.0;
    VX_LOG_INFO("GPU 纹理显存记账：材质数组 %.2f MB + 深度目标 %.2f MB + HDR 目标 %.2f MB + "
                "阴影 %u 级 %u² %.2f MB（占 %.1f%%）+ MSAA %u×（颜色 %.2f MB + 深度 %.2f MB = %.2f MB）"
                " = 合计 %.2f MB（交换链 %ux%u）",
                static_cast<double>(materialBytes) / kBytesPerMb, static_cast<double>(baseDepthBytes) / kBytesPerMb,
                static_cast<double>(m_hdrBytes) / kBytesPerMb, m_shadowTextureCascades, m_shadowTextureResolution,
                static_cast<double>(m_shadowTextureBytes) / kBytesPerMb, shadowShare, m_msaaSampleCount,
                static_cast<double>(m_msaaColorBytes) / kBytesPerMb, static_cast<double>(msaaDepthBytes) / kBytesPerMb,
                static_cast<double>(msaaTotalBytes) / kBytesPerMb, totalMb, width, height);
}

void MeshRenderer::DrawMeshes(SDL_GPURenderPass* pass, const MeshHandle* meshes, std::size_t meshCount) {
    if (meshes == nullptr) {
        return;
    }
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

        // 统计**实际执行**的绘制（T24）：主通道与阴影通道的索引绘制都计入。
        ++m_stats.drawCalls;
        m_stats.triangleCount += resources.indexCount / 3U;
        m_stats.vertexCount += resources.vertexCount;
    }
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

    EnsureDepthTarget(width, height, m_msaaSampleCount);
    EnsureHdrTarget(width, height);
    // T23：档位 > 1 时创建多采样颜色目标；档位 = 1 时不创建（直接渲进单采样 HDR 目标）。
    EnsureMsaaColorTarget(width, height, m_msaaSampleCount);
    // 主通道管线的采样数必须与渲进的目标一致：档位变化时用常驻 Shader 重建。
    EnsureMainPipeline(m_msaaSampleCount);
    EnsureShadowTarget();
    UploadCameraUniform(commandBuffer);
    // 阴影关闭时不上传矩阵（省一次每帧的小拷贝；着色器也整体跳过采样）。
    if (m_shadowUniformValid && m_shadowUniform.enabled > 0.5F) {
        UploadShadowMatrices(commandBuffer);
    }

    // 显存记账（ADR 0010）：仅在渲染目标被（重）创建的帧打印一次，避免逐帧刷屏。
    if (m_textureAccountingDirty) {
        LogTextureAccounting(width, height);
        m_textureAccountingDirty = false;
    }

    // 本帧绘制统计清零（纹理显存记账是持久的，不在此列）。
    m_stats.drawCalls     = 0;
    m_stats.triangleCount = 0;
    m_stats.vertexCount   = 0;

    // ---- 阴影通道（T21b / ADR 0010 P1）：必须在**主通道之前**，逐级把同一批网格写进深度数组的一层 ----
    // 顶点是相机相对坐标，光空间矩阵也建立在同一坐标系（见 shadow_cascade.hpp），故这里直接复用
    // 主通道的顶点 / 索引缓冲，不需要任何重传或坐标补偿。
    if (m_shadowUniformValid && m_shadowUniform.enabled > 0.5F && m_shadowTexture != nullptr) {
        for (std::uint32_t cascade = 0; cascade < m_shadowCascadeCount; ++cascade) {
            SDL_GPUDepthStencilTargetInfo shadowTarget {};
            shadowTarget.texture          = m_shadowTexture;
            shadowTarget.clear_depth      = 1.0F;
            shadowTarget.load_op          = SDL_GPU_LOADOP_CLEAR;
            shadowTarget.store_op         = SDL_GPU_STOREOP_STORE;
            shadowTarget.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
            shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            shadowTarget.cycle            = false;
            shadowTarget.mip_level        = 0;
            shadowTarget.layer            = static_cast<Uint8>(cascade);  // 深度数组：层 i 即级联 i

            SDL_GPURenderPass* shadowPass = SDL_BeginGPURenderPass(commandBuffer, nullptr, 0, &shadowTarget);
            if (shadowPass == nullptr) {
                break;
            }
            SDL_BindGPUGraphicsPipeline(shadowPass, m_shadowPipeline);

            // 该级的光空间矩阵（顶点只读 storage buffer，slot 0；与相机矩阵同类机制）。
            SDL_GPUBuffer* matrixBuffers[1] = { m_shadowMatrixBuffers[cascade] };
            SDL_BindGPUVertexStorageBuffers(shadowPass, 0, matrixBuffers, 1);

            DrawMeshes(shadowPass, meshes, meshCount);
            SDL_EndGPURenderPass(shadowPass);
        }
    }

    // 主通道渲到**离屏 HDR 目标**（T20）：亮度不被交换链的 8 位范围截断。
    // T23 / ADR 0010 P3：MSAA 档位 > 1 时改渲进多采样颜色目标，并在**同一渲染通道内** resolve 到
    // 既有的单采样 HDR 目标（`resolve_texture`）；色调映射通道仍采样该单采样目标（不读 MSAA 纹理）。
    // 档位 = 1 时 `m_msaaColorTexture` 为空指针 → 直接渲进 HDR 目标，回到 P0 行为（零额外开销）。
    const bool             msaaEnabled = (m_msaaColorTexture != nullptr);
    SDL_GPUColorTargetInfo colorTarget {};
    colorTarget.texture                 = msaaEnabled ? m_msaaColorTexture : m_hdrTexture;
    colorTarget.mip_level               = 0;
    colorTarget.layer_or_depth_plane    = 0;
    colorTarget.load_op                 = SDL_GPU_LOADOP_CLEAR;
    colorTarget.clear_color             = clearColor;
    if (msaaEnabled) {
        // 用 `RESOLVE` 而非 `RESOLVE_AND_STORE`：多采样目标的内容**此后不再被采样**（色调映射读的是
        // 单采样 HDR 目标），保留它纯属浪费带宽。`SDL_gpu.h` 亦注明 `RESOLVE` 是"最省带宽"的解析方式，
        // 而 `RESOLVE_AND_STORE` "需要可观的显存带宽"。
        colorTarget.store_op          = SDL_GPU_STOREOP_RESOLVE;
        colorTarget.resolve_texture   = m_hdrTexture;
        colorTarget.resolve_mip_level = 0;
        colorTarget.resolve_layer     = 0;
    } else {
        colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    }

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

    // 片元资源：采样器槽 0..4 = albedo / normal / roughness / AO / macro（材质四件套 + 宏观变化），
    // 槽 5 = 阴影深度数组；uniform 槽 0 = 材质、槽 1 = 光照、槽 2 = 阴影。
    // 都在调用方设置过时才绑定 / 推送；引擎不解释其内容。
    const bool materialArraysReady =
        m_albedoTexture.IsValid() && m_normalTexture.IsValid() && m_roughnessTexture.IsValid() &&
        m_aoTexture.IsValid() && m_macroTexture.IsValid() && m_albedoTexture.id <= m_textureArrays.size() &&
        m_normalTexture.id <= m_textureArrays.size() && m_roughnessTexture.id <= m_textureArrays.size() &&
        m_aoTexture.id <= m_textureArrays.size() && m_macroTexture.id <= m_textureArrays.size();
    if (materialArraysReady) {
        SDL_GPUTextureSamplerBinding bindings[6] = {};
        bindings[0].texture = m_textureArrays[m_albedoTexture.id - 1];
        bindings[0].sampler = m_layerSampler;
        bindings[1].texture = m_textureArrays[m_normalTexture.id - 1];
        bindings[1].sampler = m_layerSampler;
        bindings[2].texture = m_textureArrays[m_roughnessTexture.id - 1];
        bindings[2].sampler = m_layerSampler;
        bindings[3].texture = m_textureArrays[m_aoTexture.id - 1];
        bindings[3].sampler = m_layerSampler;
        bindings[4].texture = m_textureArrays[m_macroTexture.id - 1];
        bindings[4].sampler = m_layerSampler;
        // 着色器恒声明阴影采样器（槽 5），故即使阴影关闭也必须绑定（关闭时由 uniform 的 enabled 跳过采样）。
        bindings[5].texture = m_shadowTexture;
        bindings[5].sampler = m_shadowSampler;
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 6);
    }
    if (m_materialUniformSize > 0) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, m_materialUniform.data(),
                                       static_cast<Uint32>(m_materialUniformSize));
    }
    // 光照（T21a / T21c）：片元 uniform 槽 1（槽 0 已被材质占用；SDK 每阶段有 4 个槽，见 SDL_gpu.h）。
    if (m_lightingUniformSize > 0) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 1, m_lightingUniform.data(),
                                       static_cast<Uint32>(m_lightingUniformSize));
    }
    // 阴影（T21b）：片元 uniform 槽 2。
    if (m_shadowUniformValid) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 2, &m_shadowUniform,
                                       static_cast<Uint32>(sizeof(ShadowUniform)));
    }

    DrawMeshes(pass, meshes, meshCount);

    SDL_EndGPURenderPass(pass);

    // ---- 色调映射通道（T20 / ADR 0010 P0）：HDR 目标 → 交换链 ----
    // 全屏三角形覆盖整个交换链，因此无需保留上一帧内容（DONT_CARE 加载）。
    SDL_GPUColorTargetInfo tonemapTarget {};
    tonemapTarget.texture  = swapchain;
    tonemapTarget.load_op  = SDL_GPU_LOADOP_DONT_CARE;
    tonemapTarget.store_op = SDL_GPU_STOREOP_STORE;  // 叠加层稍后以 LOAD 追加，故必须存储

    // 色调映射片元 uniform（std140：单个 vec4，x = 曝光）；在开渲染通道前推送。
    struct TonemapUniform {
        float exposureParams[4];
    };
    TonemapUniform tonemapUniform {};
    tonemapUniform.exposureParams[0] = m_exposure;
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &tonemapUniform, static_cast<Uint32>(sizeof(tonemapUniform)));

    SDL_GPURenderPass* tonemapPass = SDL_BeginGPURenderPass(commandBuffer, &tonemapTarget, 1, nullptr);
    if (tonemapPass != nullptr) {
        SDL_BindGPUGraphicsPipeline(tonemapPass, m_tonemapPipeline);

        SDL_GPUTextureSamplerBinding hdrBinding { m_hdrTexture, m_hdrSampler };
        SDL_BindGPUFragmentSamplers(tonemapPass, 0, &hdrBinding, 1);

        // 顶点缓冲为空：3 个顶点由 gl_VertexIndex 直接生成。
        SDL_DrawGPUPrimitives(tonemapPass, 3, /*num_instances=*/1, /*first_vertex=*/0, /*first_instance=*/0);
        SDL_EndGPURenderPass(tonemapPass);
    }

    // 叠加层：与 3D 通道共用本命令缓冲（交换链纹理只在获取它的命令缓冲里有效）。
    if (overlay != nullptr) {
        overlay->DrawOverlay(commandBuffer, swapchain, width, height);
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
    return true;
}

}  // namespace vx
