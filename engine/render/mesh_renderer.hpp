#pragma once

#include "render/camera.hpp"
#include "render/shadow_cascade.hpp"

#include <SDL3/SDL.h>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 材质槽位**覆盖**的"未指定"值（[ADR 0014](../../docs/adr/0014-voxel-material-index.md)）。
///
/// 片元遇到它时才按世界高度与坡度**逐像素**算权重（ADR 0009 的地表路径）；
/// 可挖体积的内表面则携带**具体槽位**，直接用它切开的那种材质 —— 否则洞底（位于地下、坡度 0）
/// 会被材质规则判成"草"，出现"地下草地"。
inline constexpr float kNoMaterialOverride = -1.0F;

/// 地表 / 体积网格的**通用**顶点格式。
///
/// 刻意不含任何方块 / 体素语义：没有面朝向枚举、没有方块 ID、没有 UV 层号。
/// 顶点由世界层按**平滑曲面**产出，渲染侧只按下面的固定布局消费。
///
/// 布局与 Shader 的顶点输入位置一一对应（见 mesh_renderer.cpp 的管线描述）：
///   - location 0 `vec3 position` —— **网格局部坐标**（T41）：地表 tile 内（0..64 格）或可挖体积块内（0..32 格）。
///                                  "这块网格在世界哪里"不由顶点承载，而由**上传时登记、绘制时推送**的
///                                  网格原点 `origin` 给出（`SDL_PushGPUVertexUniformData`，偏移 = `origin − 渲染原点`）。
///                                  世界定位仍用整数 / `double`（红线 6），顶点 float 因此只承载小数值。
///                                  动态网格（角色 / 光球）是例外：它们每帧烘焙**渲染相对**顶点、
///                                  并把 `origin` 设为**当前渲染原点**（偏移恒 0）。
///   - location 1 `vec3 normal`   —— 世界空间单位法线（由高度场 / 密度场梯度算出，
///                                  禁止用面法线近似）
///   - location 2 `float material` —— **材质槽位覆盖**（ADR 0014）：`kNoMaterialOverride` = 由片元
///                                  按高度 / 坡度算（地表）；否则直接取该槽位（可挖体积的内表面）
///
/// **不承载材质权重**（ADR 0009）：权重由片元着色器按世界高度与坡度**逐像素**重算，
/// 过渡带宽因此由几何曲率决定、不受顶点间距限制。片元用 uniform 的**渲染原点**把这里的顶点
/// 还原为世界坐标（见 `SetMaterialUniform`）。
struct MeshVertex {
    float position[3] = { 0.0F, 0.0F, 0.0F };
    float normal[3]   = { 0.0F, 1.0F, 0.0F };
    float material    = kNoMaterialOverride;
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

/// 逐网格顶点 uniform：与 Shader 中 `set = 1, binding = 0` 的 std140 块一一对应（T41 起；T33 泛化）。
///
/// `modelToRender` = **平移(网格世界原点 − 渲染原点) × 旋转** —— 把**网格局部坐标**变成渲染相对坐标。
/// 顶点只承载网格局部坐标 ⇒ ① 渲染原点重定基只需改这一个 uniform（不再重传整世界顶点）；
/// ② 倒塌中的刚体只需每帧改这一块（**不必**在 CPU 侧重烘焙上万顶点）。
/// std140 下 `mat4` 即 4 个 `vec4`（64 字节），无隐式填充。
struct MeshTransformUniform {
    glm::mat4 modelToRender { 1.0F };
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

/// 一份**等距柱状** HDRI 的原始像素（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
///
/// 布局与 `texture_loader.hpp` 的 `ImageRgb32f` 相同，但这里**刻意不复用那个类型**：
/// 渲染器只吃"原始线性像素"，不依赖加载器（`texture_loader` 的输出由上层喂进来）。
///
/// 约定：**线性光**、float32、RGB 三通道、行主序、行间无填充、**第 0 行 = 天顶**
/// （与 `sky.frag` 的等距柱状约定一致：`v = 0` 在天顶）。
struct EnvironmentSource {
    /// 前置条件：非空，且长度 ≥ `width * height * 3`。
    const float*  pixels = nullptr;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

/// 材质 uniform 块的最大字节数（`SetMaterialUniform` 的容量上限）。
/// 当前片元块 = 渲染原点（`vec4`）+ 三平面参数（`vec4`，C 项）+ 4 层 × 4 个 `vec4` = 288 字节
/// （ADR 0010 P2 起每层含 roughness / ao / 宏观参数）；留余量给后续参数。
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

    /// 本帧**等待交换链纹理**的毫秒数（`SDL_WaitAndAcquireGPUSwapchainTexture` 内）。
    ///
    /// 为什么单独记账（T38 / `references/performance-and-hitches.md` §2）：这段等待发生在
    /// `RenderFrame` 内部，若并入"渲染提交"耗时，会把"**在空等 GPU**"误判成"CPU 忙"，
    /// 从而把卡顿定位到错误的方向。它与 draw call 数一起，是区分"CPU 忙 / GPU 忙 / 在等"的关键量。
    double swapchainWaitMs = 0.0;
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
///   - 顶点着色器入口 `main`：消费上面 `MeshVertex` 的三个 location，
///     绑定一个只读 storage buffer（set 0 / slot 0，内容为 `CameraUniform`），
///     并读取**一个顶点 uniform 块**（set 1 / slot 0，内容为 `MeshTransformUniform` —— 逐网格模型变换，见 `DrawMeshes`）；
///   - 片元着色器入口 `main`：采样九个纹理（slot 0..4 = albedo / normal / roughness / AO / macro
///     材质四件套与宏观变化，slot 5 = 阴影深度数组，**slot 6..8 = 环境贴图三件套**（T67：irradiance /
///     预过滤高光 / BRDF LUT；未烘焙时绑 1×1 占位））
///     并读取四个 uniform 块（slot 0 = 材质，slot 1 = 光照；slot 2 = 阴影，槽 3 = **自发光**，见下）。
///   - **自发光**（T27）：`UploadMesh(..., true)` 的网格在**同一条管线**里由槽 3 给出发光颜色
///     （`DrawMeshes` 逐网格推送；普通网格推零值）——不加开关分支、不加第二条管线。
///   - **天空**（T67）：`sky.frag` + `tonemap.vert` 组成的全屏三角管线，在**主通道内、网格之前**绘制
///     （深度测试与写入关闭 ⇒ 网格照常覆盖天空）；采样 slot 0 = HDRI，uniform 槽 0 = 逆视图投影。
///   - **IBL 烘焙**（T67）：`ibl_irradiance.frag` / `ibl_prefilter.frag` / `ibl_brdf_lut.frag` 三条
///     全屏三角管线，仅在 `BakeEnvironment` 内使用（加载期，不在渲染帧里）。
///   - 阴影通道另用 `shadow.vert` + 空入口 `shadow.frag`：无颜色目标、只写深度，
///     顶点 set 0 / slot 0 绑定该级的光空间矩阵（与相机矩阵**同类**机制：`SDL_BindGPUVertexStorageBuffers`），
///     set 1 / slot 0 读取**同一份**逐网格偏移（两个通道必须推同一份值，否则阴影与几何错位）。
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

    /// 创建并按（可预留的）容量上传一个网格。空网格（顶点或索引为空）返回无效句柄。
    ///
    /// `origin` 是该网格的**世界原点**（地表 tile = tile 世界原点、可挖体积块 = 块世界原点）：
    /// 顶点只承载**网格局部**坐标，绘制时由 `DrawMeshes` 用 `origin − 渲染原点` 作为逐网格偏移（T41）。
    /// 动态网格（角色 / 光球）的顶点是**渲染相对**的，传**当前渲染原点**即可（偏移恒 0）。
    ///
    /// 前置条件：`mesh` 的索引为 32 位且都在顶点范围内。
    /// `emissive = true` 时该网格的主通道绘制会带上 `SetEmissiveColor` 的自发光项（**在雾之后**叠加），
    /// 用于光球之类的自发光体；阴影通道不受影响（只写深度）。
    ///
    /// `depthBiased = true` 时该网格走**带光栅化深度偏移的主通道管线变体**（T78 / 2026-09-29）：
    /// 用于**层间共面重叠**的网格（地表 tile 与可挖体积网格在接管边界环上逐点重合 ⇒ 不偏移会 z-fighting）。
    /// 偏移让**体积面稳定胜出**（地表面被压在其下），不改几何、不留缝。阴影通道**不使用**该变体。
    ///
    /// **容量**（T75 / 2026-09-29）：缓冲容量 = `max(实际, 预留)`。**变长网格必须预留** ——
    /// 可挖体积块的 Surface Nets 网格、以及"被体积接管过又恢复"的地表 tile，都会在后续
    /// `UpdateMeshGeometry` 时变大；容量不够就只能走"重建"兜底（等价于旧路径）。
    /// **本方法不做同步等待**（内部走 `UpdateMeshGeometry` 的"提交即走"路径）⇒ 可用于生成 / 加载帧；
    /// 真正的热路径成本控制靠"预留容量 + 逐帧上传预算"（见 `references/performance-and-hitches.md`）。
    [[nodiscard]] MeshHandle UploadMesh(const MeshData& mesh, const glm::dvec3& origin, bool emissive = false,
                                       std::uint32_t reserveVertexCount = 0,
                                       std::uint32_t reserveIndexCount = 0, bool depthBiased = false);

    /// 用一个**顶点数不变**的新顶点数组就地刷新已上传网格的顶点缓冲；索引缓冲保持不变。
    ///
    /// 与 `UploadMesh` 的区别：复用既有 GPU 缓冲与一个常驻暂存缓冲，**不创建 GPU 资源、不做同步等待**，
    /// 因而可用于每帧改写"渲染相对顶点"的**动态**网格（例如主角胶囊体）。`origin` 与 `UploadMesh`
    /// 同义（动态网格传**当前渲染原点** ⇒ 偏移恒 0）；本方法同时更新该网格登记的原点，
    /// 因此渲染原点重定基后无需重传 —— 只要调用方按同一原点烘焙顶点。
    /// 返回 false 表示句柄无效、槽位已释放，或 `vertices.size()` 与上传时不一致。
    [[nodiscard]] bool UpdateMeshVertices(MeshHandle handle, const std::vector<MeshVertex>& vertices,
                                          const glm::dvec3& origin);

    /// 就地更新一个**变长**网格的几何（顶点 + 索引），**只上传用到的前缀**（T42）。
    ///
    /// 与 `UpdateMeshVertices` 的区别：① 顶点数可变；② **索引也可变**（Surface Nets 是共享顶点的索引网格，
    /// 索引数随形状变化）；③ 只上传 `mesh.vertices.size()` / `mesh.indices.size()` 对应的前缀
    /// （倒塌整体的池槽位按**最大容量**建，而一座塔的外表面只有几千个四边形 ⇒ 省下大部分拷贝与带宽）。
    /// `mesh.indices` 为空 ⇒ 该网格本帧**不可见**（`usedIndexCount = 0`，且**不做任何上传**）。
    /// 返回 false 表示句柄无效、槽位已释放，或 `mesh` 超出该网格上传时的容量（调用方按"截断 / 跳过"处理）。
    [[nodiscard]] bool UpdateMeshGeometry(MeshHandle handle, const MeshData& mesh, const glm::dvec3& origin);

    /// 设置本帧的**渲染原点**（世界整数坐标，红线 6）：逐网格变换按 `网格原点 − 渲染原点` 计算。
    ///
    /// T41 起这是"渲染原点重定基"的**唯一**代价 —— 一次常量写入，**不重传任何顶点**
    /// （旧做法在重定基那一帧对 137 个网格逐个 `ReleaseMesh` + `UploadMesh`，debug 下当帧停顿 ≈70~140 ms）。
    /// 顶点只承载网格局部坐标，故改变渲染原点只改变这一个平移量。下一次 `RenderFrame` 生效。
    void SetRenderOrigin(const glm::dvec3& origin) noexcept { m_renderOrigin = origin; }

    /// 设置一个网格的**世界位姿**（原点 + 旋转），不触碰顶点缓冲。
    ///
    /// 用途（T33）：倒塌整体的刚体每帧都在动（位置**与姿态**），但其**局部顶点完全不变** ⇒
    /// 只需改这一块 uniform，CPU 不必重烘焙上万顶点（对照 `UpdateMeshVertices` 的烘焙路径）。
    /// 无效句柄为无操作。下一次 `RenderFrame` 生效。
    void SetMeshTransform(MeshHandle handle, const glm::dvec3& origin, const glm::quat& rotation) noexcept;

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

    /// 设置**自发光网格**的颜色（线性光；ADR 0010 的 HDR 通路会把它推过 1.0，从而在色调映射后发白发光）。
    /// 只影响以 `emissive = true` 上传的网格；下一次 `RenderFrame` 生效。
    void SetEmissiveColor(float red, float green, float blue) noexcept {
        m_emissiveColor[0] = red;
        m_emissiveColor[1] = green;
        m_emissiveColor[2] = blue;
    }

    /// 烘焙并上传**环境贴图**（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）：
    /// 天空 HDRI + 漫反射 irradiance（32×16）+ 预过滤高光（6 级 mip，128×64 起）+ BRDF LUT（256²）。
    ///
    /// 时机：**加载期一次性**（阻塞到 GPU 完成，与 `CreateTextureArray` 同模式）——三条烘焙 pass 各提交一次、
    /// 结束时等一次栅栏；SKILL「不冻结画面」要求的是"画面不停"，加载画面在此期间照常出帧。
    /// 返回 false 表示失败（HDRI 为空 / 创造纹理或管线失败）⇒ 调用方 **WARN 并回落半球天空光**；
    /// 失败时本次的产物会被释放干净（可从零重试，不泄漏）。
    /// 成功后再调用会**先释放旧产物**再重建（重载不泄漏）。
    /// 前置条件：`source.pixels` 非空且 `width / height ≥ 1`（不满足直接返回 false，不抛）。
    [[nodiscard]] bool BakeEnvironment(const EnvironmentSource& source);

    /// 只读：环境贴图是否可用（`BakeEnvironment` 成功）。
    /// false ⇒ 主通道不绘制天空，且环境项走半球天空光回落（见 `BuildLightingUniform` 的 IBL 启用位）。
    [[nodiscard]] bool EnvironmentReady() const noexcept { return m_environmentReady; }

    /// 只读：环境贴图的显存字节总量（4 张纹理之和；已计入 `Stats().textureBytes`）。
    [[nodiscard]] std::uint64_t EnvironmentTextureBytes() const noexcept { return m_environmentBytes; }

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
        std::uint32_t  vertexCount  = 0;  ///< 顶点缓冲**容量**（上传时的顶点数；就地刷新不得改变它）
        std::uint32_t  indexCount   = 0;  ///< 索引缓冲**容量**（`UpdateMeshGeometry` 只能写这个前缀之内）
        /// 本帧**实际绘制**的索引数（T42）：`UploadMesh` = 上传的索引数；变长网格由 `UpdateMeshGeometry`
        /// 改写；`0` = 本帧不可见（跳过绘制）—— 因此"清空一个倒塌槽位"是零上传、零绘制的。
        std::uint32_t  usedIndexCount = 0;
        bool           emissive     = false;  ///< 主通道是否叠加自发光项（见 `UploadMesh`）
        /// 主通道是否使用**带光栅化深度偏移的管线变体**（T78 / 2026-09-29；见 `UploadMesh` 的 `depthBiased`）。
        /// 来源 = 上传时的调用方标记（当前仅**地表 tile** 置 true）；用途 = 消除"层间接管边界环上
        /// 地表网格与可挖体积网格共面重叠"引起的 z-fighting 闪烁。**阴影通道不理会本标记**。
        bool           depthBiased  = false;
        /// 该网格的**世界原点**（T41）：绘制时推送 `平移(原点 − 渲染原点) × 旋转`（见 `MeshTransformUniform`）。
        /// 顶点只承载网格局部坐标，故重定基不必重传顶点（见 `SetRenderOrigin`）。
        /// **`double`**：世界定位不用 `float`（红线 6）——偏移在 `double` 下相减后才落回 `float`。
        double origin[3] = { 0.0, 0.0, 0.0 };
        /// 该网格的**旋转**（T33：倒塌中的刚体；其余网格恒为单位四元数）。
        glm::quat rotation { 1.0F, 0.0F, 0.0F, 0.0F };
    };

    /// 保证主通道图形管线与请求的 MSAA 档位一致（档位变化时用常驻 Shader 重建）。
    /// T67 起**同时**重建天空管线：它在主通道的同一个渲染通道里绘制，采样数必须与目标一致。
    void EnsureMainPipeline(std::uint32_t sampleCount);

    /// 保证一块**常驻暂存缓冲**的容量 ≥ `bytes`（容量够则**不重新分配** ⇒ 稳态零堆分配 / 零 GPU 资源创建）。
    /// 失败返回 false（并把 `buffer` 置空、`capacity` 归零）。T42 起顶点与索引各一条。
    [[nodiscard]] bool EnsureStagingBuffer(SDL_GPUTransferBuffer*& buffer, std::uint32_t& capacity,
                                           std::uint32_t bytes);

    /// 按给定档位创建主通道图形管线（用常驻的 `m_meshVertexShader` / `m_meshFragmentShader`），
    /// 写入 `m_pipeline` 与 `m_pipelineSampleCount`；失败抛 `std::runtime_error`。
    void CreateMainPipeline(std::uint32_t sampleCount);

    /// 创建一条**全屏三角**图形管线（顶点阶段复用 `m_fullscreenVertexShader`，无顶点输入、不剔除）。
    ///
    /// `colorFormat` 即其唯一颜色目标的格式；`withDepthStencil = true` 时声明 D32_FLOAT 深度目标但
    /// **不启用深度测试 / 写入**（用于"在主通道的同一个渲染通道里画天空"），`false` 用于离屏烘焙 pass。
    /// 失败返回 nullptr（调用方决定抛还是回落：主通道抛、烘焙回落）。
    [[nodiscard]] SDL_GPUGraphicsPipeline* CreateFullscreenPipeline(SDL_GPUShader* fragmentShader,
                                                                   SDL_GPUTextureFormat colorFormat,
                                                                   bool withDepthStencil, std::uint32_t sampleCount,
                                                                   const char* label);

    /// 开一个**单颜色目标、无深度**的渲染通道、画一个全屏三角形、关通道（T67：三条烘焙 pass 与色调映射共用）。
    ///
    /// `samplers` / `fragmentUniform` 可为空（分别为 0 个采样器 / 不推送 uniform）。
    void DrawFullscreenPass(SDL_GPUCommandBuffer* commandBuffer, const SDL_GPUColorTargetInfo& colorTarget,
                            SDL_GPUGraphicsPipeline* pipeline, const SDL_GPUTextureSamplerBinding* samplers,
                            std::uint32_t samplerCount, const void* fragmentUniform,
                            std::uint32_t fragmentUniformBytes);

    /// 释放全部环境贴图资源（天空 / irradiance / 预过滤 / LUT）并同步显存记账与就绪标志。
    /// 无资源时为无操作（可重复调用；`BakeEnvironment` 重建前与析构都走这里）。
    void ReleaseEnvironmentTextures() noexcept;

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
    ///
    /// `pushEmissive` 为 true 时（仅主通道）把该网格的自发光参数推到片元 uniform 槽 3：
    /// 自发光网格用 `m_emissiveColor`，其余网格用零（`SDL_gpu.h` §SDL_PushGPUFragmentUniformData：
    /// "Subsequent draw calls in this command buffer will use this uniform data" ⇒ 逐网格推送即可）。
    ///
    /// `depthBiasedPipeline` 为**主通道**的"带深度偏移变体"管线（T78）；为 `nullptr` 时（阴影通道）
    /// 本方法不切换管线，保持调用方绑定的那条。非空时：逐网格按其 `depthBiased` 标记选择管线，
    /// **仅在该标记与当前绑定不一致时**才 `SDL_BindGPUGraphicsPipeline` ⇒ 绑定次数 = 标记切换次数
    /// （调用方保证"同一标记的网格连续出现"：地表 tile 全在绘制列表最前）⇒ **最多 2 次**，
    /// **绝不退化为逐网格绑定**，且**保持既有确定序**（不重排网格）。
    ///
    /// T39：`emissiveState` 是**推送去重**状态 —— 推送值与该状态相同则跳过推送。因为 uniform 数据对
    /// **后续**绘制持续生效，跳过"值没变"的推送不改变任何绘制结果，却把推送次数从"每网格一次"降到
    /// "值变化次数"（本场景实测约 2 次/帧）。调用方须在每个渲染通道开始时传入一个**全新状态**。
    struct EmissivePushState {
        bool  pushed   = false;
        float color[3] = { 0.0F, 0.0F, 0.0F };
        float strength = 0.0F;
    };

    /// T41/T33：逐网格顶点变换的**推送去重**状态（与 `EmissivePushState` 同构、同源理由）。
    /// 变换由 `SDL_PushGPUVertexUniformData(cmd, 0, ...)` 推送（64 字节 / 次，**不是**上传），
    /// 对后续绘制持续生效 ⇒ 值没变就不推。主通道与阴影通道**各持一份**（两个通道的着色器各读自己的）。
    struct MeshTransformPushState {
        bool  pushed = false;
        float matrix[16] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                             0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    };

    void DrawMeshes(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const MeshHandle* meshes,
                    std::size_t meshCount, bool pushEmissive, SDL_GPUGraphicsPipeline* depthBiasedPipeline,
                    EmissivePushState& emissiveState, MeshTransformPushState& transformState);

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

    /// 主通道的**带光栅化深度偏移变体**（T78 / 2026-09-29）：与 `m_pipeline` 除 `rasterizer_state` 的
    /// 深度偏移外**完全相同**（同一批 Shader、同一顶点布局、同一目标格式、同一采样数）。
    /// 由**地表 tile** 之类的共面重叠网格使用（见 `UploadMesh` 的 `depthBiased`）；**随 MSAA 档位与
    /// `m_pipeline` 在 `CreateMainPipeline` 里同生共死**。**`RenderFrame` 热路径绝不创建它**。
    SDL_GPUGraphicsPipeline* m_pipelineDepthBiased = nullptr;

    /// 色调映射管线：全屏三角形，HDR 颜色目标 → 交换链（无深度、不剔除）。
    SDL_GPUGraphicsPipeline* m_tonemapPipeline = nullptr;

    /// 阴影深度管线：**仅顶点着色器**、无颜色目标、深度目标 = `D32_FLOAT` 深度数组的一层。
    SDL_GPUGraphicsPipeline* m_shadowPipeline = nullptr;

    // ---- 环境贴图 / IBL（T67 / ADR 0021）----

    /// 全屏三角的**顶点阶段**（`tonemap.vert`）：色调映射、天空与三条烘焙管线**共用同一个对象**。
    /// 常驻到析构（与 `m_meshVertexShader` 同理由：天空管线要随 MSAA 档位重建，重建时复用同一批 Shader）。
    SDL_GPUShader* m_fullscreenVertexShader = nullptr;

    /// 天空的片元阶段（`sky.frag`）：**常驻**（天空管线随 MSAA 档位重建）。
    SDL_GPUShader* m_skyFragmentShader = nullptr;

    /// 天空管线：全屏三角，写 HDR 颜色目标；声明深度目标但**关闭深度测试与写入** ⇒ 网格照常覆盖天空。
    /// 采样数必须与主通道目标一致 ⇒ 由 `EnsureMainPipeline` 随 MSAA 档位一起重建。
    SDL_GPUGraphicsPipeline* m_skyPipeline = nullptr;

    /// 三条 IBL 烘焙管线（仅加载期在 `BakeEnvironment` 内使用；采样数恒为 1、无深度目标）。
    SDL_GPUGraphicsPipeline* m_irradiancePipeline = nullptr;  ///< HDRI → 32×16 余弦卷积
    SDL_GPUGraphicsPipeline* m_prefilterPipeline  = nullptr;  ///< HDRI → 6 级 mip 的 GGX 预过滤高光
    SDL_GPUGraphicsPipeline* m_brdfLutPipeline    = nullptr;  ///< 无输入 → 256² BRDF LUT

    /// 环境贴图四件套（`R16G16B16A16_FLOAT`）：天空 HDRI、漫反射 irradiance、预过滤高光、BRDF LUT。
    /// **只有同时非空且 `m_environmentReady` 时**才被采样（见 `RenderFrame` 的天空绘制与采样器绑定）。
    SDL_GPUTexture* m_skyTexture        = nullptr;
    SDL_GPUTexture* m_irradianceTexture = nullptr;
    SDL_GPUTexture* m_prefilterTexture  = nullptr;
    SDL_GPUTexture* m_brdfLutTexture    = nullptr;

    /// 环境贴图的采样器：**U 重复 / V,W 钳制** + 线性过滤 + mipmap 线性。
    /// 为什么 U 重复：等距柱状贴图在方位角方向首尾相接（u = 0 与 u = 1 是同一个方向）；
    /// V 必须钳制：极点是奇点，重复会跨到对侧。
    SDL_GPUSampler* m_environmentSampler = nullptr;

    /// 1×1 占位纹理（`R16G16B16A16_FLOAT`，内容 = 白）：环境贴图未就绪时用来**满足采样器绑定**
    /// （着色器已声明 binding 6..8，SDL_gpu 要求声明的采样器都有绑定；此时 `fogParams.z = 0`，
    /// 着色器整段跳过 IBL 采样 ⇒ 内容无意义，只求"有合法绑定"）。
    SDL_GPUTexture* m_environmentPlaceholder = nullptr;

    /// 环境贴图的显存字节（四张之和，含预过滤的 mip 链）；随创建 / 释放增减（`m_stats.textureBytes` 同步）。
    std::uint64_t m_environmentBytes = 0;

    /// 环境贴图是否可用（`BakeEnvironment` 成功）。false ⇒ 不画天空 + 环境项走半球天空光。
    bool m_environmentReady = false;

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

    /// 自发光网格的颜色（线性光；由 `SetEmissiveColor` 写入，逐网格推送到片元槽 3）。
    /// 默认值 = 暖白，供"光球"这类自发光体在未显式设置时也有确定外观。
    float m_emissiveColor[3] = { 1.60F, 1.25F, 0.65F };

    /// 本帧的渲染原点（世界整数坐标，由 `SetRenderOrigin` 写入，T41）。
    /// 逐网格变换 = `平移(MeshResources::origin − m_renderOrigin) × MeshResources::rotation`，绘制时推送。
    /// 默认 `(0,0,0)`：在调用方设置它之前不会绘制任何网格（加载期只画 2D 叠加层），故无需哨兵。
    glm::dvec3 m_renderOrigin { 0.0, 0.0, 0.0 };

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

    /// `UpdateMeshGeometry`（T42）复用的**索引**常驻暂存缓冲（与顶点那条同构、同理由）。
    SDL_GPUTransferBuffer* m_indexStagingBuffer   = nullptr;
    std::uint32_t          m_indexStagingCapacity = 0;
};

}  // namespace vx
