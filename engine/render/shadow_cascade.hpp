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

/// 把原始 texel 世界尺寸**向上量化到 2 的幂**（缺陷 B8 修复，治机制 2："级联半径随视角连续变化"）。
///
/// 推导：每级 texel 世界尺寸 = `2·halfExtent / resolution`，而 `halfExtent` 取自"视锥切片 AABB 的外接球"
/// 半径 —— 相机一转 AABB 就变 ⇒ texel 连续变化 ⇒ texel 对齐网格在变，影子随视角"爬行"。
/// 把 texel 向上量化到 2 的幂 `q` 后，由 `q` 反推**实际使用的半径** `radiusUsed = q·resolution/2`
/// （`halfExtent` 相应取 `max(halfExtentRaw, radiusUsed)` 以保证覆盖不缩水）。
/// 因 `resolution` 也是 2 的幂，可证 `q·resolution/2 = nextPowerOfTwo(halfExtentRaw)`（量纲推导：
/// `texel = halfExtent / (resolution/2)`，两边同乘 `resolution/2` 即 `nextPowerOfTwo(x/c)·c =
/// nextPowerOfTwo(x)`，`c = resolution/2` 为 2 的幂）—— 故 `radiusUsed` 就是 halfExtent 的 2 的幂上界，
/// **分段恒定**：相机小幅旋转（半径不跨 2 的幂边界）时 texel 与投影缩放均不变，网格不再爬行。
///
/// 保证：结果 `≥ rawTexelWorldSize`（覆盖不缩水）、是 2 的幂、随 `raw` 单调不减；
/// `raw ≤ 0` 或 `resolution ≤ 0` 时原样返回（调用方不应发生，仅作兜底）。不读全局、不分配。
[[nodiscard]] float QuantizeTexelWorldSize(float rawTexelWorldSize, int resolution) noexcept;

/// 级联过渡带的混合权重（缺陷 B8 修复，治机制 1："级联选择依赖相机朝向"）。
///
/// `viewDepth` = 沿相机视轴的线性深度；`splitInner` = 该级远平面（级联边界）；
/// `blendBand` = 过渡带宽度 = `cascade_blend × splitInner`（相对比例，避免固定格数在近处过宽）。
/// 返回**近级**（`splitInner` 所属级联）的权重 ∈ `[0, 1]`，**远级**（下一级）权重 = `1 − 返回值`：
///   - `viewDepth ≤ splitInner − blendBand`（带外近侧）→ `1`（纯近级）；
///   - `viewDepth ≥ splitInner`（带外远侧）→ `0`（纯远级）；
///   - 带内用 `smoothstep` 过渡 ⇒ **两侧权重和恒为 1**，跨级联处不再出现硬跳变 / 重采样突跳。
/// `blendBand ≤ 0`（即 `cascade_blend = 0`）时恒返回 `1` ⇒ 逐字退回"单级采样"（旧行为）。
/// 这是 CPU 侧纯函数（不读全局、不分配），与 `assets/shaders/mesh.frag` 的 `cascadeBlendWeight()`
/// **逐字镜像**；两边改动必须同步。
[[nodiscard]] float CascadeBlendWeight(float viewDepth, float splitInner, float blendBand) noexcept;

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
/// 视图与正交参数：眼睛位于 `snappedCenter + sunDirection · (cascadeRadius + axisExtension)`，看向中心，
/// 正交范围见下、深度 `[0, 2·cascadeRadius + axisExtension]` —— 包住该包围球**并向前覆盖投射体**。
///
/// **投射体扩展（shadow caster extension，缺陷 1 修复）**：
/// 只用"视锥切片外接球"时，正交盒只有 `±cascadeRadius`（X/Y）与 `0~2·cascadeRadius`（沿光轴）——
/// 高于该球的高大投射体（地标塔）在阴影通道被裁剪，只有落在盒内的那段塔身参与投射，
/// 相机转动 ⇒ 视锥切片变 ⇒ 盒变 ⇒ 参与投射的塔身段变，阴影因此**随视角变化**（违反"阴影是几何与光照的
/// 函数"这一契约）。故按下述方式把盒**朝太阳一侧**扩展 `casterHeight`（投射体高出**级联中心**的高度，格）：
///   - 沿光轴延伸 `axisExtension = casterHeight / max(|dir.y|, eps)`；
///   - X/Y 各扩 `horizontalExtension = casterHeight · tan(θ)`，其中
///     `tanθ = sqrt(1 - dir.y²) / max(|dir.y|, eps)`，`θ` = 光照方向与竖直方向的夹角。
///
/// 推导（保守上界，保证覆盖；两种等价写法取"扩盒"一种）：
///   取光空间正交基 `(right, up, dir)`。投射体竖直向上高出中心 `h`，其顶端相对中心的光空间坐标为
///   `(h·(right·Y), h·(up·Y), h·(dir·Y)) = (·, ·, h·cosθ)`：
///     - 沿光轴：竖直位移在光轴上的投影为 `h·cosθ`（`dir.y = cosθ`）。本实现取更大的保守量
///       `h / max(|dir.y|, eps) ≥ h·cosθ`（`cosθ ≤ 1`；太阳接近竖直时二者相等），
///       即"把竖直高度按光轴斜率折算"，绝不会欠覆盖；
///     - X/Y：`|right·Y| ≤ sinθ`、`|up·Y| ≤ sinθ`，取更大的保守量 `h·tanθ ≥ h·sinθ`（`θ ∈ [0, 90°)`）。
///   扩展后：眼睛沿 `sunDirection` 再外移 `axisExtension`，深度上限 += `axisExtension`；
///   原切片角点仍满足 `|x'|,|y'| ≤ cascadeRadius ≤ halfExtent`、深度落在 `[0, 2r + axisExtension]`，
///   故**旧覆盖不丢**，只是盒变大、分辨率摊薄一点。
///
/// `casterHeight = 0`（默认）⇒ `axisExtension = horizontalExtension = 0`，逐字退回旧行为
/// （既有"包围球 8 点在内""亚 texel 平移矩阵不变"等断言不受影响）。
///
/// 前置条件：`sunDirection` 长度非零；`cascadeRadius > 0`；`cascadeTexelSize > 0`；`casterHeight ≥ 0`。
/// 不读全局、不分配。
[[nodiscard]] glm::mat4 BuildCascadeLightMatrix(const glm::vec3& sunDirection, const glm::vec3& cascadeCenter,
                                                float cascadeRadius, float cascadeTexelSize,
                                                float casterHeight = 0.0F) noexcept;

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
///   - `cascadeBlend`            级联过渡带宽度比例（缺陷 B8）；过渡带宽 = 本值 × 该级远平面。
///                               最后一个 `vec4` 只有 `x` 有效，`y/z/w` 为填充（保持 std140 对齐）。
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

    float cascadeBlend       = 0.0F;
    float cascadeBlendUnused0 = 0.0F;
    float cascadeBlendUnused1 = 0.0F;
    float cascadeBlendUnused2 = 0.0F;
};

static_assert(sizeof(ShadowUniform) == 16 * 20,
              "ShadowUniform 必须与 mesh.frag 的 std140 布局逐字节一致（4×mat4 + 4 个 vec4 = 320 字节）");

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
///
/// **`casterTopRelative`（缺陷 1 的新参数，坐标系一致是关键）**：
/// 传入的是**地形中最高投射体相对渲染原点的高度**（格，≥ 0），即
/// `最高地形高度（世界 Y）− 渲染原点 Y`；`game/` 从已加载地形推导（`engine/` 不读配置、不认识世界类型）。
/// 本函数对**每一级**算出该级真正需要覆盖的高度：
///   `casterHeight_i = max(casterHeightMin, casterTopRelative − center_i.y)`
/// 其中 `center_i` 是级联中心，**已是渲染原点相对的坐标**（由 `viewRelative` 的逆矩阵得到）。
/// 因 `casterTopRelative − center_i.y = 最高地形高度 − center_i 的世界 Y`（渲染原点 Y 在相减中抵消），
/// 该值与"最高地形高度 − 当前切片中心高度"逐字等价。**若把 `casterTopRelative` 误当成世界坐标，
/// 就会与相对的 `center_i.y` 混用、整段偏移一个渲染原点 Y —— 这是本项最易写错之处。**
/// `casterHeightMin` 取自 `table.Shadow().casterHeightMin`，是配置里的**下限兜底**：
/// 即使地形推导为 0（或地形为空），也保证每级覆盖到中心之上该高度，避免漏投影。
///
/// **缺陷 B8 的两条修复（阴影不再随视角变化）**：
///   1. texel 量化：每级 halfExtent 经 `QuantizeTexelWorldSize` 量化后**分段恒定**（见其说明），
///      相机小幅旋转时投影缩放与 texel 网格不变（治"级联半径连续变化 ⇒ 网格爬行"）；
///   2. 级联过渡带：`cascadeBlend` 随 `cascade_blend` 配置透传，着色器据 `CascadeBlendWeight`
///      在边界附近混合相邻两级（治"级联选择依赖相机朝向 ⇒ 跨级联重采样"）。
[[nodiscard]] ShadowUniform BuildShadowUniform(const LightingTable& table, const glm::mat4& viewRelative,
                                               float fieldOfViewDegrees, float aspectRatio, float nearPlane,
                                               float farPlane, float casterTopRelative) noexcept;

}  // namespace vx
