#pragma once

#include "render/camera.hpp"

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
/// 当前片元块 = 渲染原点（`vec4`）+ 4 层 × 3 个 `vec4` = 208 字节；留余量给后续参数。
inline constexpr std::size_t kMaxMaterialUniformBytes = 512;

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
///   - 片元着色器入口 `main`：采样两个纹理数组（slot 0 / 1，见 `SetSampledTextureArrays`）
///     并读取一个 uniform 块（slot 0，见 `SetMaterialUniform`）。
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

    /// 绑定两个纹理数组到网格着色器的采样槽 0（albedo）/ 1（normal）。
    /// 槽序与 `assets/shaders/mesh.frag` 的 `set = 2, binding = 0/1` 一致。
    void SetSampledTextureArrays(TextureArrayHandle albedo, TextureArrayHandle normal) noexcept;

    /// 设置片元着色器的材质 uniform 块（**原样字节**，引擎不解释其语义）。
    ///
    /// 需与 mesh.frag 的 std140 `set = 3, binding = 0` 布局一致（块内容由世界层构建）。
    /// 只做拷贝、不分配；`size > kMaxMaterialUniformBytes` 时忽略。
    void SetMaterialUniform(const void* data, std::size_t size) noexcept;

    /// 设置本帧相机常量；下一次 `RenderFrame` 生效。
    void SetCamera(const CameraView& camera) noexcept;

    /// 渲染一帧：清屏（颜色 + 深度）→ 绑定相机常量 → 依次索引绘制 → 可选的叠加层。
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

    /// 保证深度目标与当前交换链尺寸一致（尺寸变化时重建）。
    void EnsureDepthTarget(std::uint32_t width, std::uint32_t height);

    /// 把 `m_cameraUniform` 传到相机常量的 GPU 缓冲。
    void UploadCameraUniform(SDL_GPUCommandBuffer* commandBuffer);

    SDL_GPUDevice*           m_device   = nullptr;
    SDL_Window*              m_window   = nullptr;
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;

    SDL_GPUBuffer*         m_cameraUniformBuffer  = nullptr;
    SDL_GPUTransferBuffer* m_cameraTransferBuffer = nullptr;
    CameraUniform          m_cameraUniform {};

    SDL_GPUTexture* m_depthTexture = nullptr;
    std::uint32_t   m_depthWidth   = 0;
    std::uint32_t   m_depthHeight  = 0;

    std::vector<MeshResources> m_meshes;
    std::vector<std::uint32_t> m_freeSlots;

    // ---- 纹理数组与材质 uniform（通用，引擎不解释内容）----

    /// 复用的采样器：repeat 寻址 + 线性过滤 + mipmap 线性（创建一次，全体纹理数组共用）。
    SDL_GPUSampler* m_layerSampler = nullptr;

    std::vector<SDL_GPUTexture*> m_textureArrays;
    std::vector<std::uint32_t>   m_freeTextureSlots;
    TextureArrayHandle           m_albedoTexture;
    TextureArrayHandle           m_normalTexture;

    /// 片元 uniform 块的暂存字节（固定容量，`SetMaterialUniform` 只做 memcpy、不分配）。
    std::array<std::uint8_t, kMaxMaterialUniformBytes> m_materialUniform {};
    std::size_t                                        m_materialUniformSize = 0;

    /// `UpdateMeshVertices` 复用的常驻暂存缓冲：容量足够时**不**重新分配（避免每帧堆分配）。
    SDL_GPUTransferBuffer* m_vertexStagingBuffer   = nullptr;
    std::uint32_t          m_vertexStagingCapacity = 0;
};

}  // namespace vx
