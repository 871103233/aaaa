#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>

namespace vx {

class LightingTable;

/// 级联阴影（CSM）的级数上限。与 `assets/shaders/mesh.frag` 的 `kMaxShadowCascades` 必须一致。
inline constexpr int kMaxShadowCascades = 4;

/// 级联分割（practical split scheme）：返回**各级的远平面视距**（相机视距，单位 = 格）。
///
/// 公式（`i = 1..cascadeCount`，`n = cascadeCount`，`t = i / n`）：
///   `split_i = λ · near·(far/near)^t + (1-λ)·(near + (far-near)·t)`
/// 取 `out[i-1] = split_i`，故 `out[cascadeCount-1] == farPlane`；`[cascadeCount, kMaxShadowCascades)`
/// 的槽位填 `farPlane`（不参与渲染，只为让调用方拿到确定值、不做越界读取）。
///
/// 退化情形：`λ = 0` → 均匀分割（`split_i = near + (far-near)·t`）；`λ = 1` → 完全对数分割
/// （`split_i = near·(far/near)^t`）。两者都满足"严格递增、首项 > near、末项 = far"。
///
/// 前置条件：`nearPlane > 0`、`farPlane > nearPlane`、`lambda ∈ [0, 1]`；
/// `cascadeCount` 会被钳制到 `[1, kMaxShadowCascades]`。不读全局、不分配。
[[nodiscard]] std::array<float, kMaxShadowCascades> ComputeCascadeSplits(float nearPlane, float farPlane,
                                                                          int cascadeCount, float lambda) noexcept;

/// 单个级联的正交光空间矩阵：**正交投影 × 视图**，把该级联包围球映射进 NDC `[-1, 1]³`。
///
/// 关键约定（见 `.trae/skills/.../references/meshing-and-render.md` §4 与 SDL_gpu.h §Coordinate System）：
///   - SDL_gpu 的 NDC：左下角 `(-1,-1)`、**+Y 向上**、Z ∈ `[0, 1]`（近平面 = 0）。因此这里用
///     `glm::orthoRH_ZO`（右手系 + 0~1 深度），与主相机的 `glm::perspectiveRH_ZO` 同一深度约定。
///   - **`cascadeCenter` 必须与待投影顶点处于同一坐标系**。本项目地形 / 体积 / 角色顶点都是
///     **相机相对坐标**（红线 6：上传 GPU 前已减去渲染原点），故调用方传入的必须是
///     **渲染原点相对**的级联中心；矩阵因此直接作用于相机相对顶点，**不需要任何额外翻转 / 平移补偿**。
///     等价推导：世界空间光视图 `V_w` 作用在相对坐标上的效果是 `V_w · p_rel = V_w · (p_world − O)`；
///     而 `lookAt(c + dir·r, c, up)` 对平移是可交换的（`lookAt(c−O, ...) = lookAt(c, ...) · T(−O)`），
///     所以直接用 `c_rel = c_world − O` 构造视图即可得到 `V_w · T(−O)`，与"世界空间矩阵右乘平移"等价。
///   - **texel 对齐**：把中心在光空间右 / 上轴上的投影量化到 `cascadeTexelSize` 的整数倍，再据量化后的
///     中心重建视图矩阵。相机移动不足一个 texel 时矩阵**不变**，因此阴影不会随相机亚 texel 抖动。
///
/// 视图与正交参数：眼睛位于 `snappedCenter + sunDirection · cascadeRadius`，看向中心，正交范围
/// `±cascadeRadius`、深度 `[0, 2·cascadeRadius]` —— 正好包住该包围球。
///
/// 前置条件：`sunDirection` 长度非零；`cascadeRadius > 0`；`cascadeTexelSize > 0`。不读全局、不分配。
[[nodiscard]] glm::mat4 BuildCascadeLightMatrix(const glm::vec3& sunDirection, const glm::vec3& cascadeCenter,
                                                float cascadeRadius, float cascadeTexelSize) noexcept;

/// 片元槽 2（`set = 3, binding = 2`）的阴影 uniform 块，std140 布局。
///
/// 字段排布与 `assets/shaders/mesh.frag` 的 `ShadowBlock` **逐字对应**：
///   - `lightMatrices[4]`        每级光空间矩阵（**渲染原点相对坐标系**，见 `BuildCascadeLightMatrix`）
///   - `splitDistances`          `x..w` = 各级远平面（相机视距，格）
///   - `cascadeCount`            实际级数（着色器按它截断数组遍历）
///   - `texelSize`               `1 / resolution`（UV 单位，PCF 步长）
///   - `depthBias`               深度偏移（阴影图深度单位）
///   - `normalOffset`            法线偏移（世界单位 / 格）
///   - `cameraForwardX/Y/Z`      相机世界前向（单位向量，级联选择按"沿视轴的线性深度"）
///   - `enabled`                 1 = 启用、0 = 关闭（着色器据此**整段跳过**采样）
struct ShadowUniform {
    glm::mat4 lightMatrices[kMaxShadowCascades];

    float splitDistances[kMaxShadowCascades];

    float cascadeCount = 0.0F;
    float texelSize    = 0.0F;
    float depthBias    = 0.0F;
    float normalOffset = 0.0F;

    float cameraForwardX = 0.0F;
    float cameraForwardY = 0.0F;
    float cameraForwardZ = -1.0F;
    float enabled        = 0.0F;
};

static_assert(sizeof(ShadowUniform) == 16 * 19,
              "ShadowUniform 必须与 mesh.frag 的 std140 布局逐字节一致（4×mat4 + 4 个 vec4 = 304 字节）");

/// 由光照表与**渲染原点相对**的相机参数构建本帧的阴影 uniform（CPU→GPU 唯一入口）。
///
/// 这是"配置 → GPU"的唯一投影入口：级联数与分割系数取自 `table.Shadow()`，太阳方向取自
/// `table.Sun()`（此处归一化）；**禁止**在着色器里硬编码第二份级数 / 分割 / 偏移。
///
/// 为什么在这里而不是渲染器里：`MeshRenderer` 不认识相机设置（near/far/fov/aspect），
/// 这些值由 `game/` 传入；本函数是**纯函数**（不读全局、不分配），便于单测锁定几何正确性。
///
/// 输入坐标系：`viewRelative` 是**渲染原点相对**的相机视图矩阵（与上传顶点的坐标系一致），
/// 其逆矩阵把视空间视锥角点还原到相对坐标；由此得到的级联中心也是相对坐标，
/// 从而 `BuildCascadeLightMatrix` 产出的矩阵可直接作用于相机相对顶点。
/// 相机世界前向由视图矩阵导出：`view` 的第三列为 `-forward`。
///
/// `enabled = false` 时返回一个显式关闭的块（`enabled = 0`、`cascadeCount = 0`），
/// 着色器据 `enabled` 整段跳过；此时不保证矩阵有意义。
[[nodiscard]] ShadowUniform BuildShadowUniform(const LightingTable& table, const glm::mat4& viewRelative,
                                               float fieldOfViewDegrees, float aspectRatio, float nearPlane,
                                               float farPlane) noexcept;

}  // namespace vx
