#include "render/mesh_renderer.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"
#include "render/environment.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace vx {
namespace {

// ---- 层间共面重叠的光栅化深度偏移（T78 / 2026-09-29）----
//
// 用途：地表 tile 网格与可挖体积网格在**层间接管边界环**上逐点重合（共面）⇒ 两个面各自栅格化出的
// 深度仅有浮点插值级差异 ⇒ 逐像素随机胜负 ⇒ **z-fighting 闪烁**（世界扩到 1 km 后边界环被搬到地图正中，可见）。
// 业界标准手段 = **光栅化 depth bias / polygon offset**（D3D12 `D3D12_RASTERIZER_DESC::DepthBias`、
// Vulkan `VkPipelineRasterizationStateCreateInfo::depthBiasConstantFactor`、UE `FMeshPassProcessor` 的
// `DepthBias`）：给**地表**这一层一个**正**偏移（把它的深度往"更远"推）⇒ 共面处**体积面稳定胜出**、
// 不改几何、不留缝。
//
// 取值说明（**待实测微调的经验值**）：SDL3_gpu 把 `depth_bias_constant_factor` 原样映射到后端，
// 而后端会**再乘以该深度格式的最小可分辨差 `r`**（float32 深度 `r ≈ 2^-23`），且 **D3D12 后端会把它
// 取整**（`SDL_gpu_d3d12.c` 的 `SDL_lroundf`）⇒ 形如 `1e-3` 的浮点量级在本机 D3D12 上会被取整成 **0**、
// 完全失效。故这里给**整数值** `100`（等效归一化深度偏移 ≈ `100 × 2^-23 ≈ 1.2e-5`），配合 slope factor
// 覆盖掠射角；量级目标 = "稳定压过共面差异、又不产生可察觉台阶"。**本机实测若仍闪 ⇒ 适度上调，
// 若地表相对体积出现可见下沉 ⇒ 下调**（对应 devlog 2026-09-29 T78 条目的"待实测微调"）。
inline constexpr float kSurfaceDepthBiasConstant = 100.0F;
inline constexpr float kSurfaceDepthBiasSlope    = 1.0F;
inline constexpr float kSurfaceDepthBiasClamp    = 0.0F;

// ---- 实例化（V0.7 H1 / ADR 0034）----
//
// 每个实例在 GPU 侧占 `mat4 modelToRender` + 两个 `vec4`（围合体代理，V0.8）= 96 字节
// （std430 下 `mat4` 无隐式填充、两个 `vec4` 紧随其后 ⇒ 数组步长 96）。
// 口径的**唯一事实来源** = `instance_batch.hpp` 的 `kInstancePoseBytes`（与着色器结构体逐字节对应）。
inline constexpr std::uint32_t kInstanceTransformBytes = vx::kInstancePoseBytes;

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

/// **只创建**（不上传、不做任何同步等待）一个 GPU 缓冲；失败抛 `std::runtime_error`。
///
/// T75：供"先按容量建缓冲、再走 `UpdateMeshGeometry` 提交即走"的新路径使用 ——
/// 旧路径（`create_and_upload_buffer`）每次上传都要额外建一个 transfer buffer 并**等一次 fence**，
/// 对"每帧都要重建若干网格"的流式/破坏路径是不可接受的成本（见 `references/performance-and-hitches.md` §1.4）。
[[nodiscard]] SDL_GPUBuffer* create_buffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage,
                                           std::uint32_t size) {
    SDL_GPUBufferCreateInfo bufferInfo {};
    bufferInfo.usage = usage;
    bufferInfo.size  = size;

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &bufferInfo);
    if (buffer == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateGPUBuffer 失败：") + SDL_GetError());
    }
    return buffer;
}

/// 创建一张 2D 纹理并**同步上传第 0 级**（mip 级数 = 1），返回纹理句柄；失败返回 nullptr。
///
/// 与 `CreateTextureArray` 同模式（阻塞到 GPU 完成）：**只在加载期**调用（T67 的 HDRI 与占位纹理）。
/// 不做异常：环境贴图是"可失败、失败即回落"的路径（ADR 0021），由调用方决定报错还是 WARN。
[[nodiscard]] SDL_GPUTexture* create_and_upload_texture_2d(SDL_GPUDevice* device, SDL_GPUTextureFormat format,
                                                          std::uint32_t width, std::uint32_t height,
                                                          SDL_GPUTextureUsageFlags usage, const void* pixels,
                                                          std::uint32_t sourceBytes) {
    SDL_GPUTextureCreateInfo textureInfo {};
    textureInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.format               = format;
    textureInfo.usage                = usage;
    textureInfo.width                = width;
    textureInfo.height               = height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels           = 1;
    textureInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &textureInfo);
    if (texture == nullptr) {
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size  = sourceBytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (transfer == nullptr) {
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, /*cycle=*/false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    std::memcpy(mapped, pixels, sourceBytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTextureTransferInfo source { transfer, 0, 0, 0 };
    SDL_GPUTextureRegion       destination { texture, /*mip_level=*/0, /*layer=*/0, 0, 0, 0, width, height, /*d=*/1 };
    SDL_UploadToGPUTexture(copyPass, &source, &destination, /*cycle=*/false);
    SDL_EndGPUCopyPass(copyPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(device, /*wait_all=*/true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return texture;
}

/// 把等距柱状 HDRI（线性 float32 RGB、行主序、第 0 行 = 天顶）转成 `R16G16B16A16_FLOAT` 的上传字节。
///
/// 为什么转半精度（T67 / ADR 0021 的显存后果）：二进制的 HDRI 只用于**采样**、
/// 半精度对辐照度足够（动态范围 ±65504、相对精度约 1e-3），而字节数减半 ——
/// 2048×1024 由 33.5 MB 降到 16.8 MB，直接决定是否守得住 ADR 0008 的 300 MB 预算。
[[nodiscard]] std::vector<std::uint16_t> ConvertEquirectRgb32fToRgba16f(const EnvironmentSource& source) {
    const std::size_t pixelCount = static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height);
    std::vector<std::uint16_t> out(pixelCount * 4U);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        out[pixel * 4U + 0U] = HalfFromFloat(source.pixels[pixel * 3U + 0U]);
        out[pixel * 4U + 1U] = HalfFromFloat(source.pixels[pixel * 3U + 1U]);
        out[pixel * 4U + 2U] = HalfFromFloat(source.pixels[pixel * 3U + 2U]);
        out[pixel * 4U + 3U] = HalfFromFloat(1.0F);  // A：采样用不到，填 1 保持"不透明白"
    }
    return out;
}

}  // namespace

MeshRenderer::MeshRenderer(SDL_GPUDevice* device, SDL_Window* window, std::filesystem::path shader_dir,
                           std::string shader_name)
    : m_device(device), m_window(window) {
    const ShaderArtifact artifact = select_shader_artifact(m_device);
    const std::string    extension = artifact.extension;

    // 顶点着色器：1 个只读 storage buffer（相机常量，set 0）
    //             + **1 个 uniform 块**（T41：逐网格顶点偏移，set 1，逐网格 `SDL_PushGPUVertexUniformData`）。
    // 数量必须与 SPIR-V 里声明的资源一致（SDL3_gpu 的 set 约定：顶点 set 0 = 只读资源、set 1 = uniform）。
    // 注意：T23 起把两个 Shader **持有到析构**（而非创建管线后立即释放）——MSAA 档位变化时
    // 需按新的采样数**重建主通道管线**，重建要复用同一批 Shader 对象，避免重新读盘。
    m_meshVertexShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".vert" + extension),
                                SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                ShaderResourceCounts { 0, 0, /*storageBuffers=*/1, /*uniformBuffers=*/1 });
    // 片元着色器：6 个采样纹理（slot 0..4 = albedo / normal / roughness / AO / macro，slot 5 = 阴影深度数组）
    //              + 4 个 uniform 块（slot 0 材质、slot 1 光照、slot 2 阴影、**slot 3 自发光**，T27）。
    // SDL_gpu 每阶段采样器上限为 16（MAX_TEXTURE_SAMPLERS_PER_STAGE），6 个无需把 roughness/AO 打包进通道；
    // uniform 槽上限为 4（`MAX_UNIFORM_BUFFERS_PER_STAGE`）——**声明的个数必须与着色器里 `set = 3` 的
    // binding 数一致**，否则 `SDL_CreateGPUGraphicsPipeline` 会以 E_INVALIDARG 失败（T27 实测踩到）。
    m_meshFragmentShader =
        create_shader_from_file(m_device, shader_dir / (shader_name + ".frag" + extension),
                                SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                ShaderResourceCounts { /*samplers=*/9, 0, 0, /*uniformBuffers=*/4 });

    // T69：**蒙皮**顶点着色器 —— 与 mesh.vert 同源，但声明 **2 个**顶点 storage buffer
    // （binding 0 = 相机、binding 1 = 骨骼矩阵数组）并多消费关节索引 / 权重；片元阶段仍复用 mesh.frag。
    // 常驻到析构（与 `m_meshVertexShader` 同理由：蒙皮主通道管线随 MSAA 档位重建）。
    m_skinnedVertexShader =
        create_shader_from_file(m_device, shader_dir / ("mesh_skinned.vert" + extension), SDL_GPU_SHADERSTAGE_VERTEX,
                                artifact.format, ShaderResourceCounts { 0, 0, /*storageBuffers=*/2, /*uniformBuffers=*/1 });

    // V0.7 H1：**实例化**顶点着色器 —— 与 mesh.vert 同源，但逐实例 `modelToRender` 来自
    // set 0 / binding 1 的只读 storage buffer（`mat4[]`，用 `gl_InstanceIndex` 索引）⇒ 2 个顶点 storage buffer
    // （binding 0 = 相机、binding 1 = 实例变换）。常驻到析构（理由同 `m_meshVertexShader`：实例化主通道管线
    // 要随 MSAA 档位重建）。片元阶段仍复用 `mesh.frag`。
    m_instancedVertexShader =
        create_shader_from_file(m_device, shader_dir / ("mesh_instanced.vert" + extension), SDL_GPU_SHADERSTAGE_VERTEX,
                                artifact.format, ShaderResourceCounts { 0, 0, /*storageBuffers=*/2, /*uniformBuffers=*/1 });

    // 全屏三角的**顶点阶段**（T20 的 `tonemap.vert`）：色调映射、天空（T67）与三条 IBL 烘焙管线共用。
    // 常驻到析构 —— 天空管线要随 MSAA 档位重建，重建时复用同一个 Shader 对象（与 m_meshVertexShader 同理由）。
    m_fullscreenVertexShader =
        create_shader_from_file(m_device, shader_dir / ("tonemap.vert" + extension), SDL_GPU_SHADERSTAGE_VERTEX,
                                artifact.format, ShaderResourceCounts {});

    // 天空的片元阶段（T67）：1 个采样器（等距柱状 HDRI）+ 1 个 uniform 块（逆视图投影）。
    // 常驻（原因同上：天空管线随 MSAA 档位重建）。
    m_skyFragmentShader =
        create_shader_from_file(m_device, shader_dir / ("sky.frag" + extension), SDL_GPU_SHADERSTAGE_FRAGMENT,
                                artifact.format, ShaderResourceCounts { /*samplers=*/1, 0, 0, /*uniformBuffers=*/1 });

    // W6：**水面**片元阶段（`water.frag`）—— 1 个 uniform 块（槽 0 = 水面参数：时间 / 渲染原点），无采样器。
    // 顶点阶段**复用** `m_meshVertexShader`（同一顶点布局与逐网格变换）。常驻到析构（同 `m_meshVertexShader` 理由）。
    m_waterFragmentShader =
        create_shader_from_file(m_device, shader_dir / ("water.frag" + extension), SDL_GPU_SHADERSTAGE_FRAGMENT,
                                artifact.format, ShaderResourceCounts { /*samplers=*/0, 0, 0, /*uniformBuffers=*/1 });

    // 主通道管线：先按单采样创建（`SetMsaaSampleCount` 通常在构造之后调用；档位变化时
    // `EnsureMainPipeline` 用同一批 Shader 重建），保证构造期即验证"设备 + 管线 + Shader"链路。
    // T67 起它**同时**创建天空管线（两者必须在同一个渲染通道里共存 ⇒ 采样数必须一致）。
    CreateMainPipeline(1);

    // ---- 色调映射管线（T20 / ADR 0010 P0）：HDR 目标 → 交换链 ----
    // 全屏三角形（顶点缓冲为空，位置由 gl_VertexIndex 生成）；无深度目标、sample_count = 1、不剔除。
    {
        SDL_GPUShader* tonemapFragment =
            create_shader_from_file(m_device, shader_dir / ("tonemap.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/1, 0, 0, /*uniformBuffers=*/1 });

        m_tonemapPipeline = CreateFullscreenPipeline(tonemapFragment, SDL_GetGPUSwapchainTextureFormat(m_device, m_window),
                                                    /*withDepthStencil=*/false, /*sampleCount=*/1, "色调映射");
        SDL_ReleaseGPUShader(m_device, tonemapFragment);

        if (m_tonemapPipeline == nullptr) {
            throw std::runtime_error(std::string("创建色调映射管线失败：") + SDL_GetError());
        }
    }

    // ---- IBL 烘焙管线（T67 / ADR 0021）：三条全屏三角、无深度目标、采样数恒为 1 ----
    // 在构造期创建（而非首次烘焙时）：管线创建**不得**出现在渲染热路径，且这样"GLSL 编译错误"在启动即暴露。
    {
        SDL_GPUShader* irradianceFragment =
            create_shader_from_file(m_device, shader_dir / ("ibl_irradiance.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/1, 0, 0, /*uniformBuffers=*/0 });
        m_irradiancePipeline = CreateFullscreenPipeline(irradianceFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                       /*withDepthStencil=*/false, /*sampleCount=*/1,
                                                       "IBL 漫反射 irradiance");
        SDL_ReleaseGPUShader(m_device, irradianceFragment);

        SDL_GPUShader* prefilterFragment =
            create_shader_from_file(m_device, shader_dir / ("ibl_prefilter.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/1, 0, 0, /*uniformBuffers=*/1 });
        m_prefilterPipeline = CreateFullscreenPipeline(prefilterFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                      /*withDepthStencil=*/false, /*sampleCount=*/1,
                                                      "IBL 预过滤高光");
        SDL_ReleaseGPUShader(m_device, prefilterFragment);

        // BRDF LUT：无任何输入（解析拟合，见 ibl_brdf_lut.frag 顶部说明）⇒ 0 采样器、0 uniform 块。
        SDL_GPUShader* brdfLutFragment =
            create_shader_from_file(m_device, shader_dir / ("ibl_brdf_lut.frag" + extension),
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, artifact.format, ShaderResourceCounts {});
        m_brdfLutPipeline = CreateFullscreenPipeline(brdfLutFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                    /*withDepthStencil=*/false, /*sampleCount=*/1, "IBL BRDF LUT");
        SDL_ReleaseGPUShader(m_device, brdfLutFragment);

        if (m_irradiancePipeline == nullptr || m_prefilterPipeline == nullptr || m_brdfLutPipeline == nullptr) {
            throw std::runtime_error(std::string("创建 IBL 烘焙管线失败：") + SDL_GetError());
        }
    }

    // ---- 阴影深度管线（T21b / ADR 0010 P1）：无颜色目标，仅写深度 ----
    {
        SDL_GPUShader* shadowVertex =
            create_shader_from_file(m_device, shader_dir / ("shadow.vert" + extension),
                                    SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                    ShaderResourceCounts { /*samplers=*/0, 0, /*storageBuffers=*/1,
                                                           /*uniformBuffers=*/1 });  // set 1 = 逐网格偏移（T41）
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
        if (m_shadowPipeline == nullptr) {
            throw std::runtime_error(std::string("创建阴影深度管线失败：") + SDL_GetError());
        }

        // T69：**蒙皮阴影管线**（顶点布局 = `SkinnedVertex`；着色器声明 2 个 storage buffer：
        // binding 0 = 本级光空间矩阵、binding 1 = 骨骼矩阵）。除了顶点布局与着色器，其余状态与
        // `m_shadowPipeline` 完全相同。**为什么必须补这一条**：主角胶囊原本会投影；若蒙皮网格在
        // 阴影通道缺席，主角就会"影子消失" —— 那是**可见回退**（见 docs/plans/v0.3.md §1.3）。
        m_shadowSkinnedVertexShader =
            create_shader_from_file(m_device, shader_dir / ("shadow_skinned.vert" + extension),
                                    SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                    ShaderResourceCounts { 0, 0, /*storageBuffers=*/2, /*uniformBuffers=*/1 });
        SDL_GPUVertexBufferDescription skinnedShadowVertexBuffer {};
        skinnedShadowVertexBuffer.slot               = 0;
        skinnedShadowVertexBuffer.pitch              = static_cast<Uint32>(sizeof(SkinnedVertex));
        skinnedShadowVertexBuffer.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        skinnedShadowVertexBuffer.instance_step_rate = 0;
        // 只消费位置（法线不参与深度写入）——但关节索引 / 权重必须声明，否则蒙皮算不出来。
        const SDL_GPUVertexAttribute skinnedShadowAttribute[] = {
            { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(SkinnedVertex, position)) },
            { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, static_cast<Uint32>(offsetof(SkinnedVertex, joints)) },
            { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<Uint32>(offsetof(SkinnedVertex, weights)) },
        };
        SDL_GPUVertexInputState skinnedShadowVertexInput {};
        skinnedShadowVertexInput.vertex_buffer_descriptions = &skinnedShadowVertexBuffer;
        skinnedShadowVertexInput.num_vertex_buffers         = 1;
        skinnedShadowVertexInput.vertex_attributes          = skinnedShadowAttribute;
        skinnedShadowVertexInput.num_vertex_attributes      = static_cast<Uint32>(std::size(skinnedShadowAttribute));

        SDL_GPUGraphicsPipelineCreateInfo skinnedShadowInfo = shadowInfo;
        skinnedShadowInfo.vertex_shader                     = m_shadowSkinnedVertexShader;
        skinnedShadowInfo.vertex_input_state                = skinnedShadowVertexInput;
        m_shadowSkinnedPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &skinnedShadowInfo);
        if (m_shadowSkinnedPipeline == nullptr) {
            throw std::runtime_error(std::string("创建蒙皮阴影管线失败：") + SDL_GetError());
        }

        // V0.7 H1：**实例化阴影管线** —— 与 `m_shadowPipeline` 只差顶点着色器（`shadow_instanced.vert`，
        // 声明 2 个顶点 storage buffer：binding 0 = 本级光空间矩阵、binding 1 = 实例变换）。
        // 顶点布局与 `shadow.vert` **相同**（只消费位置）⇒ 直接复用 `shadowVertexInput`；
        // 阴影目标恒为单采样 ⇒ 构造期创建一次（不随 MSAA 档位变化）。**为什么必须补这一条**：
        // 实例化物件在主通道可见，若阴影通道缺席就会"影子消失"——那是**可见回退**（同 T69 的蒙皮理由）。
        m_shadowInstancedVertexShader =
            create_shader_from_file(m_device, shader_dir / ("shadow_instanced.vert" + extension),
                                    SDL_GPU_SHADERSTAGE_VERTEX, artifact.format,
                                    ShaderResourceCounts { 0, 0, /*storageBuffers=*/2, /*uniformBuffers=*/1 });
        SDL_GPUGraphicsPipelineCreateInfo instancedShadowInfo = shadowInfo;
        instancedShadowInfo.vertex_shader                     = m_shadowInstancedVertexShader;
        instancedShadowInfo.vertex_input_state                = shadowVertexInput;  // 只消费位置，与 shadow.vert 一致
        m_shadowInstancedPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &instancedShadowInfo);
        if (m_shadowInstancedPipeline == nullptr) {
            throw std::runtime_error(std::string("创建实例化阴影管线失败：") + SDL_GetError());
        }
        // **顺序要紧**：两条阴影管线都从 `shadowVertex` / `shadowFragment` 建好之后才能释放它们。
        // （SDL_gpu 不持有 shader 引用 ⇒ 先释放再用就是 use-after-free：实测会触发 D3D12 后端的
        //   `CreateGraphicsPipeline was passed a vertex shader for the fragment stage` 断言。）
        SDL_ReleaseGPUShader(m_device, shadowVertex);
        SDL_ReleaseGPUShader(m_device, shadowFragment);
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

    // 环境贴图采样器（T67 / ADR 0021）：**U 重复 / V,W 钳制** + 线性过滤 + mipmap 线性。
    // 为什么 U 重复：等距柱状贴图在方位角方向首尾相接（u = 0 与 u = 1 是同一方向）；
    // V 必须钳制：两极为奇点，重复会跨到对侧。mipmap 线性供运行时 `textureLod` 在预过滤的级间过渡。
    SDL_GPUSamplerCreateInfo environmentSamplerInfo {};
    environmentSamplerInfo.min_filter     = SDL_GPU_FILTER_LINEAR;
    environmentSamplerInfo.mag_filter     = SDL_GPU_FILTER_LINEAR;
    environmentSamplerInfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    environmentSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    environmentSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    environmentSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    environmentSamplerInfo.min_lod        = 0.0F;
    // 上限 = 预过滤贴图的最大 mip 级号（运行期 `textureLod` 的 lod 上限）；天空与 irradiance 只有 1 级，
    // 硬件会把 lod 钳到实际级数，故同一个采样器可安全服务三张贴图。
    environmentSamplerInfo.max_lod        = static_cast<float>(kEnvironmentPrefilterLodMax);
    m_environmentSampler = SDL_CreateGPUSampler(m_device, &environmentSamplerInfo);
    if (m_environmentSampler == nullptr) {
        throw std::runtime_error(std::string("创建环境贴图采样器失败：") + SDL_GetError());
    }

    // 1×1 占位纹理（白）：`mesh.frag` 恒声明 binding 6..8 三个采样器，SDL_gpu 要求声明的采样器都有绑定，
    // 故环境贴图未就绪时用它占位（此时 uniform 的 IBL 启用位为 0，着色器整段跳过采样 ⇒ 内容无意义）。
    {
        const std::uint16_t whiteHalf = HalfFromFloat(1.0F);
        const std::uint16_t whitePixel[4] = { whiteHalf, whiteHalf, whiteHalf, whiteHalf };
        m_environmentPlaceholder =
            create_and_upload_texture_2d(m_device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 1, 1,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER, whitePixel,
                                         static_cast<std::uint32_t>(sizeof(whitePixel)));
        if (m_environmentPlaceholder == nullptr) {
            throw std::runtime_error(std::string("创建环境贴图占位纹理失败：") + SDL_GetError());
        }
        // 记账：4 × 2 字节 = 8 字节（虽小，但"全部纹理都记账"才让总量可核对）。
        // 刻意**不计入** `m_environmentBytes`：那个量是"四张环境贴图之和"（`EnvironmentTextureBytes`），
        // 占位纹理常驻到析构、不随 `ReleaseEnvironmentTextures` 释放。
        m_stats.textureBytes += 8ULL;
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
        // location 2：材质槽位覆盖（ADR 0014）。地表网格填 kNoMaterialOverride（由片元按高度/坡度算权重）。
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, static_cast<Uint32>(offsetof(MeshVertex, material)) },
        // location 3：morph 目标高度（W7-S3b，CDLOD 顶点过渡消接缝，ADR 0024）。**水面管线复用同一份
        // attributes**（其顶点着色器同为 `m_meshVertexShader`）⇒ 自动生效。**阴影通道不追加**本项：
        // `shadow.vert` 只声明 location 0，且阴影投射体都在 Ring 0 内、morph 因子恒为 0（morph 只在 Ring 1/2
        // 逐步推进），故不追加不丢任何信息。
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, static_cast<Uint32>(offsetof(MeshVertex, morph)) },
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

    // T78：**带光栅化深度偏移的主通道变体**（与 `m_pipeline` 只差 `rasterizer_state` 的深度偏移）。
    // 与 `m_pipeline` 在同一处（这里）创建 / 重建 ⇒ **绝不出现在 `RenderFrame` 热路径**（SKILL 硬规则 4）。
    if (m_pipelineDepthBiased != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipelineDepthBiased);
        m_pipelineDepthBiased = nullptr;
    }
    info.rasterizer_state.enable_depth_bias            = true;
    info.rasterizer_state.depth_bias_constant_factor   = kSurfaceDepthBiasConstant;
    info.rasterizer_state.depth_bias_slope_factor      = kSurfaceDepthBiasSlope;
    info.rasterizer_state.depth_bias_clamp             = kSurfaceDepthBiasClamp;
    m_pipelineDepthBiased = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    if (m_pipelineDepthBiased == nullptr) {
        m_pipelineSampleCount = 0;
        throw std::runtime_error(std::string("创建带深度偏移的主通道管线失败：") + SDL_GetError());
    }

    // T69：**蒙皮主通道管线** —— 顶点布局换 `SkinnedVertex`、顶点着色器换 `mesh_skinned.vert`
    // （声明 2 个顶点 storage buffer），其余（片元着色器 / 目标格式 / 采样数 / 剔除 / 深度状态）与
    // `m_pipeline` 完全相同，故与它**同生共死**（档位变化时一起重建）。**绝不在 `RenderFrame` 里创建**。
    if (m_skinnedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_skinnedPipeline);
        m_skinnedPipeline = nullptr;
    }
    SDL_GPUVertexBufferDescription skinnedVertexBufferDescription {};
    skinnedVertexBufferDescription.slot               = 0;
    skinnedVertexBufferDescription.pitch              = static_cast<Uint32>(sizeof(SkinnedVertex));
    skinnedVertexBufferDescription.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    skinnedVertexBufferDescription.instance_step_rate = 0;

    const SDL_GPUVertexAttribute skinnedAttributes[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(SkinnedVertex, position)) },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<Uint32>(offsetof(SkinnedVertex, normal)) },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, static_cast<Uint32>(offsetof(SkinnedVertex, joints)) },
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<Uint32>(offsetof(SkinnedVertex, weights)) },
    };

    SDL_GPUVertexInputState skinnedVertexInput {};
    skinnedVertexInput.vertex_buffer_descriptions = &skinnedVertexBufferDescription;
    skinnedVertexInput.num_vertex_buffers         = 1;
    skinnedVertexInput.vertex_attributes          = skinnedAttributes;
    skinnedVertexInput.num_vertex_attributes      = static_cast<Uint32>(std::size(skinnedAttributes));

    SDL_GPUGraphicsPipelineCreateInfo skinnedInfo = info;
    skinnedInfo.vertex_shader                     = m_skinnedVertexShader;
    skinnedInfo.vertex_input_state                = skinnedVertexInput;
    // `info` 上一步被改成了"带深度偏移"变体 ⇒ 这里必须**显式关掉**，否则蒙皮网格会继承该偏移。
    skinnedInfo.rasterizer_state.enable_depth_bias = false;
    m_skinnedPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &skinnedInfo);
    if (m_skinnedPipeline == nullptr) {
        m_pipelineSampleCount = 0;
        throw std::runtime_error(std::string("创建蒙皮主通道管线失败：") + SDL_GetError());
    }

    // V0.7 H1：**实例化主通道管线** —— 顶点布局 / 片元着色器 / 目标格式 / 采样数 / 剔除 / 深度状态与
    // `m_pipeline` 完全相同，只把顶点着色器换成 `mesh_instanced.vert`（声明 2 个顶点 storage buffer）。
    // 与 `m_pipeline` **同生共死**（随 MSAA 档位在此重建），**绝不**出现在 `RenderFrame` 热路径。
    if (m_instancedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_instancedPipeline);
        m_instancedPipeline = nullptr;
    }
    SDL_GPUGraphicsPipelineCreateInfo instancedInfo = info;
    instancedInfo.vertex_shader                     = m_instancedVertexShader;
    instancedInfo.vertex_input_state                = vertexInput;  // 与 MeshVertex 一致（物件的顶点布局同地表）
    // `info` 上一步被改成了"带深度偏移"变体 ⇒ 这里必须**显式关掉**，否则物件会继承该偏移。
    instancedInfo.rasterizer_state.enable_depth_bias = false;
    m_instancedPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &instancedInfo);
    if (m_instancedPipeline == nullptr) {
        m_pipelineSampleCount = 0;
        throw std::runtime_error(std::string("创建实例化主通道管线失败：") + SDL_GetError());
    }

    // W6：**水面管线**（顶点 = `m_meshVertexShader`、片元 = `water.frag`）—— 与主通道同生共死（随 MSAA 档位重建）。
    // 与主通道的差异（都是"水面"语义所必需）：① **alpha 混合**（半透明）；② **关闭背面剔除**
    // （水面 ribbon 从上下看都要可见）；③ **关闭深度写入**（水面不得遮挡其后的地形）。深度测试仍开启。
    if (m_waterPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_waterPipeline);
        m_waterPipeline = nullptr;
    }
    {
        SDL_GPUGraphicsPipelineCreateInfo waterInfo = info;
        waterInfo.fragment_shader             = m_waterFragmentShader;
        waterInfo.vertex_input_state          = vertexInput;  // 与 MeshVertex 一致（复用 mesh.vert）
        waterInfo.rasterizer_state.enable_depth_bias = false;  // 不要继承上一步的"带深度偏移"变体
        waterInfo.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_NONE;
        waterInfo.depth_stencil_state.enable_depth_write = false;

        SDL_GPUColorTargetDescription waterColor = colorTargetDescription;
        waterColor.blend_state.enable_blend             = true;
        waterColor.blend_state.src_color_blendfactor    = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        waterColor.blend_state.dst_color_blendfactor    = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        waterColor.blend_state.color_blend_op           = SDL_GPU_BLENDOP_ADD;
        waterColor.blend_state.src_alpha_blendfactor    = SDL_GPU_BLENDFACTOR_ONE;
        waterColor.blend_state.dst_alpha_blendfactor    = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        waterColor.blend_state.alpha_blend_op           = SDL_GPU_BLENDOP_ADD;
        waterInfo.target_info.color_target_descriptions = &waterColor;

        m_waterPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &waterInfo);
        if (m_waterPipeline == nullptr) {
            m_pipelineSampleCount = 0;
            throw std::runtime_error(std::string("创建水面管线失败：") + SDL_GetError());
        }
    }

    // T67：天空管线与主通道**共用同一个渲染通道**（天空先画、网格覆盖其上）⇒ 采样数必须与目标一致，
    // 因此与主通道同生共死：这里一并（重）建，`EnsureMainPipeline` 的两个判断即覆盖两者。
    if (m_skyPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_skyPipeline);
        m_skyPipeline = nullptr;
    }
    m_skyPipeline = CreateFullscreenPipeline(m_skyFragmentShader, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                             /*withDepthStencil=*/true, sampleCount, "天空");
    if (m_skyPipeline == nullptr) {
        m_pipelineSampleCount = 0;
        throw std::runtime_error(std::string("创建天空管线失败：") + SDL_GetError());
    }
}

SDL_GPUGraphicsPipeline* MeshRenderer::CreateFullscreenPipeline(SDL_GPUShader* fragmentShader,
                                                               SDL_GPUTextureFormat colorFormat,
                                                               bool withDepthStencil, std::uint32_t sampleCount,
                                                               const char* label) {
    SDL_GPUColorTargetDescription colorTarget {};
    colorTarget.format = colorFormat;

    SDL_GPUGraphicsPipelineCreateInfo info {};
    info.vertex_shader   = m_fullscreenVertexShader;
    info.fragment_shader = fragmentShader;
    // 无顶点输入：位置由 `gl_VertexIndex` 在顶点着色器内生成（见 tonemap.vert）。
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = false;  // 全屏三角恒在 [0,1] 之外超界，不裁

    info.multisample_state.sample_count = ToSdlSampleCount(sampleCount);

    if (withDepthStencil) {
        // 天空：与主通道同处一个渲染通道（该通道**有**深度目标）⇒ 必须声明深度格式；
        // 但**关闭深度测试与写入** ⇒ 天空不遮挡、也不被任何几何遮挡（网格永远画在天空之上）。
        info.depth_stencil_state.compare_op          = SDL_GPU_COMPAREOP_ALWAYS;
        info.depth_stencil_state.enable_depth_test   = false;
        info.depth_stencil_state.enable_depth_write  = false;
        info.depth_stencil_state.enable_stencil_test = false;
        info.target_info.depth_stencil_format        = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target    = true;
    } else {
        info.target_info.has_depth_stencil_target = false;
    }

    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets         = 1;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    if (pipeline == nullptr) {
        VX_LOG_ERROR("创建全屏管线失败（%s）：%s", label, SDL_GetError());
    }
    return pipeline;
}

void MeshRenderer::DrawFullscreenPass(SDL_GPUCommandBuffer* commandBuffer, const SDL_GPUColorTargetInfo& colorTarget,
                                      SDL_GPUGraphicsPipeline* pipeline,
                                      const SDL_GPUTextureSamplerBinding* samplers, std::uint32_t samplerCount,
                                      const void* fragmentUniform, std::uint32_t fragmentUniformBytes) {
    if (pipeline == nullptr) {
        return;
    }
    // uniform 在开渲染通道前推送（对后续绘制持续生效；本通道只画这一个全屏三角，故无需去重）。
    if (fragmentUniform != nullptr && fragmentUniformBytes > 0) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, fragmentUniform, fragmentUniformBytes);
    }

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
    if (pass == nullptr) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (samplers != nullptr && samplerCount > 0) {
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, samplerCount);
    }
    // 顶点缓冲为空：3 个顶点由 gl_VertexIndex 直接生成。
    SDL_DrawGPUPrimitives(pass, 3, /*num_instances=*/1, /*first_vertex=*/0, /*first_instance=*/0);
    SDL_EndGPURenderPass(pass);
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
        // T69 / V0.7 H1：蒙皮网格的骨骼矩阵缓冲与实例化原型的实例缓冲（非对应类型恒为 nullptr）。
        if (resources.boneMatrixBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(m_device, resources.boneMatrixBuffer);
        }
        if (resources.instanceBuffer != nullptr) {
            SDL_ReleaseGPUBuffer(m_device, resources.instanceBuffer);
        }
    }
    m_stats.meshBytes = 0;  // V0.7 H0：记账随资源一并归零
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
    // T67：环境贴图（四张，同步显存记账）与常驻的 1×1 占位纹理。
    ReleaseEnvironmentTextures();
    if (m_environmentPlaceholder != nullptr) {
        SDL_ReleaseGPUTexture(m_device, m_environmentPlaceholder);
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
    if (m_indexStagingBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_indexStagingBuffer);
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
    if (m_environmentSampler != nullptr) {
        SDL_ReleaseGPUSampler(m_device, m_environmentSampler);
    }
    if (m_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    }
    if (m_pipelineDepthBiased != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipelineDepthBiased);
    }
    // T69：蒙皮主通道管线（与 m_pipeline 同生共死）与蒙皮阴影管线。
    if (m_skinnedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_skinnedPipeline);
    }
    if (m_shadowSkinnedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_shadowSkinnedPipeline);
    }
    // V0.7 H1：实例化主通道管线（与 m_pipeline 同生共死）与实例化阴影管线。
    if (m_instancedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_instancedPipeline);
    }
    if (m_shadowInstancedPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_shadowInstancedPipeline);
    }
    if (m_tonemapPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_tonemapPipeline);
    }
    if (m_shadowPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_shadowPipeline);
    }
    if (m_skyPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_skyPipeline);
    }
    if (m_irradiancePipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_irradiancePipeline);
    }
    if (m_prefilterPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_prefilterPipeline);
    }
    if (m_brdfLutPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_brdfLutPipeline);
    }
    // 主通道 Shader 常驻到析构（T23）：必须在**使用它们的管线**销毁之后再释放。
    if (m_waterPipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_waterPipeline);
    }
    if (m_meshVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_meshVertexShader);
    }
    if (m_meshFragmentShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_meshFragmentShader);
    }
    if (m_waterFragmentShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_waterFragmentShader);
    }
    // T69：蒙皮顶点着色器（主通道 + 阴影通道各一条）同样常驻到析构。
    if (m_skinnedVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_skinnedVertexShader);
    }
    if (m_shadowSkinnedVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_shadowSkinnedVertexShader);
    }
    // V0.7 H1：实例化顶点着色器（主通道 + 阴影通道各一条）同样常驻到析构。
    if (m_instancedVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_instancedVertexShader);
    }
    if (m_shadowInstancedVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_shadowInstancedVertexShader);
    }
    // 骨骼矩阵上传的常驻暂存缓冲（若有）。
    if (m_boneStagingBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_boneStagingBuffer);
    }
    // V0.7 H1：实例位姿上传的常驻暂存缓冲（若有）。
    if (m_instanceStagingBuffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_instanceStagingBuffer);
    }
    // T67：全屏三角的顶点 / 天空片元阶段同样常驻到析构（天空管线随 MSAA 档位重建）。
    if (m_fullscreenVertexShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_fullscreenVertexShader);
    }
    if (m_skyFragmentShader != nullptr) {
        SDL_ReleaseGPUShader(m_device, m_skyFragmentShader);
    }
}

MeshHandle MeshRenderer::UploadMesh(const MeshData& mesh, const glm::dvec3& origin, bool emissive,
                                   std::uint32_t reserveVertexCount, std::uint32_t reserveIndexCount,
                                   bool depthBiased, bool water) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return MeshHandle {};
    }

    const std::uint32_t vertexCapacity =
        std::max(static_cast<std::uint32_t>(mesh.vertices.size()), reserveVertexCount);
    const std::uint32_t indexCapacity = std::max(static_cast<std::uint32_t>(mesh.indices.size()), reserveIndexCount);

    MeshResources resources;
    resources.vertexBuffer = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_VERTEX,
                                           vertexCapacity * static_cast<std::uint32_t>(sizeof(MeshVertex)));
    resources.indexBuffer  = create_buffer(
        m_device, SDL_GPU_BUFFERUSAGE_INDEX, indexCapacity * static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    // 登记的容量 = 实际建出来的缓冲大小 ⇒ 后续 `UpdateMeshGeometry` 只允许写这个前缀之内。
    resources.vertexCount = vertexCapacity;
    resources.indexCount  = indexCapacity;
    // V0.7 H0：显存记账（顶点 + 索引；蒙皮另有骨骼矩阵缓冲）。
    resources.vertexBytes = static_cast<std::uint64_t>(vertexCapacity) * sizeof(MeshVertex);
    resources.indexBytes  = static_cast<std::uint64_t>(indexCapacity) * sizeof(std::uint32_t);
    // 本帧实际绘制索引数由下面的 `UpdateMeshGeometry` 写（T42 的"变长网格"口径）。
    resources.usedIndexCount = 0;
    resources.emissive       = emissive;
    resources.depthBiased    = depthBiased;  // T78：主通道是否走带深度偏移的管线变体（阴影通道不理会）
    resources.water          = water;        // W6：水面网格走独立管线、在主通道最后绘制、不投影阴影
    // 世界原点只作"这块网格在世界哪里"的登记（T41）；绘制时与渲染原点相减得平移量。
    // 存 `double`（红线 6）：偏移在 double 下相减后才落回 float，大坐标也不会丢精度。
    resources.origin[0] = origin.x;
    resources.origin[1] = origin.y;
    resources.origin[2] = origin.z;

    std::uint32_t slot = 0;
    if (!m_freeSlots.empty()) {
        slot                 = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_meshes[slot]       = resources;
    } else {
        slot = static_cast<std::uint32_t>(m_meshes.size());
        m_meshes.push_back(resources);
    }
    const MeshHandle handle { slot + 1 };
    m_stats.meshBytes += resources.vertexBytes + resources.indexBytes;  // V0.7 H0：显存记账

    // **提交即走**（T75）：复用常驻暂存缓冲 + 单命令缓冲，**不建 transfer buffer、不等 fence**。
    // 这一步是"每块 2 次 `SDL_WaitForGPUFences`"的消除点（旧路径见 `create_and_upload_buffer`）。
    if (!UpdateMeshGeometry(handle, mesh, origin)) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadMesh：网格上传失败（容量预留不足或 GPU 命令提交失败）");
    }
    return handle;
}

bool MeshRenderer::EnsureStagingBuffer(SDL_GPUTransferBuffer*& buffer, std::uint32_t& capacity,
                                       std::uint32_t bytes) {
    if (bytes == 0U) {
        return true;  // 没有要上传的字节：保留既有缓冲（也不视为失败）
    }
    if (buffer != nullptr && capacity >= bytes) {
        return true;  // 容量够 ⇒ 不重新分配（稳态零 GPU 资源创建）
    }
    if (buffer != nullptr) {
        SDL_ReleaseGPUTransferBuffer(m_device, buffer);
        buffer = nullptr;
    }
    SDL_GPUTransferBufferCreateInfo stagingInfo {};
    stagingInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    stagingInfo.size  = bytes;
    buffer = SDL_CreateGPUTransferBuffer(m_device, &stagingInfo);
    if (buffer == nullptr) {
        capacity = 0;
        return false;
    }
    capacity = bytes;
    return true;
}

bool MeshRenderer::UpdateMeshVertices(MeshHandle handle, const std::vector<MeshVertex>& vertices,
                                      const glm::dvec3& origin) {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return false;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return false;
    }
    // 顶点数必须与上传时一致，否则既有的 GPU 顶点缓冲装不下（本方法只做就地刷新）。
    if (vertices.size() != static_cast<std::size_t>(resources.vertexCount)) {
        return false;
    }

    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(vertices.size() * sizeof(MeshVertex));
    // 扩容只在首次或网格变大时发生，稳态（定长动态网格）下不触发，故不构成每帧分配。
    if (!EnsureStagingBuffer(m_vertexStagingBuffer, m_vertexStagingCapacity, vertexBytes)) {
        return false;
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
    // 顶点与变换必须**同源**（T41）：调用方按 `origin` 烘焙了这批顶点，这里一并更新登记的原点，
    // 否则渲染原点重定基后平移会与该批顶点错配。失败路径不改原点（顶点也没有被改写）。
    // 这条路径是"渲染相对顶点"的烘焙路径（角色 / 光球）⇒ 旋转恒为**单位**（T33 的刚体走 `SetMeshTransform`）。
    resources.origin[0] = origin.x;
    resources.origin[1] = origin.y;
    resources.origin[2] = origin.z;
    resources.rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    return true;
}

bool MeshRenderer::UpdateMeshGeometry(MeshHandle handle, const MeshData& mesh, const glm::dvec3& origin) {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return false;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr || resources.indexBuffer == nullptr) {
        return false;
    }
    // 容量是上传时定死的（池槽位不重建）⇒ 超出即拒绝，由调用方决定截断还是跳过。
    if (mesh.vertices.size() > static_cast<std::size_t>(resources.vertexCount) ||
        mesh.indices.size() > static_cast<std::size_t>(resources.indexCount)) {
        return false;
    }

    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(MeshVertex));
    const std::uint32_t indexBytes  = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));

    // 更新登记的位姿（与顶点同源）：本路径的顶点是**网格局部坐标**，旋转由 `SetMeshTransform` 每帧推。
    resources.origin[0] = origin.x;
    resources.origin[1] = origin.y;
    resources.origin[2] = origin.z;
    resources.rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);

    if (indexBytes == 0U) {
        // 空网格 = **不可见**：不改缓冲、不做任何上传（"清空一个倒塌槽位"因此是零成本）。
        resources.usedIndexCount = 0;
        return true;
    }

    if (!EnsureStagingBuffer(m_vertexStagingBuffer, m_vertexStagingCapacity, vertexBytes) ||
        !EnsureStagingBuffer(m_indexStagingBuffer, m_indexStagingCapacity, indexBytes)) {
        return false;
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_vertexStagingBuffer, /*cycle=*/true);
    if (mappedVertices == nullptr) {
        return false;
    }
    std::memcpy(mappedVertices, mesh.vertices.data(), vertexBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexStagingBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_indexStagingBuffer, /*cycle=*/true);
    if (mappedIndices == nullptr) {
        return false;
    }
    std::memcpy(mappedIndices, mesh.indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_indexStagingBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        return false;
    }
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    // **只上传用到的前缀**（T42）：池槽位按最大容量建，而实际形状通常只有其一小部分。
    SDL_GPUTransferBufferLocation vertexSource { m_vertexStagingBuffer, 0 };
    SDL_GPUBufferRegion           vertexDestination { resources.vertexBuffer, 0, vertexBytes };
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, /*cycle=*/false);

    SDL_GPUTransferBufferLocation indexSource { m_indexStagingBuffer, 0 };
    SDL_GPUBufferRegion           indexDestination { resources.indexBuffer, 0, indexBytes };
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, /*cycle=*/false);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    resources.usedIndexCount = static_cast<std::uint32_t>(mesh.indices.size());
    return true;
}

MeshHandle MeshRenderer::UploadSkinnedMesh(const SkinnedMeshData& mesh, const glm::dvec3& origin,
                                           std::uint32_t jointCapacity) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return MeshHandle {};
    }
    if (jointCapacity == 0U || jointCapacity > kMaxSkinJoints) {
        throw std::runtime_error("UploadSkinnedMesh：jointCapacity 非法（须满足 0 < capacity <= kMaxSkinJoints）");
    }

    MeshResources resources;
    resources.skinned        = true;
    resources.jointCapacity  = jointCapacity;
    resources.vertexBuffer   = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_VERTEX,
                                             static_cast<std::uint32_t>(mesh.vertices.size()) *
                                                 static_cast<std::uint32_t>(sizeof(SkinnedVertex)));
    resources.indexBuffer    = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_INDEX,
                                             static_cast<std::uint32_t>(mesh.indices.size()) *
                                                 static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    // 骨骼矩阵数组（顶点阶段只读 storage buffer）：**容量在创建时定死** ⇒ 之后每帧只覆盖写入，
    // 不在渲染帧里创建 / 扩容任何 GPU 资源（SKILL 第四节硬规则 4）。
    resources.boneMatrixBuffer = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                               jointCapacity * static_cast<std::uint32_t>(sizeof(float)) * 16U);
    resources.vertexCount     = static_cast<std::uint32_t>(mesh.vertices.size());
    resources.indexCount      = static_cast<std::uint32_t>(mesh.indices.size());
    resources.usedIndexCount  = 0;
    // V0.7 H0：显存记账（顶点 + 索引 + 骨骼矩阵数组）。
    resources.vertexBytes = static_cast<std::uint64_t>(mesh.vertices.size()) * sizeof(SkinnedVertex);
    resources.indexBytes  = static_cast<std::uint64_t>(mesh.indices.size()) * sizeof(std::uint32_t);
    resources.boneBytes   = static_cast<std::uint64_t>(jointCapacity) * sizeof(float) * 16U;
    // 世界原点登记（T41）：蒙皮网格的顶点是**网格局部坐标** ⇒ 每帧用 `SetMeshTransform` 推"原点 − 渲染原点"。
    resources.origin[0] = origin.x;
    resources.origin[1] = origin.y;
    resources.origin[2] = origin.z;
    // 待上传矩阵的 CPU 侧缓冲：**一次分配**（大小 = jointCapacity × 16），之后只 memcpy，零每帧分配。
    resources.pendingBoneMatrices.assign(static_cast<std::size_t>(jointCapacity) * 16U, 0.0F);

    std::uint32_t slot = 0;
    if (!m_freeSlots.empty()) {
        slot           = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_meshes[slot] = resources;
    } else {
        slot = static_cast<std::uint32_t>(m_meshes.size());
        m_meshes.push_back(resources);
    }
    const MeshHandle handle { slot + 1 };
    m_stats.meshBytes += resources.vertexBytes + resources.indexBytes + resources.boneBytes;  // V0.7 H0：显存记账

    // 初始几何上传：与 `UpdateMeshGeometry` 同一套"提交即走"路径（复用常驻暂存缓冲，不建 transfer buffer、不等 fence）。
    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(SkinnedVertex));
    const std::uint32_t indexBytes  = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));
    const std::uint32_t boneBytes   = jointCapacity * static_cast<std::uint32_t>(sizeof(float)) * 16U;
    if (!EnsureStagingBuffer(m_vertexStagingBuffer, m_vertexStagingCapacity, vertexBytes) ||
        !EnsureStagingBuffer(m_indexStagingBuffer, m_indexStagingCapacity, indexBytes) ||
        !EnsureStagingBuffer(m_boneStagingBuffer, m_boneStagingCapacity, boneBytes)) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadSkinnedMesh：暂存缓冲分配失败");
    }
    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_vertexStagingBuffer, /*cycle=*/true);
    if (mappedVertices == nullptr) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadSkinnedMesh：映射顶点暂存缓冲失败");
    }
    std::memcpy(mappedVertices, mesh.vertices.data(), vertexBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexStagingBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_indexStagingBuffer, /*cycle=*/true);
    if (mappedIndices == nullptr) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadSkinnedMesh：映射索引暂存缓冲失败");
    }
    std::memcpy(mappedIndices, mesh.indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(m_device, m_indexStagingBuffer);

    // 骨骼缓冲的**初值 = 绑定姿态**（每个关节一个单位矩阵 ⇒ 顶点保持绑定姿态）。
    // **为什么必须显式写一次**：GPU 缓冲的初始内容是未定义的；若在首次 `SetSkinningMatrices` 之前
    // 就被绘制，着色器会把顶点乘上零 / 垃圾矩阵 ⇒ 整份几何塌成一点 ⇒ **角色完全看不见**。
    // 写入绑定姿态后，"第一帧之前"与"没有动画"两种情形都有确定外观。
    {
        std::vector<float> identity(static_cast<std::size_t>(jointCapacity) * 16U, 0.0F);
        for (std::uint32_t joint = 0; joint < jointCapacity; ++joint) {
            identity[static_cast<std::size_t>(joint) * 16U + 0U]  = 1.0F;   // 列主序：对角元
            identity[static_cast<std::size_t>(joint) * 16U + 5U]  = 1.0F;
            identity[static_cast<std::size_t>(joint) * 16U + 10U] = 1.0F;
            identity[static_cast<std::size_t>(joint) * 16U + 15U] = 1.0F;
        }
        void* mappedBones = SDL_MapGPUTransferBuffer(m_device, m_boneStagingBuffer, /*cycle=*/true);
        if (mappedBones == nullptr) {
            ReleaseMesh(handle);
            throw std::runtime_error("UploadSkinnedMesh：映射骨骼暂存缓冲失败");
        }
        std::memcpy(mappedBones, identity.data(), boneBytes);
        SDL_UnmapGPUTransferBuffer(m_device, m_boneStagingBuffer);
    }

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadSkinnedMesh：获取命令缓冲失败");
    }
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTransferBufferLocation vertexSource { m_vertexStagingBuffer, 0 };
    SDL_GPUBufferRegion           vertexDestination { resources.vertexBuffer, 0, vertexBytes };
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, /*cycle=*/false);
    SDL_GPUTransferBufferLocation indexSource { m_indexStagingBuffer, 0 };
    SDL_GPUBufferRegion           indexDestination { resources.indexBuffer, 0, indexBytes };
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, /*cycle=*/false);
    SDL_GPUTransferBufferLocation boneSource { m_boneStagingBuffer, 0 };
    SDL_GPUBufferRegion           boneDestination { resources.boneMatrixBuffer, 0, boneBytes };
    SDL_UploadToGPUBuffer(copyPass, &boneSource, &boneDestination, /*cycle=*/false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    m_meshes[slot].usedIndexCount = resources.indexCount;
    return handle;
}

MeshHandle MeshRenderer::UploadInstancedMesh(const MeshData& prototype, std::uint32_t maxInstances) {
    if (prototype.vertices.empty() || prototype.indices.empty() || maxInstances == 0U) {
        return MeshHandle {};  // 前置条件不满足 ⇒ 无效句柄（不抛，见声明）
    }

    const std::uint32_t vertexCount = static_cast<std::uint32_t>(prototype.vertices.size());
    const std::uint32_t indexCount  = static_cast<std::uint32_t>(prototype.indices.size());

    MeshResources resources;
    resources.instanced        = true;
    resources.instanceCapacity = maxInstances;
    resources.vertexBuffer     = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_VERTEX,
                                               vertexCount * static_cast<std::uint32_t>(sizeof(MeshVertex)));
    resources.indexBuffer      = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_INDEX,
                                               indexCount * static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    // 实例数据数组（顶点阶段只读 storage buffer：`mat4` 变换 + 围合体 `vec4`×2）：**容量创建时定死**
    // ⇒ 之后每帧只覆盖写入，不在渲染帧里创建 / 扩容任何 GPU 资源（SKILL 第四节硬规则 4）。字节计入 `meshBytes`（ADR 0010 记账）。
    resources.instanceBuffer = create_buffer(m_device, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                             maxInstances * kInstanceTransformBytes);
    resources.vertexCount    = vertexCount;
    resources.indexCount     = indexCount;
    resources.usedIndexCount = 0;  // 由下面的 `UpdateMeshGeometry` 写（= 原型索引数 ⇒ 本帧可见）
    resources.vertexBytes    = static_cast<std::uint64_t>(vertexCount) * sizeof(MeshVertex);
    resources.indexBytes     = static_cast<std::uint64_t>(indexCount) * sizeof(std::uint32_t);
    resources.instanceBytes  = static_cast<std::uint64_t>(maxInstances) * kInstanceTransformBytes;
    // 实例原型**不登记**世界原点：每个实例的世界位姿由 `InstanceBatch::poses` 逐帧给出（见 InstanceBatch），
    // 故这里既不写 `origin` 也不用 `rotation`（保持缺省）。

    std::uint32_t slot = 0;
    if (!m_freeSlots.empty()) {
        slot           = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_meshes[slot] = resources;
    } else {
        slot = static_cast<std::uint32_t>(m_meshes.size());
        m_meshes.push_back(resources);
    }
    const MeshHandle handle { slot + 1 };
    m_stats.meshBytes += resources.vertexBytes + resources.indexBytes + resources.instanceBytes;

    // 初始几何上传：与 `UploadMesh` 同一套"提交即走"路径（复用常驻暂存缓冲，不建 transfer buffer、不等 fence）。
    // 实例缓冲的初值无需写：本帧没有 `UploadInstances` 之前 `instanceCount` 恒为 0 ⇒ 不会被绘制。
    if (!UpdateMeshGeometry(handle, prototype, glm::dvec3(0.0, 0.0, 0.0))) {
        ReleaseMesh(handle);
        throw std::runtime_error("UploadInstancedMesh：原型几何上传失败（容量不足或 GPU 命令提交失败）");
    }
    return handle;
}

void MeshRenderer::SetSkinningMatrices(MeshHandle handle, const float* matrices, std::uint32_t jointCount) {
    if (!handle.IsValid() || handle.id > m_meshes.size() || matrices == nullptr || jointCount == 0U) {
        return;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (!resources.skinned || resources.boneMatrixBuffer == nullptr) {
        return;
    }
    if (jointCount > resources.jointCapacity) {
        VX_LOG_WARN("SetSkinningMatrices：关节数 %u 超出该网格容量 %u ⇒ 忽略本次更新（不扩容，见 UploadSkinnedMesh）",
                    jointCount, resources.jointCapacity);
        return;
    }
    std::memcpy(resources.pendingBoneMatrices.data(), matrices, static_cast<std::size_t>(jointCount) * 16U * sizeof(float));
    resources.boneMatrixCount   = jointCount;
    resources.boneMatricesDirty = true;
}

void MeshRenderer::UploadSkinningMatrices(SDL_GPUCommandBuffer* commandBuffer) {
    // 统计本帧要上传的总字节（只有**脏**的蒙皮网格才算）。
    std::uint32_t totalBytes = 0;
    for (const MeshResources& resources : m_meshes) {
        if (resources.skinned && resources.boneMatricesDirty && resources.boneMatrixBuffer != nullptr) {
            totalBytes += resources.boneMatrixCount * static_cast<std::uint32_t>(sizeof(float)) * 16U;
        }
    }
    if (totalBytes == 0U) {
        return;  // 本帧没有蒙皮更新：零上传、零拷贝（"没有角色"时也不付出成本）
    }
    if (!EnsureStagingBuffer(m_boneStagingBuffer, m_boneStagingCapacity, totalBytes)) {
        VX_LOG_WARN("骨骼矩阵暂存缓冲分配失败（%u 字节）⇒ 本帧跳过蒙皮矩阵上传", totalBytes);
        return;
    }

    // 一次 map：把所有脏网格的矩阵按序拷进暂存缓冲，并记下各自的（偏移, 目标缓冲）。
    struct PendingUpload {
        SDL_GPUBuffer* buffer = nullptr;
        std::uint32_t  offset = 0;
        std::uint32_t  bytes  = 0;
    };
    std::vector<PendingUpload> uploads;
    uploads.reserve(4);
    {
        void* mapped = SDL_MapGPUTransferBuffer(m_device, m_boneStagingBuffer, /*cycle=*/true);
        if (mapped == nullptr) {
            VX_LOG_WARN("骨骼矩阵暂存缓冲映射失败 ⇒ 本帧跳过蒙皮矩阵上传");
            return;
        }
        auto* base       = static_cast<std::uint8_t*>(mapped);
        std::uint32_t offset = 0;
        for (MeshResources& resources : m_meshes) {
            if (!resources.skinned || !resources.boneMatricesDirty || resources.boneMatrixBuffer == nullptr) {
                continue;
            }
            const std::uint32_t bytes = resources.boneMatrixCount * static_cast<std::uint32_t>(sizeof(float)) * 16U;
            std::memcpy(base + offset, resources.pendingBoneMatrices.data(), bytes);
            uploads.push_back(PendingUpload { resources.boneMatrixBuffer, offset, bytes });
            offset += bytes;
        }
        SDL_UnmapGPUTransferBuffer(m_device, m_boneStagingBuffer);
    }

    // 一次 copy pass：逐网格拷到各自的骨骼缓冲（**不**新建任何 GPU 资源）。
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    for (const PendingUpload& upload : uploads) {
        SDL_GPUTransferBufferLocation source { m_boneStagingBuffer, upload.offset };
        SDL_GPUBufferRegion           destination { upload.buffer, 0, upload.bytes };
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    }
    SDL_EndGPUCopyPass(copyPass);

    for (MeshResources& resources : m_meshes) {
        resources.boneMatricesDirty = false;
    }
}

bool MeshRenderer::UploadInstances(SDL_GPUCommandBuffer* commandBuffer, const InstanceBatch* batches,
                                   std::size_t batchCount) {
    // 先把所有实例化原型标记为"本帧不可见" —— 只有本帧真正提供的批次才会被重新计数。
    // 否则上一帧留下的 `instanceCount` 会让**本帧不再提交**的批次继续被绘制（陈旧实例）。
    for (MeshResources& resources : m_meshes) {
        if (resources.instanced) {
            resources.instanceCount = 0;
        }
    }
    if (batches == nullptr || batchCount == 0) {
        return true;  // 本帧无实例批次：零上传、零拷贝
    }

    // 统计本帧要上传的总字节，并把超容量的批次**截断**（不静默：记 WARN 含数量，ADR 0034）。
    struct PendingBatch {
        std::uint32_t       slot   = 0;        ///< `m_meshes` 槽位（0 基）
        const InstancePose* poses  = nullptr;  ///< 该批的世界位姿数组
        std::uint32_t       offset = 0;        ///< 暂存缓冲内的字节偏移
        std::uint32_t       bytes  = 0;        ///< 本批写入字节
        std::uint32_t       used   = 0;        ///< 本批实例数（已按容量截断）
    };
    std::vector<PendingBatch> pending;
    pending.reserve(batchCount);
    std::uint32_t totalBytes = 0;
    for (std::size_t i = 0; i < batchCount; ++i) {
        const InstanceBatch& batch = batches[i];
        if (!batch.prototype.IsValid() || batch.prototype.id > m_meshes.size() || batch.poses == nullptr ||
            batch.count == 0U) {
            continue;
        }
        const MeshResources& resources = m_meshes[batch.prototype.id - 1];
        if (!resources.instanced || resources.instanceBuffer == nullptr) {
            continue;
        }
        std::uint32_t used = batch.count;
        if (used > resources.instanceCapacity) {
            VX_LOG_WARN("实例批次超容量：原型槽位 %u 请求 %u 实例 > 容量 %u ⇒ 截断（不静默，见 ADR 0034）",
                        batch.prototype.id, batch.count, resources.instanceCapacity);
            used = resources.instanceCapacity;
        }
        const std::uint32_t bytes = used * kInstanceTransformBytes;
        pending.push_back(PendingBatch { batch.prototype.id - 1, batch.poses, totalBytes, bytes, used });
        totalBytes += bytes;
    }
    if (pending.empty()) {
        return true;
    }
    if (!EnsureStagingBuffer(m_instanceStagingBuffer, m_instanceStagingCapacity, totalBytes)) {
        VX_LOG_WARN("实例位姿暂存缓冲分配失败（%u 字节）⇒ 本帧跳过实例绘制", totalBytes);
        return false;
    }

    // **一次 map**：把所有批次的位姿按序打包成渲染相对 `mat4` 写进暂存缓冲（禁止逐实例上传，SKILL 硬规则 3）。
    {
        void* mapped = SDL_MapGPUTransferBuffer(m_device, m_instanceStagingBuffer, /*cycle=*/true);
        if (mapped == nullptr) {
            VX_LOG_WARN("实例位姿暂存缓冲映射失败 ⇒ 本帧跳过实例绘制");
            return false;
        }
        auto* base = static_cast<std::uint8_t*>(mapped);
        for (const PendingBatch& item : pending) {
            const std::uint32_t written = PackInstanceTransforms(item.poses, item.used, item.used, m_renderOrigin,
                                                                 reinterpret_cast<float*>(base + item.offset));
            m_meshes[item.slot].instanceCount = written;
        }
        SDL_UnmapGPUTransferBuffer(m_device, m_instanceStagingBuffer);
    }

    // **一次 copy pass**：逐批拷到各自原型的实例缓冲（**不**新建任何 GPU 资源）。
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    for (const PendingBatch& item : pending) {
        SDL_GPUTransferBufferLocation source { m_instanceStagingBuffer, item.offset };
        SDL_GPUBufferRegion           destination { m_meshes[item.slot].instanceBuffer, 0, item.bytes };
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, /*cycle=*/false);
    }
    SDL_EndGPUCopyPass(copyPass);
    return true;
}

void MeshRenderer::SetMeshTransform(MeshHandle handle, const glm::dvec3& origin, const glm::quat& rotation) noexcept {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return;
    }
    resources.origin[0] = origin.x;
    resources.origin[1] = origin.y;
    resources.origin[2] = origin.z;
    resources.rotation  = rotation;
}

void MeshRenderer::SetMeshOpacity(MeshHandle handle, float opacity) noexcept {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return;
    }
    resources.opacity = std::clamp(opacity, 0.0F, 1.0F);
}

void MeshRenderer::SetMeshTint(MeshHandle handle, float red, float green, float blue, float strength) noexcept {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return;
    }
    // 强度钳到 [0,1]（0 = 不变）；颜色原样保留（强度为 0 时不影响结果）。
    resources.tint[0] = red;
    resources.tint[1] = green;
    resources.tint[2] = blue;
    resources.tint[3] = std::clamp(strength, 0.0F, 1.0F);
}

void MeshRenderer::SetMeshLodMorph(MeshHandle handle, float morphStep, float startDistance,
                                   float endDistance) noexcept {
    if (!handle.IsValid() || handle.id > m_meshes.size()) {
        return;
    }
    MeshResources& resources = m_meshes[handle.id - 1];
    if (resources.vertexBuffer == nullptr) {
        return;
    }
    // morphStep 钳到 ≥ 0（0 = 关闭 morph）；起 / 止距离**保持调用方给的顺序**（不交换）。
    resources.morphStep          = std::max(morphStep, 0.0F);
    resources.morphStartDistance = startDistance;
    resources.morphEndDistance   = endDistance;
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
    // T69：蒙皮网格还有一块骨骼矩阵 storage buffer（非蒙皮网格恒为 nullptr）。
    if (resources.boneMatrixBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, resources.boneMatrixBuffer);
    }
    // V0.7 H1：实例化原型还有一块实例变换 storage buffer（非实例化网格恒为 nullptr）。
    if (resources.instanceBuffer != nullptr) {
        SDL_ReleaseGPUBuffer(m_device, resources.instanceBuffer);
    }
    // V0.7 H0：显存记账回收（**饱和减法**，重复释放不会下溢）。
    const std::uint64_t freedBytes =
        resources.vertexBytes + resources.indexBytes + resources.boneBytes + resources.instanceBytes;
    m_stats.meshBytes = (m_stats.meshBytes >= freedBytes) ? (m_stats.meshBytes - freedBytes) : 0ULL;
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
    // W7-S3b：LOD 原点（渲染相对坐标，xyz；w 预留）——顶点着色器 morph 距离的基准。缺省 (0,0,0)。
    m_cameraUniform.lodOrigin = glm::vec4(camera.lodOrigin, 0.0F);
}

void MeshRenderer::ReleaseEnvironmentTextures() noexcept {
    // 四张环境贴图（不含常驻的 1×1 占位纹理 —— 见构造函数里的记账说明）。
    SDL_GPUTexture** members[] = { &m_skyTexture, &m_irradianceTexture, &m_prefilterTexture, &m_brdfLutTexture };
    for (SDL_GPUTexture** member : members) {
        if (*member != nullptr) {
            SDL_ReleaseGPUTexture(m_device, *member);
            *member = nullptr;
        }
    }
    // 记账同步（重复调用时 m_environmentBytes 已为 0，扣 0 无副作用）。
    m_stats.textureBytes -= m_environmentBytes;
    m_environmentBytes       = 0;
    m_environmentReady       = false;
    m_textureAccountingDirty = true;
}

bool MeshRenderer::BakeEnvironment(const EnvironmentSource& source) {
    if (source.pixels == nullptr || source.width == 0 || source.height == 0) {
        return false;
    }

    // 重建前先把上一次的产物释放干净（幂等 ⇒ 失败可重试、重载不泄漏）。
    ReleaseEnvironmentTextures();

    constexpr std::uint64_t kBytesPerPixel = 8ULL;  // R16G16B16A16_FLOAT
    auto account = [this](std::uint64_t bytes) {
        m_environmentBytes += bytes;
        m_stats.textureBytes += bytes;
    };

    // ---- ① 天空 HDRI：转半精度后上传（只作采样源 ⇒ usage 只需 SAMPLER）----
    const std::vector<std::uint16_t> skyPixels = ConvertEquirectRgb32fToRgba16f(source);
    m_skyTexture = create_and_upload_texture_2d(
        m_device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, source.width, source.height, SDL_GPU_TEXTUREUSAGE_SAMPLER,
        skyPixels.data(), static_cast<std::uint32_t>(skyPixels.size() * sizeof(std::uint16_t)));
    if (m_skyTexture == nullptr) {
        VX_LOG_ERROR("上传天空 HDRI 失败（%u×%u）：%s", source.width, source.height, SDL_GetError());
        ReleaseEnvironmentTextures();
        return false;
    }
    account(EstimateTextureMipChainBytes(source.width, source.height, /*levels=*/1, kBytesPerPixel));

    // ---- ② 三件套的渲染目标：先建纹理（烘焙 pass 必须有输出目标）----
    // 为什么格式用 RGBA16F 而不是 ADR 0021 里估计的 RG16F（BRDF LUT）：R16G16B16A16_FLOAT 是
    // 主通道 HDR 目标**已经在用**的格式，渲染目标支持面最广；代价是 LUT 多 0.25 MB（256² × 4 B），
    // 在 300 MB 预算下可忽略。切换条件：需要省这点显存时改回 RG16F 并实测后端支持。
    auto createBakeTarget = [this](std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
        SDL_GPUTextureCreateInfo info {};
        info.type                 = SDL_GPU_TEXTURETYPE_2D;
        info.format               = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width                = width;
        info.height               = height;
        info.layer_count_or_depth = 1;
        info.num_levels           = levels;
        info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        return SDL_CreateGPUTexture(m_device, &info);
    };

    m_irradianceTexture = createBakeTarget(kEnvironmentIrradianceWidth, kEnvironmentIrradianceHeight, /*levels=*/1);
    m_prefilterTexture  = createBakeTarget(kEnvironmentPrefilterBaseWidth, kEnvironmentPrefilterBaseHeight,
                                          kEnvironmentPrefilterMipCount);
    m_brdfLutTexture    = createBakeTarget(kEnvironmentBrdfLutSize, kEnvironmentBrdfLutSize, /*levels=*/1);
    if (m_irradianceTexture == nullptr || m_prefilterTexture == nullptr || m_brdfLutTexture == nullptr) {
        VX_LOG_ERROR("创建 IBL 烘焙目标失败：%s", SDL_GetError());
        ReleaseEnvironmentTextures();
        return false;
    }
    account(EstimateTextureMipChainBytes(kEnvironmentIrradianceWidth, kEnvironmentIrradianceHeight, 1, kBytesPerPixel));
    account(EstimateTextureMipChainBytes(kEnvironmentPrefilterBaseWidth, kEnvironmentPrefilterBaseHeight,
                                         kEnvironmentPrefilterMipCount, kBytesPerPixel));
    account(EstimateTextureMipChainBytes(kEnvironmentBrdfLutSize, kEnvironmentBrdfLutSize, 1, kBytesPerPixel));

    // ---- ③ 三条烘焙 pass：同一个命令缓冲内依次绘制，末尾一次提交 + 等栅栏（与 CreateTextureArray 同模式）----
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        VX_LOG_ERROR("IBL 烘焙获取命令缓冲失败：%s", SDL_GetError());
        ReleaseEnvironmentTextures();
        return false;
    }

    const SDL_GPUTextureSamplerBinding skyBinding { m_skyTexture, m_environmentSampler };
    // 三条 pass 都是"全屏覆盖" ⇒ 颜色目标用 DONT_CARE 加载（省一次无用的读取 / 清屏）。
    constexpr SDL_GPULoadOp  kOverwriteLoad = SDL_GPU_LOADOP_DONT_CARE;
    constexpr SDL_GPUStoreOp kKeepStore     = SDL_GPU_STOREOP_STORE;

    {
        SDL_GPUColorTargetInfo target {};
        target.texture  = m_irradianceTexture;
        target.load_op  = kOverwriteLoad;
        target.store_op = kKeepStore;
        DrawFullscreenPass(commandBuffer, target, m_irradiancePipeline, &skyBinding, 1, nullptr, 0);
    }

    for (std::uint32_t mip = 0; mip < kEnvironmentPrefilterMipCount; ++mip) {
        struct PrefilterUniform {
            float params[4];  ///< x = 本 mip 对应的 roughness（std140：单个 vec4）
        };
        PrefilterUniform uniform {};
        uniform.params[0] = PrefilterRoughnessForMip(mip);

        SDL_GPUColorTargetInfo target {};
        target.texture   = m_prefilterTexture;
        target.mip_level = mip;
        target.load_op   = kOverwriteLoad;
        target.store_op  = kKeepStore;
        DrawFullscreenPass(commandBuffer, target, m_prefilterPipeline, &skyBinding, 1, &uniform,
                           static_cast<std::uint32_t>(sizeof(uniform)));
    }

    {
        SDL_GPUColorTargetInfo target {};
        target.texture  = m_brdfLutTexture;
        target.load_op  = kOverwriteLoad;
        target.store_op = kKeepStore;
        DrawFullscreenPass(commandBuffer, target, m_brdfLutPipeline, nullptr, 0, nullptr, 0);
    }

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr) {
        // 提交失败：命令缓冲已被消费，**不敢**在此释放纹理（GPU 可能仍在用）⇒ 保留资源、
        // 只把"就绪"置否（着色器因此不会采样它们）；记账保持不变，析构或下次烘焙会回收。
        VX_LOG_ERROR("IBL 烘焙提交失败：%s", SDL_GetError());
        return false;
    }
    SDL_WaitForGPUFences(m_device, /*wait_all=*/true, &fence, 1);
    SDL_ReleaseGPUFence(m_device, fence);

    m_environmentReady       = true;
    m_textureAccountingDirty = true;
    return true;
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

    // T79⑥（业界标准 = **PSO 预建 / 创建移出热路径**，参照 UE 的 `FShaderPipelineCache` 与 SKILL 第四节硬规则 4）：
    // 采样数一变，主通道（含深度偏移变体）与天空管线都必须重建；原先它发生在**下一帧的 `RenderFrame` 里**
    // ⇒ "切档当帧"被算进帧时间。现在改在**设置生效点**（本调用）就地重建，`RenderFrame` 里的
    // `EnsureMainPipeline` 退化为**幂等保险**（档位已一致 ⇒ 直接返回，不再创建任何东西）。
    // 为什么可以在这里建：管线创建不依赖渲染目标 / 窗口尺寸，只用常驻 Shader ⇒ 与尺寸变更解耦。
    if (m_pipeline == nullptr || m_pipelineSampleCount != effective) {
        try {
            EnsureMainPipeline(effective);
            VX_LOG_INFO("MSAA 管线已按 %u× 预建（T79⑥：创建移出 `RenderFrame`，主通道 + 深度偏移变体 + 天空）",
                        effective);
        } catch (const std::exception& error) {
            // 本函数是 `noexcept`：失败只记 ERROR，`RenderFrame` 的 `EnsureMainPipeline` 会再试一次
            //（那条路径要抛就得抛 —— 没有管线的帧必须显式失败，不能默默画出空画面）。
            VX_LOG_ERROR("MSAA 管线预建失败（%u×）：%s", effective, error.what());
        }
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

void MeshRenderer::LogVramAccounting(std::uint32_t width, std::uint32_t height) const {
    // ADR 0010 的记账义务：把 **GPU 显存**按项打到日志（首次创建与每次尺寸 / 档位变化各记一次），
    // 这样"预算是否达标"可以直接在日志里核对，不必依赖面板读数或截图。
    // T23 起把 MSAA 颜色目标与 MSAA 深度目标也列出，使"开 / 关 MSAA 的代价"可直接对比。
    // V0.7 H0 起**网格缓冲**（顶点 + 索引 + 骨骼矩阵）一并入账 —— 原先只统计纹理，无法核对 ADR 0024 的 VRAM 预算。
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
    const double        textureMb      = static_cast<double>(m_stats.textureBytes) / kBytesPerMb;
    const double        meshMb         = static_cast<double>(m_stats.meshBytes) / kBytesPerMb;
    const double        vramMb         = static_cast<double>(m_stats.textureBytes + m_stats.meshBytes) / kBytesPerMb;
    const double        shadowShare    = (m_stats.textureBytes > 0)
                                             ? 100.0 * static_cast<double>(m_shadowTextureBytes) /
                                                   static_cast<double>(m_stats.textureBytes)
                                             : 0.0;
    VX_LOG_INFO("GPU 显存记账（纹理 + 网格缓冲）：材质数组 %.2f MB + 深度目标 %.2f MB + HDR 目标 %.2f MB + "
                "阴影 %u 级 %u² %.2f MB（占 %.1f%%）+ MSAA %u×（颜色 %.2f MB + 深度 %.2f MB = %.2f MB）"
                "+ 环境贴图 %.2f MB = 纹理小计 %.2f MB；**网格缓冲 %.2f MB** ⇒ 合计 %.2f MB（交换链 %ux%u）",
                static_cast<double>(materialBytes) / kBytesPerMb, static_cast<double>(baseDepthBytes) / kBytesPerMb,
                static_cast<double>(m_hdrBytes) / kBytesPerMb, m_shadowTextureCascades, m_shadowTextureResolution,
                static_cast<double>(m_shadowTextureBytes) / kBytesPerMb, shadowShare, m_msaaSampleCount,
                static_cast<double>(m_msaaColorBytes) / kBytesPerMb, static_cast<double>(msaaDepthBytes) / kBytesPerMb,
                static_cast<double>(msaaTotalBytes) / kBytesPerMb,
                static_cast<double>(m_environmentBytes) / kBytesPerMb, textureMb, meshMb, vramMb, width, height);
}

void MeshRenderer::DrawMeshes(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const MeshHandle* meshes,
                              std::size_t meshCount, bool pushEmissive, SDL_GPUGraphicsPipeline* basePipeline,
                              SDL_GPUGraphicsPipeline* depthBiasedPipeline, SDL_GPUGraphicsPipeline* skinnedPipeline,
                              SDL_GPUBuffer* primaryStorageBuffer, EmissivePushState& emissiveState,
                              MeshTransformPushState& transformState, SDL_GPUGraphicsPipeline* waterPipeline,
                              bool waterPass) {
    if (meshes == nullptr) {
        return;
    }
    if (waterPass && waterPipeline == nullptr) {
        return;  // 水面管线不可用 ⇒ 不画（而不是绑空管线）
    }

    // 管线与 storage buffer 的**当前绑定状态**（只在选择结果变化时才重新绑定 ⇒ 绑定次数 = 分组数）：
    //   管线 0 = `basePipeline`（调用方进入时已绑好：主通道 `m_pipeline` / 阴影通道 `m_shadowPipeline`）
    //   管线 1 = `depthBiasedPipeline`（T78：仅地表 tile 这类共面重叠网格；阴影通道传 nullptr ⇒ 永不选中）
    //   管线 2 = `skinnedPipeline`（T69：蒙皮网格，顶点着色器做线性混合蒙皮）
    //   管线 3 = `waterPipeline`（W6：水面，`water.frag` 的 flow 着色 + alpha 混合；阴影通道传 nullptr）
    // storage buffer 数：非蒙皮 = 1（binding 0 = 相机 / 光空间矩阵）；蒙皮 = 2（+ binding 1 = 骨骼矩阵）。
    int boundPipeline       = 0;
    int boundStorageBuffers = 1;

    for (std::size_t i = 0; i < meshCount; ++i) {
        const MeshHandle handle = meshes[i];
        if (!handle.IsValid() || handle.id > m_meshes.size()) {
            continue;
        }
        const MeshResources& resources = m_meshes[handle.id - 1];
        // W6：水面网格**只在**水面通道绘制（且**不投影阴影**：阴影通道 `waterPass == false` ⇒ 这里被跳过）；
        // 非水面网格**不在**水面通道绘制。两个通道因此互不重复、互不遗漏。
        if (resources.water != waterPass) {
            continue;
        }
        if (resources.vertexBuffer == nullptr || resources.indexBuffer == nullptr) {
            continue;
        }
        // T42：变长网格（倒塌整体池）可能"本帧不可见"（尚未填充，或落定后被清空）——
        // 此时索引数为 0，直接跳过（**不推 uniform、不绑定、不绘制**）。
        if (resources.usedIndexCount == 0U) {
            continue;
        }

        // 管线选择（只在**选择结果变化**时绑定）：3 = 水面、2 = 蒙皮、1 = 带深度偏移变体、0 = 基础管线。
        const int wanted = waterPass ? 3
                                     : (resources.skinned
                                            ? 2
                                            : (((depthBiasedPipeline != nullptr) && resources.depthBiased) ? 1 : 0));
        if (wanted == 2 && skinnedPipeline == nullptr) {
            continue;  // 蒙皮管线不可用（不应发生）⇒ 跳过而不是绑空管线
        }
        if (wanted != boundPipeline) {
            SDL_BindGPUGraphicsPipeline(pass, wanted == 3 ? waterPipeline
                                                          : (wanted == 2 ? skinnedPipeline
                                                                         : (wanted == 1 ? depthBiasedPipeline
                                                                                        : basePipeline)));
            boundPipeline = wanted;
        }
        // 顶点 storage buffer：蒙皮网格多一块骨骼矩阵（binding 1）。绑定数变化时才重绑。
        const int wantedStorageBuffers = resources.skinned ? 2 : 1;
        if (wantedStorageBuffers != boundStorageBuffers) {
            SDL_GPUBuffer* storageBuffers[2] = { primaryStorageBuffer, resources.boneMatrixBuffer };
            SDL_BindGPUVertexStorageBuffers(pass, 0, storageBuffers, static_cast<Uint32>(wantedStorageBuffers));
            boundStorageBuffers = wantedStorageBuffers;
        }

        // 逐网格模型变换（T41 起；T33 由 `vec4` 偏移泛化为 `mat4`；W6e 增不透明度）：把**网格局部坐标**
        // 变成渲染相对坐标。每帧对每个网格只是一次 96 字节的 `SDL_PushGPUVertexUniformData`（**不是**上传，
        // 不创建 / 不拷贝 GPU 缓冲），且 uniform 对**后续**绘制持续生效 ⇒ 与自发光同一套去重。
        // 为什么用矩阵而不是"偏移 + 旋转分开传"：倒塌中的刚体**位置与姿态都在变**，
        // 而局部顶点完全不变 ⇒ 一次推送即可，CPU 无需重烘焙上万顶点（T33）。
        {
            MeshTransformUniform transform;
            transform.modelToRender    = glm::mat4_cast(resources.rotation);
            transform.modelToRender[3] = glm::vec4(static_cast<float>(resources.origin[0] - m_renderOrigin.x),
                                                   static_cast<float>(resources.origin[1] - m_renderOrigin.y),
                                                   static_cast<float>(resources.origin[2] - m_renderOrigin.z), 1.0F);
            // W6e：逐网格不透明度（片元按 Bayer 抖动 discard 做 dither 淡出）。必须进**去重键**，
            // 否则"只改不透明度"的网格会被误判为未变而不推送。
            transform.meshParams[0] = resources.opacity;
            // W7-S3b：LOD morph 参数（CDLOD 顶点过渡消接缝）。`y` = morphStep（> 0 启用）、
            // `z`/`w` = morph 起 / 止距离（格）。三者也必须进去重键（否则"只改 LOD 参数"会被误判为未变）。
            transform.meshParams[1] = resources.morphStep;
            transform.meshParams[2] = resources.morphStartDistance;
            transform.meshParams[3] = resources.morphEndDistance;
            // V0.10：逐网格 tint（摆放模式"不可放置"红色提示）。强度 0（默认）⇒ 与原值逐位一致，
            // 但仍必须进去重键，否则"只改 tint"的网格会被误判为未变而不推送。
            transform.meshTint = glm::vec4(resources.tint[0], resources.tint[1], resources.tint[2], resources.tint[3]);
            const float* matrix  = &transform.modelToRender[0][0];
            bool         changed = !transformState.pushed || transformState.opacity != transform.meshParams[0] ||
                                   transformState.morphStep != transform.meshParams[1] ||
                                   transformState.morphStartDistance != transform.meshParams[2] ||
                                   transformState.morphEndDistance != transform.meshParams[3];
            for (int element = 0; element < 16 && !changed; ++element) {
                changed = transformState.matrix[element] != matrix[element];
            }
            for (int channel = 0; channel < 4 && !changed; ++channel) {
                changed = transformState.tint[channel] != resources.tint[channel];
            }
            if (changed) {
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &transform, static_cast<Uint32>(sizeof(transform)));
                transformState.pushed             = true;
                transformState.opacity            = transform.meshParams[0];
                transformState.morphStep          = transform.meshParams[1];
                transformState.morphStartDistance = transform.meshParams[2];
                transformState.morphEndDistance   = transform.meshParams[3];
                for (int element = 0; element < 16; ++element) {
                    transformState.matrix[element] = matrix[element];
                }
                for (int channel = 0; channel < 4; ++channel) {
                    transformState.tint[channel] = resources.tint[channel];
                }
            }
        }

        // 自发光（T27，片元 uniform 槽 3）：只对 `emissive = true` 的网格叠加，其余为零值。
        // T39：uniform 数据对**后续**绘制持续生效，故只在"值变了"时推送 —— 推送次数从 O(网格数)
        // 降到 O(值变化次数)（本场景：非自发光一批 + 自发光一批 ⇒ 约 2 次），绘制结果逐像素不变。
        if (pushEmissive) {
            struct EmissiveParams {
                float emissive[4];  ///< rgb = 自发光颜色（线性光），a = 强度（0 = 普通地表网格）
            };
            EmissiveParams params {};
            if (resources.emissive) {
                params.emissive[0] = m_emissiveColor[0];
                params.emissive[1] = m_emissiveColor[1];
                params.emissive[2] = m_emissiveColor[2];
                params.emissive[3] = 1.0F;
            }
            const bool changed = !emissiveState.pushed || emissiveState.color[0] != params.emissive[0] ||
                                 emissiveState.color[1] != params.emissive[1] ||
                                 emissiveState.color[2] != params.emissive[2] ||
                                 emissiveState.strength != params.emissive[3];
            if (changed) {
                SDL_PushGPUFragmentUniformData(commandBuffer, 3, &params, static_cast<Uint32>(sizeof(params)));
                emissiveState.pushed     = true;
                emissiveState.color[0]   = params.emissive[0];
                emissiveState.color[1]   = params.emissive[1];
                emissiveState.color[2]   = params.emissive[2];
                emissiveState.strength   = params.emissive[3];
            }
        }
        SDL_GPUBufferBinding vertexBinding { resources.vertexBuffer, 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

        SDL_GPUBufferBinding indexBinding { resources.indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_DrawGPUIndexedPrimitives(pass, resources.usedIndexCount, /*num_instances=*/1, /*first_index=*/0,
                                     /*vertex_offset=*/0, /*first_instance=*/0);

        // 统计**实际执行**的绘制（T24）：主通道与阴影通道的索引绘制都计入。
        // T42 起按 `usedIndexCount`（**实际绘制**的索引 / 顶点）记账，而不是缓冲容量。
        ++m_stats.drawCalls;
        m_stats.triangleCount += resources.usedIndexCount / 3U;
        m_stats.vertexCount += resources.usedIndexCount;
    }
}

void MeshRenderer::DrawInstancedBatches(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                                        const InstanceBatch* batches, std::size_t batchCount,
                                        SDL_GPUGraphicsPipeline* instancedPipeline,
                                        SDL_GPUBuffer* matrixStorageBuffer, bool pushEmissive,
                                        EmissivePushState& emissiveState, MeshTransformPushState& transformState) {
    if (batches == nullptr || batchCount == 0U || instancedPipeline == nullptr) {
        return;
    }
    // 全部批次共用同一条实例化管线（差异只在各自的顶点 / 实例缓冲）⇒ 本通道只绑一次。
    SDL_BindGPUGraphicsPipeline(pass, instancedPipeline);

    for (std::size_t i = 0; i < batchCount; ++i) {
        const MeshHandle handle = batches[i].prototype;
        if (!handle.IsValid() || handle.id > m_meshes.size()) {
            continue;
        }
        const MeshResources& resources = m_meshes[handle.id - 1];
        if (!resources.instanced || resources.instanceBuffer == nullptr || resources.vertexBuffer == nullptr ||
            resources.indexBuffer == nullptr) {
            continue;
        }
        // 本帧实例数由 `UploadInstances` 写入（0 ⇒ 本帧不可见 / 未上传 ⇒ 跳过）。
        if (resources.instanceCount == 0U || resources.usedIndexCount == 0U) {
            continue;
        }

        // 顶点 storage buffer：binding 0 = 相机 / 该级光空间矩阵，binding 1 = 本原型的实例变换。
        // 实例缓冲**逐原型**不同 ⇒ 每批都要重绑（两次绑定，与批次数的两倍成正比、与物件数无关）。
        SDL_GPUBuffer* storageBuffers[2] = { matrixStorageBuffer, resources.instanceBuffer };
        SDL_BindGPUVertexStorageBuffers(pass, 0, storageBuffers, 2);

        // 逐批推 `MeshTransformUniform`：矩阵字段被实例化着色器**忽略**（矩阵来自实例缓冲），
        // 只有 `meshParams.x`（逐批不透明度）生效 ⇒ 与 `DrawMeshes` 同一套去重推送。
        {
            MeshTransformUniform transform;  // 缺省 = 单位矩阵（平移分量已由实例缓冲给出）+ 不透明
            transform.meshParams[0] = resources.opacity;
            // V0.10：逐**批** tint（与 `DrawMeshes` 同义；实例着色器读同一个 `MeshTransformBlock`）。
            transform.meshTint = glm::vec4(resources.tint[0], resources.tint[1], resources.tint[2], resources.tint[3]);
            const float* matrix = &transform.modelToRender[0][0];
            bool changed = !transformState.pushed || transformState.opacity != transform.meshParams[0] ||
                           transformState.morphStep != transform.meshParams[1] ||
                           transformState.morphStartDistance != transform.meshParams[2] ||
                           transformState.morphEndDistance != transform.meshParams[3];
            for (int element = 0; element < 16 && !changed; ++element) {
                changed = transformState.matrix[element] != matrix[element];
            }
            for (int channel = 0; channel < 4 && !changed; ++channel) {
                changed = transformState.tint[channel] != resources.tint[channel];
            }
            if (changed) {
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &transform, static_cast<Uint32>(sizeof(transform)));
                transformState.pushed             = true;
                transformState.opacity            = transform.meshParams[0];
                transformState.morphStep          = transform.meshParams[1];
                transformState.morphStartDistance = transform.meshParams[2];
                transformState.morphEndDistance   = transform.meshParams[3];
                for (int element = 0; element < 16; ++element) {
                    transformState.matrix[element] = matrix[element];
                }
                for (int channel = 0; channel < 4; ++channel) {
                    transformState.tint[channel] = resources.tint[channel];
                }
            }
        }

        // 自发光（片元槽 3）：与 `DrawMeshes` 同义 —— **必须**在实例绘制前给出确定值，
        // 否则会继承上一条（可能是自发光网格的）绘制留下的 uniform。实例原型当前恒为非自发光 ⇒ 推零值。
        if (pushEmissive) {
            struct EmissiveParams {
                float emissive[4];
            };
            EmissiveParams params {};
            if (resources.emissive) {
                params.emissive[0] = m_emissiveColor[0];
                params.emissive[1] = m_emissiveColor[1];
                params.emissive[2] = m_emissiveColor[2];
                params.emissive[3] = 1.0F;
            }
            const bool changed = !emissiveState.pushed || emissiveState.color[0] != params.emissive[0] ||
                                 emissiveState.color[1] != params.emissive[1] ||
                                 emissiveState.color[2] != params.emissive[2] ||
                                 emissiveState.strength != params.emissive[3];
            if (changed) {
                SDL_PushGPUFragmentUniformData(commandBuffer, 3, &params, static_cast<Uint32>(sizeof(params)));
                emissiveState.pushed   = true;
                emissiveState.color[0] = params.emissive[0];
                emissiveState.color[1] = params.emissive[1];
                emissiveState.color[2] = params.emissive[2];
                emissiveState.strength = params.emissive[3];
            }
        }

        SDL_GPUBufferBinding vertexBinding { resources.vertexBuffer, 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding { resources.indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        // **一次绘制**画完整批：`num_instances = 本帧实例数` ⇒ draw call 与物件数解耦（ADR 0034 决策一）。
        SDL_DrawGPUIndexedPrimitives(pass, resources.usedIndexCount, resources.instanceCount, /*first_index=*/0,
                                     /*vertex_offset=*/0, /*first_instance=*/0);

        ++m_stats.drawCalls;
        m_stats.triangleCount += static_cast<std::uint64_t>(resources.usedIndexCount / 3U) * resources.instanceCount;
        m_stats.vertexCount += static_cast<std::uint64_t>(resources.usedIndexCount) * resources.instanceCount;
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
                               IRenderOverlay* overlay, const ShadowCascadeDrawList* shadowLists,
                               std::size_t shadowListCount, const InstanceBatch* batches, std::size_t batchCount) {
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr) {
        throw std::runtime_error(std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError());
    }

    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t   width     = 0;
    std::uint32_t   height    = 0;

    // T38：把"**等待**交换链纹理"的耗时单独计量（见 `RenderStats::swapchainWaitMs`）——
    // 这段等待发生在 RenderFrame 内部，若并入渲染提交耗时，会把"在空等 GPU"误判成"CPU 忙"。
    vx::Clock  swapchainClock;
    const bool swapchainAcquired =
        SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, m_window, &swapchain, &width, &height);
    m_stats.swapchainWaitMs = swapchainClock.Tick() * 1000.0;
    if (!swapchainAcquired) {
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
    // 主通道管线的采样数必须与渲进的目标一致。T79⑥ 起这条路径**通常什么都不做**：管线已在
    // `SetMsaaSampleCount`（设置生效点）预建好；这里保留为**幂等保险**（档位已一致 ⇒ 立即返回）。
    EnsureMainPipeline(m_msaaSampleCount);
    EnsureShadowTarget();
    UploadCameraUniform(commandBuffer);
    // T69：把本帧所有**脏**蒙皮网格的骨骼矩阵整块上传（每个蒙皮网格一次；无蒙皮更新时零成本）。
    // 放在这里（主 / 阴影通道之前）⇒ 两个通道读到的都是本帧同一份矩阵。
    UploadSkinningMatrices(commandBuffer);
    // V0.7 H1：把本帧所有**实例批次**的位姿一次上传（每个原型一次；无实例批次时零成本）。
    // 放在这里（主 / 阴影通道之前）⇒ 两个通道读到的是**同一份**实例缓冲 ⇒ 阴影与几何不可能错位（ADR 0034）。
    // 失败（暂存分配 / 映射失败）时内部已 WARN ⇒ 本帧不画实例，这里只需丢弃返回值。
    (void)UploadInstances(commandBuffer, batches, batchCount);
    // 阴影关闭时不上传矩阵（省一次每帧的小拷贝；着色器也整体跳过采样）。
    if (m_shadowUniformValid && m_shadowUniform.enabled > 0.5F) {
        UploadShadowMatrices(commandBuffer);
    }

    // 显存记账（ADR 0010）：仅在渲染目标被（重）创建的帧打印一次，避免逐帧刷屏。
    if (m_textureAccountingDirty) {
        LogVramAccounting(width, height);
        m_textureAccountingDirty = false;
    }

    // 本帧绘制统计清零（纹理显存记账是持久的，不在此列）。
    m_stats.drawCalls     = 0;
    m_stats.triangleCount = 0;
    m_stats.vertexCount   = 0;

    // ---- 阴影通道（T21b / ADR 0010 P1）：必须在**主通道之前**，逐级把同一批网格写进深度数组的一层 ----
    // 顶点是**网格局部**坐标（T41），`DrawMeshes` 会为两个通道各推一次"网格世界原点 − 渲染原点"
    // 把它补成渲染原点相对坐标；光空间矩阵也建立在同一坐标系（见 shadow_cascade.hpp），
    // 故这里直接复用主通道的顶点 / 索引缓冲，不需要任何重传或坐标补偿。
    if (m_shadowUniformValid && m_shadowUniform.enabled > 0.5F && m_shadowTexture != nullptr) {
        // 阴影通道不推自发光（`pushEmissive = false`），此状态仅用于满足签名；主通道另有独立状态。
        // 逐网格模型变换（T41 / T33）两通道都要推，且**各持一份去重状态**（见 `DrawMeshes`）；
        // 该状态可跨级联复用：它记录的是"命令缓冲里当前生效的值"，故下一级的第一批网格若不同会照常推送。
        EmissivePushState      shadowEmissiveState;
        MeshTransformPushState shadowTransformState;
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

            // P1：该级若有**逐级绘制列表**则用它 —— 盒外几何进不了该级阴影图（判据见
            // `AabbCastsIntoLightSpace`）；`shadowLists` 为空 / 该级无列表时退回主通道列表（逐位一致）。
            const MeshHandle* cascadeMeshes    = meshes;
            std::size_t       cascadeMeshCount = meshCount;
            if (shadowLists != nullptr && static_cast<std::size_t>(cascade) < shadowListCount) {
                cascadeMeshes    = shadowLists[cascade].meshes;
                cascadeMeshCount = shadowLists[cascade].count;
            }

            // T78：阴影通道**不使用**深度偏移变体（传 nullptr）⇒ 保持阴影现状、逐网格不切管线。
            // T69：蒙皮网格（主角）在阴影通道走蒙皮阴影管线 —— 否则换成模型后主角会不再投影（可见回退）。
            DrawMeshes(commandBuffer, shadowPass, cascadeMeshes, cascadeMeshCount, /*pushEmissive=*/false,
                       /*basePipeline=*/m_shadowPipeline, /*depthBiasedPipeline=*/nullptr,
                       /*skinnedPipeline=*/m_shadowSkinnedPipeline,
                       /*primaryStorageBuffer=*/m_shadowMatrixBuffers[cascade], shadowEmissiveState,
                       shadowTransformState, /*waterPipeline=*/nullptr, /*waterPass=*/false);
            // V0.7 H1：实例化物件在阴影通道同样投影（否则"影子消失" = 可见回退）。与 `DrawMeshes` 共用
            // 本级的去重状态（推送的是"命令缓冲里当前生效的值"，两段连续绘制之间不会错推）。
            // H1 阶段不做逐级联剔除 ⇒ 每级都画全部实例（H2 随物件层接入补逐级过滤）。
            DrawInstancedBatches(commandBuffer, shadowPass, batches, batchCount, m_shadowInstancedPipeline,
                                 /*matrixStorageBuffer=*/m_shadowMatrixBuffers[cascade], /*pushEmissive=*/false,
                                 shadowEmissiveState, shadowTransformState);
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

    // ---- 天空（T67 / ADR 0021 第一条）：主通道内、**网格之前** ----
    // 深度测试与写入均关闭 ⇒ 天空既不遮挡几何、也不被几何遮挡，网格随后照常覆盖它；
    // 因此不需要第二条颜色目标，也不会与 MSAA 的 resolve 冲突（天空写在同一个多采样目标里）。
    // 视线方向由**逆视图投影**逐像素求得 —— 相机在渲染相对坐标系的原点（红线 6 / T41），故无需相机位置。
    if (m_environmentReady && m_skyTexture != nullptr) {
        struct SkyUniform {
            glm::mat4 inverseViewProjection;  ///< 相机视投影矩阵的逆（作用在**渲染相对**坐标上）
        };
        SkyUniform skyUniform {};
        skyUniform.inverseViewProjection = glm::inverse(m_cameraUniform.viewProjection);

        // 片元 uniform 槽 0：本通道随后的材质 uniform 会把它覆盖掉（天空已绘制完毕，顺序即语义）。
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &skyUniform, static_cast<Uint32>(sizeof(skyUniform)));

        SDL_BindGPUGraphicsPipeline(pass, m_skyPipeline);
        SDL_GPUTextureSamplerBinding skyBinding { m_skyTexture, m_environmentSampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &skyBinding, 1);
        SDL_DrawGPUPrimitives(pass, 3, /*num_instances=*/1, /*first_vertex=*/0, /*first_instance=*/0);
    }

    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);

    SDL_GPUBuffer* cameraBuffers[1] = { m_cameraUniformBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, cameraBuffers, 1);

    // 片元资源：采样器槽 0..4 = albedo / normal / roughness / AO / macro（材质四件套 + 宏观变化），
    // 槽 5 = 阴影深度数组，槽 6..8 = 环境贴图三件套（T67：irradiance / 预过滤高光 / BRDF LUT）；
    // uniform 槽 0 = 材质、槽 1 = 光照、槽 2 = 阴影（槽 3 = 自发光，逐网格推送）。
    // 都在调用方设置过时才绑定 / 推送；引擎不解释其内容。
    const bool materialArraysReady =
        m_albedoTexture.IsValid() && m_normalTexture.IsValid() && m_roughnessTexture.IsValid() &&
        m_aoTexture.IsValid() && m_macroTexture.IsValid() && m_albedoTexture.id <= m_textureArrays.size() &&
        m_normalTexture.id <= m_textureArrays.size() && m_roughnessTexture.id <= m_textureArrays.size() &&
        m_aoTexture.id <= m_textureArrays.size() && m_macroTexture.id <= m_textureArrays.size();
    if (materialArraysReady) {
        // 环境贴图未就绪时绑 1×1 占位纹理：着色器恒声明 binding 6..8（SDL_gpu 要求声明的采样器都有绑定），
        // 而此时光照 uniform 的 IBL 启用位为 0 ⇒ 着色器整段跳过采样，占位内容不会被用到。
        const bool environmentReady = m_environmentReady && m_irradianceTexture != nullptr &&
                                      m_prefilterTexture != nullptr && m_brdfLutTexture != nullptr;

        SDL_GPUTextureSamplerBinding bindings[9] = {};
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
        bindings[6].texture = environmentReady ? m_irradianceTexture : m_environmentPlaceholder;
        bindings[6].sampler = m_environmentSampler;
        bindings[7].texture = environmentReady ? m_prefilterTexture : m_environmentPlaceholder;
        bindings[7].sampler = m_environmentSampler;
        // BRDF LUT 用**复用的 HDR 采样器**（clamp 寻址 + 线性 + 不采 mip）：正是 LUT 需要的采样行为
        // （LUT 不是等距柱状、不该在 U 方向回绕），故不为它单建采样器。
        bindings[8].texture = environmentReady ? m_brdfLutTexture : m_environmentPlaceholder;
        bindings[8].sampler = m_hdrSampler;
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 9);
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
    // 阴影（T21b）：片元 uniform 槽 2。槽 3（自发光）由 `DrawMeshes` **逐网格**推送。
    if (m_shadowUniformValid) {
        SDL_PushGPUFragmentUniformData(commandBuffer, 2, &m_shadowUniform,
                                       static_cast<Uint32>(sizeof(ShadowUniform)));
    }

    // T39：主通道的推送去重状态（每帧新建 ⇒ 通道开始时"从未推送过"，行为与逐网格推送等价）。
    // T41 / T33：逐网格模型变换同样逐网格推送，主通道另有自己的一份去重状态。
    EmissivePushState      emissiveState;
    MeshTransformPushState transformState;
    // T78：主通道传入带深度偏移的管线变体 —— `DrawMeshes` 只在地表 tile（标记为 true）那一段切换过去。
    // T69：蒙皮网格（主角）走 `m_skinnedPipeline`，其骨骼矩阵经顶点 storage buffer 的 binding 1 绑定。
    DrawMeshes(commandBuffer, pass, meshes, meshCount, /*pushEmissive=*/true, /*basePipeline=*/m_pipeline,
               /*depthBiasedPipeline=*/m_pipelineDepthBiased, /*skinnedPipeline=*/m_skinnedPipeline,
               /*primaryStorageBuffer=*/m_cameraUniformBuffer, emissiveState, transformState,
               /*waterPipeline=*/nullptr, /*waterPass=*/false);

    // V0.7 H1：实例化物件（不透明）—— 与地表 / 体积共用同一批采样器与片元 uniform（本通道前面已绑 / 已推），
    // 每原型**一次**绘制（`num_instances = 本帧可见实例数`）⇒ draw call 与物件数解耦（ADR 0034 决策一）。
    // 放在水面通道**之前**（水面是其后的半透明叠加层，需最后绘制）。
    DrawInstancedBatches(commandBuffer, pass, batches, batchCount, m_instancedPipeline,
                         /*matrixStorageBuffer=*/m_cameraUniformBuffer, /*pushEmissive=*/true, emissiveState,
                         transformState);

    // ---- W6：水面通道（ADR 0027）—— 主通道内**最后**绘制，半透明叠加在地形之上 ----
    // 为什么单独一遍：`water.frag` 的唯一 uniform 块在**片元槽 0**，而该槽平时被"材质"占用
    // （见上面的 `m_materialUniform` 推送）⇒ 必须在水面绘制之前把它换成水面参数；水面画完即结束本通道，
    // 故不会影响任何其它绘制。水面**不投影阴影**（阴影通道传 waterPass=false ⇒ 自动跳过）。
    if (m_waterPipeline != nullptr) {
        struct WaterUniform {
            float params[4];  ///< x = 时间（秒）、y = 流速倍率、z/w = 预留
            float origin[4];  ///< 渲染原点（float 近似；与 `SetRenderOrigin` 一致）
        };
        WaterUniform water {};
        water.params[0] = m_waterTime;
        water.params[1] = 1.0F;
        water.origin[0] = static_cast<float>(m_renderOrigin.x);
        water.origin[1] = static_cast<float>(m_renderOrigin.y);
        water.origin[2] = static_cast<float>(m_renderOrigin.z);
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &water, static_cast<Uint32>(sizeof(water)));

        EmissivePushState      waterEmissiveState;
        MeshTransformPushState waterTransformState;
        DrawMeshes(commandBuffer, pass, meshes, meshCount, /*pushEmissive=*/false,
                   /*basePipeline=*/m_waterPipeline, /*depthBiasedPipeline=*/nullptr, /*skinnedPipeline=*/nullptr,
                   /*primaryStorageBuffer=*/m_cameraUniformBuffer, waterEmissiveState, waterTransformState,
                   /*waterPipeline=*/m_waterPipeline, /*waterPass=*/true);
    }

    SDL_EndGPURenderPass(pass);

    // ---- 色调映射通道（T20 / ADR 0010 P0）：HDR 目标 → 交换链 ----
    // 全屏三角形覆盖整个交换链，因此无需保留上一帧内容（DONT_CARE 加载）。
    SDL_GPUColorTargetInfo tonemapTarget {};
    tonemapTarget.texture  = swapchain;
    tonemapTarget.load_op  = SDL_GPU_LOADOP_DONT_CARE;
    tonemapTarget.store_op = SDL_GPU_STOREOP_STORE;  // 叠加层稍后以 LOAD 追加，故必须存储

    // 色调映射片元 uniform（std140：单个 vec4，x = 曝光）；由 `DrawFullscreenPass` 在开通道前推送。
    struct TonemapUniform {
        float exposureParams[4];
    };
    TonemapUniform tonemapUniform {};
    tonemapUniform.exposureParams[0] = m_exposure;

    // T67：全屏三角通道的公共形状（开通道 → 绑管线 → 绑采样器 → 推 uniform → 画 3 顶点 → 关通道）
    // 抽到 `DrawFullscreenPass`，色调映射与三条 IBL 烘焙 pass 共用（四处逐字相同）。
    const SDL_GPUTextureSamplerBinding hdrBinding { m_hdrTexture, m_hdrSampler };
    DrawFullscreenPass(commandBuffer, tonemapTarget, m_tonemapPipeline, &hdrBinding, 1, &tonemapUniform,
                       static_cast<std::uint32_t>(sizeof(tonemapUniform)));

    // 叠加层：与 3D 通道共用本命令缓冲（交换链纹理只在获取它的命令缓冲里有效）。
    if (overlay != nullptr) {
        overlay->DrawOverlay(commandBuffer, swapchain, width, height);
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
    return true;
}

}  // namespace vx
