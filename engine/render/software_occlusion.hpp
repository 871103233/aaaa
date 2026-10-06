#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace vx {

/// "该格没有遮挡面"的哨兵值（ADR 0031）。**必须小于任何合法 NDC 深度**（NDC z ∈ [0,1]）。
inline constexpr float kNoOccluderDepth = -1.0F;

/// CPU 软件遮挡用的**低分辨率 NDC 深度图**（[ADR 0031](../../docs/adr/0031-occlusion-culling-software.md)）。
///
/// 每格 `depth[cell]` = 落进该格的**遮挡面 NDC 深度**的**最远值**（`max` 聚合，越大越远）。
/// 为什么取**最远**而不是最近：剔除判定是"候选比该格遮挡面更远即被挡"；取最远 ⇒ 判定偏保守
/// （更少剔除）⇒ **绝不误剔可见物**（ADR 0031 决策二）。
///
/// 纯 CPU 数据（不进 VRAM）；`depth` 行主序，长度 = `width × height`。
struct OcclusionDepthGrid {
    int                width  = 0;
    int                height = 0;
    std::vector<float> depth;  ///< 全格初值 = `kNoOccluderDepth`
};

/// 重置尺寸并清空（全格 = `kNoOccluderDepth`）。尺寸未变且容量足够时**不重新分配**。
///
/// 前置条件：`width > 0 && height > 0`（否则只清空长度）。
void ResetOcclusionDepthGrid(OcclusionDepthGrid& grid, int width, int height);

/// 把一个**渲染相对**的遮挡面点投影进深度图（`max` 聚合）。
///
/// 点经 `viewProjectionRelative` 落在 NDC 盒（x,y ∈ [-1,1]、z ∈ [0,1]）之外 ⇒ 无操作（保守）。
/// 纯函数（除对 `grid` 的写入）：不读全局、不做 GPU / IO。
void SplatOccluderPoint(OcclusionDepthGrid& grid, const glm::mat4& viewProjectionRelative,
                        const glm::vec3& pointRelative) noexcept;

/// 候选 AABB（**渲染相对**）是否**确定被遮挡**（保守：只有全部条件成立才返回 `true`）。
///
/// 条件：① 8 个角点都落在 NDC 盒内；② 屏幕格矩形**不越界**且格数 ≤ `maxCoveredCells`；
/// ③ 矩形内**每一格**都有遮挡面，且候选的**最近** NDC 深度 > 该格深度 + `depthBias`。
/// 任一条件不成立即返回 `false`（宁可多提交，绝不误剔）。
///
/// 前置条件：`minimumRelative` / `maximumRelative` 各分量满足 `minimum <= maximum`。
[[nodiscard]] bool IsAabbOccluded(const OcclusionDepthGrid& grid, const glm::mat4& viewProjectionRelative,
                                  const glm::vec3& minimumRelative, const glm::vec3& maximumRelative,
                                  float depthBias, int maxCoveredCells) noexcept;

}  // namespace vx
