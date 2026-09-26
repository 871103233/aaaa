#pragma once

#include "render/camera.hpp"
#include "render/shadow_cascade.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 地表 / 体积网格的**通用**顶点格式。
///
/// 刻意不含任何方块 / 体素语义：没有面朝向枚举、没有方块 ID、没有 UV 层号。
/// 顶点由世界层按**平滑曲面**产出，渲染侧只按下面的固定布局消费。
///
/// 布局与 Shader 的顶点输入位置一一对应（见 mesh_renderer.cpp 的管线描述）：
///   - location 0 `vec3 position` —— **相机相对坐标**（红线 6：世界定位不用 `float`；
///                                  世界层上传前做相机相对偏移）
///   - location 1 `vec3 normal`   —— 世界空间单位法线（由高度场 / 密度场梯度算出，
///                                  禁止用面法线近似）
///
/// **不再承载材质权重**（ADR 0009）：权重由片元着色器按世界高度与坡度**逐像素**重算，
/// 过渡带宽因此由几何曲率决定、不受顶点间距限制。片元用 uniform 的**渲染原点**把这里的
/// 相机相对坐标还原为世界坐标（见 `SetMaterialUniform`）。
struct MeshVertex {
    float position[3] = { 0.0F, 0.0F, 0.0F };
    float normal[3]   = { 0.0F, 1.0F, 0.0F };
};

/// 一份待上传的网格数据（顶点 + 32 位索引）。
struct MeshData {
    std::vector<MeshVertex>    vertices;
    std::vector<std::uint32_t> indices;
};

/// 每帧相机常量：与 Shader 中的相机 storage buffer 布局一一对应。
/// `mat4` 在 std140 下为 4 个 `vec4`，无隐式填充，可直接整块上传。
struct CameraUniform {
    glm::mat4 viewProjection { 1.0F };
};

/// 网格资源的 GPU 句柄。`id == 0` 表示无效句柄。
struct MeshHandle {
    std::uint32_t id = 0;

    [[nodiscard]] bool IsValid() const noexcept { return id != 0; }
};

/// 通用 2D 纹理数组的 GPU 句柄。`id == 0` 表示无效句柄。
struct TextureArrayHandle {
    std::uint32_t id = 0;

    [[nodiscard]] bool IsValid() const noexcept { return id != 0; }
};

/// 通用 2D 纹理数组的创建描述。**不含任何世界 / 地形语义**：引擎只按字节上传，不解释内容。
///
/// 约定：像素格式固定为 `R8G8B8A8_UNORM`，数据为 layer-major 连续的第 0 级
/// （层 L 的像素从 `L * width * height * 4` 开始）；mip 链由引擎用 SDL 生成。
/// 前置条件：`pixels` 至少 `width * height * 4 * layerCount` 字节。
struct TextureArrayDesc {
    std::uint32_t       width      = 0;
    std::uint32_t       height     = 0;
    std::uint32_t       layerCount = 0;
    const std::uint8_t* pixels     = nullptr;
};

/// 材质 uniform 块的最大字节数（`SetMaterialUniform` 的容量上限）。
/// 当前片元块 = 渲染原点（`vec4`）+ 4 层 × 4 个 `vec4` = 272 字节（ADR 0010 P2 起每层含
/// roughness / ao / 宏观参数）；留余量给后续参数。
inline constexpr std::size_t kMaxMaterialUniformBytes = 512;

/// 光照 uniform 块的最大字节数（`SetLightingUniform` 的容量上限）。
/// 当前片元块 = 8 个 `vec4` = 128 字节（见 `render/lighting_table.hpp` 的 `LightingUniform`）；留余量给后续参数。
inline constexpr std::size_t kMaxLightingUniformBytes = 256;

/// 一帧渲染的开销统计（**通用**：不含任何世界 / 地形语义，供调试设施只读展示）。
///
/// 每帧 `RenderFrame` 开始时把 `drawCalls` / `triangleCount` / `vertexCount` 清零，
/// 并按**实际执行**的索引绘制调用累加。`textureBytes` 是引擎当前持有的 GPU 纹理字节总量
/// （显存记账，ADR 0010 的强制义务）：随纹理 / 目标的创建与释放增减，不随帧变化。
struct RenderStats {
    std::uint32_t drawCalls     = 0;  ///< 本帧实际执行的索引绘制调用次数
    std::uint64_t triangleCount = 0;  ///< 本帧实际绘制的三角形数（Σ `indexCount / 3`）
    std::uint64_t vertexCount   = 0;  ///< 本帧实际绘制的顶点数（Σ `vertexCount`）
    std::uint64_t textureBytes  = 0;  ///< 当前持有的纹理显存字节总量（含 mip 链）
};

/// 纯函数：纹理数组（`R8G8B8A8_UNORM`，4 字节 / 像素）的显存估算，**含完整 mip 链**。
///
/// mip 链各级面积之和收敛到第 0 级的 4/3（几何级数），故取
/// `4/3 × width × height × 4 × layerCount`。独立成纯函数以便单测：不读全局、不分配。
[[nodiscard]] inline std::uint64_t EstimateTextureArrayBytes(std::uint32_t width, std::uint32_t height,
                                                             std::uint32_t layerCount) noexcept {
    const std::uint64_t base = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL *
                               static_cast<std::uint64_t>(layerCount);
    return base * 4ULL / 3ULL;
}

/// 帧末叠加层：与 3D 主通道**共用同一个命令缓冲**，在提交前绘制覆盖内容（调试面板等 UI）。
///
/// 为什么需要这个钩子：交换链纹理只能在**获取它的那个命令缓冲**里引用，跨命令缓冲再获取一次会重复呈现；
/// 因此叠加内容必须由 `MeshRenderer` 在 3D 通道结束后、`Submit` 之前回调绘制。
/// 引擎层**不认识任何具体 UI 实现**（不含 ImGui 类型）：SDK 使用者自行实现本接口。
///
/// 线程约定：`DrawOverlay` 在渲染线程、`MeshRenderer::RenderFrame` 内部被调用。
class IRenderOverlay {
public:
    virtual ~IRenderOverlay() = default;

    IRenderOverlay(const IRenderOverlay&) = delete;
    IRenderOverlay& operator=(const IRenderOverlay&) = delete;
    IRenderOverlay(IRenderOverlay&&) = delete;
    IRenderOverlay& operator=(IRenderOverlay&&) = delete;

    /// 在 3D 通道之后、命令缓冲提交之前绘制叠加内容。
    /// 前置条件：`commandBuffer` 已获取交换链纹理，`swapchain` 为本次渲染目标。
    virtual void DrawOverlay(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* swapchain, std::uint32_t width,
                             std::uint32_t height) = 0;

protected:
    IRenderOverlay() = default;
};

/// 通用网格渲染路径：图形管线 + 顶点 / 索引缓冲 + 每帧相机常量 + 索引绘制。
///
/// 与 `TriangleRenderer` 的分工：后者是 PoC 冒烟路径（顶点写死在 Shader 内、无相机），
/// 用于验证"设备 + 管线 + 交换链 + 双格式 Shader"这条链路；本类才是真正的网格路径。
/// 两者并存，冒烟路径保持不变。
///
/// Shader 约定（由构建期两段式管线产出双格式，见 ADR 0002）：
///   - 顶点着色器入口 `main`：消费上面 `MeshVertex` 的两个 location，
///     并绑定一个只读 storage buffer（slot 0，内容为 `CameraUniform`）；
///   - 片元着色器入口 `main`：采样六个纹理数组（slot 0..4 = albedo / normal / roughness / AO / macro
///     材质四件套与宏观变化，slot 5 = 阴影深度数组）
///     并读取三个 uniform 块（slot 0 = 材质，slot 1 = 光照；slot 2 = 阴影，见 `SetShadowCascades`）。
///   - 阴影通道另用 `shadow.vert` + 空入口 `shadow.frag`：无颜色目标、只写深度，
///     顶点 slot 0 绑定该级的光空间矩阵（与相机矩阵**同类**机制：`SDL_BindGPUVertexStorageBuffers`）。
///   产物路径为 `<shader_dir>/<shader_name>.vert{.spv|.dxil}` 与 `<shader_name>.frag{...}`，
///   缺失时构造函数抛 `std::runtime_error`（与 `TriangleRenderer` 一致）。
///
/// 线程约定：只能在渲染线程调用（图形上下文归属创建它的线程）。
class MeshRenderer {
public:
    MeshRenderer(SDL_GPUDevice* device, SDL_Window* window, std::filesystem::path shader_dir,
                 std::string shader_name = "mesh");
    ~MeshRenderer();

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    /// 创建并同步上传一个网格。空网格（顶点或索引为空）返回无效句柄。
    ///
    /// 前置条件：`mesh` 的索引为 32 位且都在顶点范围内。
    /// 注意：上传会阻塞到 GPU 完成，只应在加载 / 生成阶段调用，**不得**放进每帧热路径。
    [[nodiscard]] MeshHandle UploadMesh(const MeshData& mesh);

    /// 用一个**顶点数不变**的新顶点数组就地刷新已上传网格的顶点缓冲；索引缓冲保持不变。
    ///
    /// 与 `UploadMesh` 的区别：复用既有 GPU 缓冲与一个常驻暂存缓冲，**不创建 GPU 资源、不做同步等待**，
    /// 因而可用于每帧改写"相机相对顶点"的**动态**网格（例如主角胶囊体）。
    /// 返回 false 表示句柄无效、槽位已释放，或 `vertices.size()` 与上传时不一致。
    [[nodiscard]] bool UpdateMeshVertices(MeshHandle handle, const std::vector<MeshVertex>& vertices);

    /// 释放一个网格的 GPU 资源；无效句柄为无操作。
    void ReleaseMesh(MeshHandle handle) noexcept;

    /// 创建并同步上传一个 2D 纹理数组（采样用，含由 SDL 生成的完整 mip 链）。
    ///
    /// 前置条件：`desc.width/height/layerCount ≥ 1` 且 `desc.pixels` 非空。
    /// 注意：会阻塞到 GPU 完成，只应在加载 / 生成阶段调用，**不得**放进每帧热路径。
    /// 失败时抛 `std::runtime_error`（启动 / 加载期允许异常）。
    [[nodiscard]] TextureArrayHandle CreateTextureArray(const TextureArrayDesc& desc);

    /// 释放一个纹理数组的 GPU 资源；无效句柄为无操作。
    void ReleaseTextureArray(TextureArrayHandle handle) noexcept;

    /// 绑定五个材质纹理数组到网格着色器的采样槽 0..4
    /// （albedo / normal / roughness / AO / macro）。
    ///
    /// 槽序与 `assets/shaders/mesh.frag` 的 `set = 2, binding = 0..4` 一致；`macro` 为**单层**数组。
    void SetSampledTextureArrays(TextureArrayHandle albedo, TextureArrayHandle normal, TextureArrayHandle roughness,
                                 TextureArrayHandle ao, TextureArrayHandle macro) noexcept;

    /// 设置片元着色器的材质 uniform 块（**原样字节**，引擎不解释其语义）。
    ///
    /// 需与 mesh.frag 的 std140 `set = 3, binding = 0` 布局一致（块内容由世界层构建）。
    /// 只做拷贝、不分配；`size > kMaxMaterialUniformBytes` 时忽略。
    void SetMaterialUniform(const void* data, std::size_t size) noexcept;

    /// 设置片元着色器的光照 uniform 块（**原样字节**，引擎不解释其语义），与 `SetMaterialUniform` 完全同构。
    ///
    /// 需与 mesh.frag 的 std140 `set = 3, binding = 1` 布局一致（块内容由 `BuildLightingUniform` 构建）。
    /// 之所以独立成第二个槽位：SDL3_gpu 的片元阶段有 4 个 uniform 槽，槽 0 已被材质占用，光照用槽 1。
    /// 只做拷贝、不分配；`size > kMaxLightingUniformBytes` 时忽略。
    void SetLightingUniform(const void* data, std::size_t size) noexcept;

    /// 设置本帧的阴影级联参数（T21b / ADR 0010 P1）。
    ///
    /// `uniform` 是 CPU 构建的片元槽 2 块（由 `BuildShadowUniform` 投影，见 `shadow_cascade.hpp`）；
    /// `cascadeCount` / `resolution` 是深度数组的层数与边长（来自配置，**与相机无关**）。
    /// 引擎只做拷贝与记录：**不创建资源、不分配**；深度数组按需在 `RenderFrame` 内惰性重建，
    /// 级数 / 分辨率变化时自动重建（参照 `EnsureDepthTarget` 的写法）。
    /// 前置条件：`cascadeCount ∈ [1, kMaxShadowCascades]`；`resolution` 是 2 的幂且 ≥ 256（越界会被钳制）。
    void SetShadowCascades(const ShadowUniform& uniform, std::uint32_t cascadeCount,
                           std::uint32_t resolution) noexcept;

    /// 设置本帧相机常量；下一次 `RenderFrame` 生效。
    void SetCamera(const CameraView& camera) noexcept;

    /// 设置色调映射的曝光系数；下一次 `RenderFrame` 生效。
    ///
    /// 取值来自配置文件（`platform/settings.hpp` 的 `exposure`，已在载入时钳制），
    /// 引擎不在此解释语义（ADR 0010：参数进配置，改值不需重编 Shader）。
    void SetExposure(float exposure) noexcept { m_exposure = exposure; }

    /// 设置 MSAA 档位（T23 / ADR 0010 P3）；下一次 `RenderFrame` 生效。
    ///
    /// 取值来自上层（`game/` 读 `settings.toml` 的 `msaa_samples` 后传入，**引擎层不读配置文件**）。
    /// `1` = 关闭 MSAA（**零额外开销**：不创建 MSAA 纹理，直接渲进单采样 HDR 目标 + 单采样深度）；
    /// `2 / 4 / 8` = 主通道渲进多采样颜色目标 + 多采样深度，再解析（resolve）到单采样 HDR 目标，
    /// 色调映射通道**始终读单采样 HDR 目标**（不读 MSAA 纹理，故 MSAA 纹理 `usage` 只需 `COLOR_TARGET`）。
    ///
    /// 引擎**不定义档位口径**（那是配置层的 `ClampMsaaSampleCount`）；这里只做**硬件能力**适配：
    /// 若请求档位不被当前设备支持（`SDL_GPUTextureSupportsSampleCount`），会向下取受支持的最高档，
    /// 以保证**管线采样数与渲染目标采样数一致**（否则 `SDL_BeginGPURenderPass` 会报错）。
    /// 档位变化与硬件降级都会记一条日志（含生效档位）。
    void SetMsaaSampleCount(std::uint32_t sampleCount) noexcept;

    /// 只读：渲染开销统计与纹理显存记账（见 `RenderStats`）。
    ///
    /// 其中绘制统计为**最近一次** `RenderFrame` 的实测值；纹理字节为当前总量。
    [[nodiscard]] const RenderStats& Stats() const noexcept { return m_stats; }

    /// 渲染一帧：网格渲到离屏 HDR 目标 → 色调映射到交换链 → 可选的叠加层。
    ///
    /// 顺序（ADR 0010 的 P0 / P3）：主通道写 `R16G16B16A16_FLOAT` HDR 颜色目标（+ 深度），
    /// 随后全屏三角形做「曝光 + ACES 近似 + sRGB 编码」输出到交换链；叠加层最后以交换链为目标绘制，
    /// **不**被色调映射处理。MSAA 档位 > 1 时，主通道写多采样颜色目标并在同一渲染通道内 resolve 到
    /// 该 HDR 目标（`SDL_GPUColorTargetInfo::resolve_texture`），色调映射通道仍采样单采样 HDR 目标。
    ///
    /// `meshes` 中被跳过的情况：指针为空、句柄无效、或该槽位已释放。
    /// 返回 false 表示本帧拿不到交换链纹理（如窗口最小化），调用方可直接跳过。
    [[nodiscard]] bool RenderFrame(const MeshHandle* meshes, std::size_t meshCount, const SDL_FColor& clearColor,
                                   IRenderOverlay* overlay = nullptr);

private:
    struct MeshResources {
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUBuffer* indexBuffer  = nullptr;
        std::uint32_t  vertexCount  = 0;
        std::uint32_t  indexCount   = 0;
    };

    /// 保证主通道图形管线与请求的 MSAA 档位一致（档位变化时用常驻 Shader 重建）。
    void EnsureMainPipeline(std::uint32_t sampleCount);

    /// 按给定档位创建主通道图形管线（用常驻的 `m_meshVertexShader` / `m_meshFragmentShader`），
    /// 写入 `m_pipeline` 与 `m_pipelineSampleCount`；失败抛 `std::runtime_error`。
    void CreateMainPipeline(std::uint32_t sampleCount);

    /// 保证深度目标与当前交换链尺寸、MSAA 档位一致（尺寸或档位变化时重建）。
    /// 档位 = 1 即单采样深度（回到 P0 行为）；档位 > 1 为多采样深度（`D32_FLOAT`，`sample_count = 档位`）。
    void EnsureDepthTarget(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount);

    /// 保证离屏 HDR 颜色目标（`R16G16B16A16_FLOAT`）与当前交换链尺寸一致（尺寸变化时重建）。
    void EnsureHdrTarget(std::uint32_t width, std::uint32_t height);

    /// 保证 MSAA 颜色目标（`R16G16B16A16_FLOAT`，`sample_count = 档位`）与尺寸 / 档位一致。
    /// **档位 = 1 时释放该纹理并保持空指针**（零开销路径：主通道直接渲进单采样 HDR 目标）。
    void EnsureMsaaColorTarget(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount);

    /// 保证阴影深度数组（`D32_FLOAT`，`2D_ARRAY`，层数 = 级数）与请求的级数 / 分辨率一致（变化时重建）。
    void EnsureShadowTarget();

    /// 把 `m_shadowUniform.lightMatrices`（各级光空间矩阵）上传到每级的顶点只读 storage buffer。
    void UploadShadowMatrices(SDL_GPUCommandBuffer* commandBuffer);

    /// 绑定并绘制一批网格（主通道与阴影通道共用同一实现）；同时累加本帧绘制统计。
    /// 前置条件：调用方已绑定图形管线（两通道的顶点输入布局一致，均为 `MeshVertex`）。
    void DrawMeshes(SDL_GPURenderPass* pass, const MeshHandle* meshes, std::size_t meshCount);

    /// 把纹理显存**按项**打到日志（材质数组 / 深度 / HDR / 阴影），供预算核对（ADR 0010 记账义务）。
    void LogTextureAccounting(std::uint32_t width, std::uint32_t height) const;

    /// 把 `m_cameraUniform` 传到相机常量的 GPU 缓冲。
    void UploadCameraUniform(SDL_GPUCommandBuffer* commandBuffer);

    SDL_GPUDevice*           m_device   = nullptr;
    SDL_Window*              m_window   = nullptr;
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;

    /// 主通道管线**常驻**的 Shader 对象（T23）：档位变化时需按新的采样数重建管线，
    /// 故不再"创建管线后立即释放"，而是持有到析构（释放时机对渲染结果无影响）。
    SDL_GPUShader* m_meshVertexShader   = nullptr;
    SDL_GPUShader* m_meshFragmentShader = nullptr;

    /// `m_pipeline` 当前烘焙的采样数档位（必须与渲进的目标一致；不一致时 `EnsureMainPipeline` 重建）。
    std::uint32_t m_pipelineSampleCount = 1;

    /// 色调映射管线：全屏三角形，HDR 颜色目标 → 交换链（无深度、不剔除）。
    SDL_GPUGraphicsPipeline* m_tonemapPipeline = nullptr;

    /// 阴影深度管线：**仅顶点着色器**、无颜色目标、深度目标 = `D32_FLOAT` 深度数组的一层。
    SDL_GPUGraphicsPipeline* m_shadowPipeline = nullptr;

    SDL_GPUBuffer*         m_cameraUniformBuffer  = nullptr;
    SDL_GPUTransferBuffer* m_cameraTransferBuffer = nullptr;
    CameraUniform          m_cameraUniform {};

    SDL_GPUTexture* m_depthTexture = nullptr;
    std::uint32_t   m_depthWidth   = 0;
    std::uint32_t   m_depthHeight  = 0;
    std::uint32_t   m_depthSampleCount = 1;  ///< 深度目标的采样数档位（1 = 单采样）
    std::uint64_t   m_depthBytes   = 0;  ///< 深度目标的显存记账（字节，已折入 MSAA 采样数）

    /// 离屏 HDR 颜色目标（主通道写入 / resolve 目标、色调映射通道采样）。
    SDL_GPUTexture* m_hdrTexture = nullptr;
    std::uint32_t   m_hdrWidth   = 0;
    std::uint32_t   m_hdrHeight  = 0;
    std::uint64_t   m_hdrBytes   = 0;  ///< HDR 目标的显存记账（字节）

    // ---- MSAA（T23 / ADR 0010 P3）：多采样颜色目标 + 档位 ----

    /// 请求的 MSAA 档位（`SetMsaaSampleCount` 写入；仅用于判断是否需要重打日志）。
    std::uint32_t m_requestedMsaaSampleCount = 1;

    /// **生效**的 MSAA 档位（`SetMsaaSampleCount` 按硬件能力归一后写入；1 = 关闭）。
    std::uint32_t m_msaaSampleCount = 1;

    /// 是否已至少打印过一次 MSAA 档位日志：保证**启动期一定记一次生效档位**，
    /// 同时允许调用方重复传入同一档位时不刷屏。
    bool m_msaaSampleCountLogged = false;

    /// 多采样颜色目标（`R16G16B16A16_FLOAT`、`sample_count = 档位`、`usage` 仅 `COLOR_TARGET`）。
    /// **档位 = 1 时为空指针**（不创建，走零开销路径）。
    SDL_GPUTexture* m_msaaColorTexture       = nullptr;
    std::uint32_t   m_msaaWidth              = 0;
    std::uint32_t   m_msaaHeight             = 0;
    std::uint32_t   m_msaaColorSampleCount   = 0;
    std::uint64_t   m_msaaColorBytes         = 0;  ///< MSAA 颜色目标的显存记账（字节）

    /// 色调映射采样 HDR 目标用的采样器（clamp 寻址 + 线性过滤，单级）。
    SDL_GPUSampler* m_hdrSampler = nullptr;

    /// 曝光系数（由 `SetExposure` 写入，随色调映射 uniform 上传）。
    float m_exposure = 1.0F;

    // ---- 阴影（T21b / ADR 0010 P1）：深度数组 + 每级矩阵缓冲 + 片元槽 2 的参数块 ----

    /// 阴影深度数组（`D32_FLOAT`、`2D_ARRAY`、层数 = 级数）；级数 / 分辨率变化时重建。
    SDL_GPUTexture* m_shadowTexture           = nullptr;
    std::uint32_t   m_shadowTextureResolution = 0;
    std::uint32_t   m_shadowTextureCascades   = 0;
    std::uint64_t   m_shadowTextureBytes      = 0;  ///< 阴影深度数组的显存记账（字节）

    /// 本帧请求的级数与分辨率（`SetShadowCascades` 写入；纹理按需重建）。
    std::uint32_t m_shadowCascadeCount = 1;
    std::uint32_t m_shadowResolution   = 256;

    /// 每级一个只读 storage buffer（存该级光空间矩阵）。SDL_gpu 的 storage buffer 绑定**不带偏移**，
    /// 故每级一个缓冲、渲染该级时绑定对应缓冲。属缓冲资源，不计入 `textureBytes`。
    std::array<SDL_GPUBuffer*, kMaxShadowCascades> m_shadowMatrixBuffers {};

    /// 上传 4 级矩阵共用的常驻暂存缓冲（每帧一次 map + 逐级 copy）。
    SDL_GPUTransferBuffer* m_shadowMatrixTransfer = nullptr;

    /// 本帧阴影 uniform（片元槽 2）与其有效标志（`SetShadowCascades` 写入）。
    ShadowUniform m_shadowUniform {};
    bool          m_shadowUniformValid = false;

    /// 阴影采样器：clamp 寻址 + **最近邻**（手动 3×3 PCF，深度值不插值）+ 不采样 mip。
    SDL_GPUSampler* m_shadowSampler = nullptr;

    /// 任一渲染目标在本次 `RenderFrame` 中被（重）创建 → 重新打印一次显存记账日志。
    bool m_textureAccountingDirty = false;

    /// 渲染开销统计与显存记账（`Stats()` 暴露）。
    RenderStats m_stats {};

    std::vector<MeshResources> m_meshes;
    std::vector<std::uint32_t> m_freeSlots;

    // ---- 纹理数组与材质 uniform（通用，引擎不解释内容）----

    /// 复用的采样器：repeat 寻址 + 线性过滤 + mipmap 线性（创建一次，全体纹理数组共用）。
    SDL_GPUSampler* m_layerSampler = nullptr;

    std::vector<SDL_GPUTexture*> m_textureArrays;
    std::vector<std::uint64_t>   m_textureArrayBytes;  ///< 与 `m_textureArrays` 同槽位：每张纹理的记账字节
    std::vector<std::uint32_t>   m_freeTextureSlots;
    TextureArrayHandle           m_albedoTexture;
    TextureArrayHandle           m_normalTexture;
    TextureArrayHandle           m_roughnessTexture;
    TextureArrayHandle           m_aoTexture;
    TextureArrayHandle           m_macroTexture;

    /// 片元 uniform 块的暂存字节（固定容量，`SetMaterialUniform` 只做 memcpy、不分配）。
    std::array<std::uint8_t, kMaxMaterialUniformBytes> m_materialUniform {};
    std::size_t                                        m_materialUniformSize = 0;

    /// 光照 uniform 块的暂存字节（固定容量，`SetLightingUniform` 只做 memcpy、不分配）。
    std::array<std::uint8_t, kMaxLightingUniformBytes> m_lightingUniform {};
    std::size_t                                        m_lightingUniformSize = 0;

    /// `UpdateMeshVertices` 复用的常驻暂存缓冲：容量足够时**不**重新分配（避免每帧堆分配）。
    SDL_GPUTransferBuffer* m_vertexStagingBuffer   = nullptr;
    std::uint32_t          m_vertexStagingCapacity = 0;
};

}  // namespace vx
