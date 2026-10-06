#pragma once

#include "generation/terrain_noise.hpp"
#include "shell/surface_shell.hpp"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace vx {

/// **Ring 0 壳窗口**（单位：tile）：壳区域随玩家所在 tile 推进（W7-S3b① / ADR 0024 决策二）。
///
/// 语义与地形 tile 的 `TerrainTileWindow` 同构：中心 = 玩家所在 tile（带滞回的推进由调用方负责，
/// 复用 `TerrainHysteresisCenterTile`），半径 = Ring 0 的 tile 半径（与 `TerrainLodRings::radii[0]` 同源）。
struct SurfaceShellWindow {
    int centerTileX = 0;
    int centerTileZ = 0;
    int radiusTiles = 0;
};

/// 由壳窗口与**世界列范围**算出壳区域（纯函数，红线 7）。
///
/// 口径：
///   - 区域列范围 = 覆盖 tile `[center − r, center + r]` 的列区间 ⇒ 边界是 `kTerrainTileSize` 的倍数
///     （`kTerrainTileSize = 2 × kVolumeBlockSize` ⇒ **同时是 block 对齐**）；这就是"被跳过的四边形整块落在
///     若干 tile 内"的前提（壳接管沿用地表 tile 的既有路径）。
///   - **钳制到 `worldBounds`**（世界列范围，右开）；钳制后为空 ⇒ 返回空区域（`min == max` ⇒ `ContainsColumn` 恒假）。
///   - `radiusTiles < 0` 按 0 处理。
///
/// 前置条件：`worldBounds` 的边界应是 `kTerrainTileSize` 的倍数（调用方用世界 tile 范围推导）。
[[nodiscard]] SurfaceShellRegion MakeSurfaceShellRegion(const SurfaceShellWindow& window,
                                                        const SurfaceShellRegion& worldBounds) noexcept;

/// **单圈重建厚度**（单位：块）= `ceil(edgeFadeBlocks / kVolumeBlockSize)`（纯函数）。
///
/// 含义：区域边界移动时，只有**距边界 < `edgeFadeBlocks` 列**的块其淡出系数才变化 ⇒ 需要重建。
/// `edgeFadeBlocks <= 0` ⇒ 返回 0（无淡出 ⇒ 区域移动不改变任何块的密度）。
[[nodiscard]] int SurfaceShellRebuildRingThickness(const SurfaceShellParams& params) noexcept;

/// 一次"壳块常驻集合调整"的目标清单（纯数据；三个清单**只由入参唯一决定**、均按坐标**升序** ⇒ 红线 7）。
struct SurfaceShellResidencyPlan {
    std::vector<BlockCoord> toLoad;     ///< 目标内、当前**不常驻** ⇒ 建
    std::vector<BlockCoord> toUnload;   ///< 常驻但**不在目标内** ⇒ 卸
    std::vector<BlockCoord> toRebuild;  ///< **仍常驻且在目标内**、但因区域边界移动致淡出变化 ⇒ 重建；`regionChanged == false` 时为空
    std::size_t             desiredCount = 0;  ///< 目标块数（O(窗口)，与世界总量无关）
};

/// **列块 → Y 块范围**的缓存（`ComputeShellBlockSpanY` 每列块要采 `34×34` 列高度 ⇒ 若每次规划都对整窗重算，
/// 单次成本会明显超过一帧预算 —— 区域只按 tile 步进，**只有新进入的边界列块**需要新算）。
///
/// 语义：`SpanOf(bx, bz)` 首次计算并缓存，之后直接返回；同一 `(noise, params)` 下结果与
/// `ComputeShellBlockSpanY` **逐位相同**（纯函数语义不变）。键空间上界 = 世界列块数
/// （10km ⇒ 313×313 ≈ 9.8 万项，每项 8 字节 ⇒ 内存有界）。
///
/// 生命周期：`noise` / `params` 的生命周期必须覆盖本对象。
class SurfaceShellSpanCache {
public:
    SurfaceShellSpanCache(const TerrainNoiseGenerator& noise, const SurfaceShellParams& params) noexcept
        : m_noise(&noise), m_params(&params) {}

    [[nodiscard]] ShellBlockSpan SpanOf(int blockX, int blockZ);

    [[nodiscard]] std::size_t CachedCount() const noexcept { return m_spans.size(); }

private:
    const TerrainNoiseGenerator*                 m_noise = nullptr;
    const SurfaceShellParams*                    m_params = nullptr;
    std::map<std::pair<int, int>, ShellBlockSpan> m_spans;
};

/// 计算壳块常驻集合的目标变化（**带列块 Y 范围缓存**；成本 **O(窗口) + 新列块**，红线 7）。
///
/// 与下面 `noise` 版**逐位等价**；区别只在于 Y 范围**跨调用复用**（主循环用这个 ⇒ 区域步进时只算新边界列块）。
[[nodiscard]] SurfaceShellResidencyPlan PlanSurfaceShellResidency(
    SurfaceShellSpanCache& spans, const SurfaceShellParams& params, const SurfaceShellRegion& region,
    bool regionChanged, const std::vector<BlockCoord>& resident);

/// 计算壳块常驻集合的目标变化（纯函数，成本 **O(窗口)**；红线 7）。
///
/// 目标集合 = 区域内每个列块（`ShellBlockTouchesRegion`）× 其 Y 块范围（`ComputeShellBlockSpanY`）。
///
/// 语义：
///   - `toLoad = 目标 \ resident`、`toUnload = resident \ 目标`（集合差，升序）；
///   - `regionChanged == true` 时，目标内**最外 `SurfaceShellRebuildRingThickness` 圈**列块（∩ 常驻）进入
///     `toRebuild` —— 这是"边界淡出随区域移动"的显式代价（见 `docs/plans/v0.4.md` §1.13）；
///     `regionChanged == false` ⇒ `toRebuild` 为空。
///
/// 前置条件：`noise` 的生命周期覆盖本调用；`resident` 可含任意序（内部排序）。
[[nodiscard]] SurfaceShellResidencyPlan PlanSurfaceShellResidency(
    const TerrainNoiseGenerator& noise, const SurfaceShellParams& params, const SurfaceShellRegion& region,
    bool regionChanged, const std::vector<BlockCoord>& resident);

}  // namespace vx
