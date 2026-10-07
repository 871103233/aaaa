#pragma once

#include "generation/terrain_params.hpp"  // LandformKind
#include "object/object_scatter.hpp"      // ScatterPoint + splitmix64 哈希（复用，保证同一套确定性口径）
#include "terrain/terrain_types.hpp"      // TileCoord / kTerrainTileSize

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// **地形感知放置规则**（`tech-plan-v2.0.md` §3.3 的四项适配度判据；[ADR 0033](../../docs/adr/0033-world-content-placement-and-streaming.md) 决策一）。
///
/// 四项判据与字段的对应：
///   | §3.3 判据 | 字段 |
///   | --- | --- |
///   | 坡度阈值 | `minSlopeDegrees` / `maxSlopeDegrees` |
///   | 高度带   | `minHeightBlocks` / `maxHeightBlocks` |
///   | 群系（当前占位 = W3 地貌分区） | `allowPlains` / `allowHills` / `allowMountains` |
///   | 互斥半径 | `cellBlocks`（抖动网格步长 ⇒ 最小间距 ≥ `0.5 × cell`） |
///
/// **V0.6 C7 追加**：**气候区间**（`minTemperature` / `maxTemperature` / `minHumidity` / `maxHumidity`）——
/// `tech-plan-v2.0.md` §3.1 的"2D 温度与湿度"，使放置可按气候（湿 / 干、冷 / 热）区分；**缺省全区间 ⇒ 不约束**。
///
/// 参数一律来自配置（`[[scatter_tiled]]`），非法由配置层拦下（ADR 0005）。
struct PlacementRule {
    /// 抖动网格步长（格，必须 > 0）：每格至多一个候选点、点落在格内 `[0.25, 0.75] × cell` 区间
    /// ⇒ **相邻格点间距 ∈ (0.5·cell, 1.5·cell]** ⇒ **最小间距 > 0.5 × cell**（可证明，已单测）。
    float cellBlocks = 16.0F;

    float minSlopeDegrees = 0.0F;    ///< 坡度下界（度，含端点）
    float maxSlopeDegrees = 45.0F;   ///< 坡度上界（度，含端点）
    float minHeightBlocks = 0.0F;    ///< 高度带下界（格，含端点）
    float maxHeightBlocks = 512.0F;  ///< 高度带上界（格，含端点）

    bool allowPlains    = true;  ///< 是否允许落在平原
    bool allowHills     = true;  ///< 是否允许落在丘陵
    bool allowMountains = true;  ///< 是否允许落在山川

    /// **气候（V0.6 C7）**：温度 / 湿度区间（闭区间，值域 `[0, 1]`）。
    /// 缺省 = **全区间** ⇒ 不构成约束（与引入气候判据之前**逐位一致**）。
    float minTemperature = 0.0F;
    float maxTemperature = 1.0F;
    float minHumidity    = 0.0F;
    float maxHumidity    = 1.0F;
};

/// 一次地形采样（由调用方提供 ⇒ 把地形访问隔离在纯函数之外，便于单测；[ADR 0033] 决策一）。
struct PlacementSample {
    float        heightBlocks = 0.0F;  ///< 该列地表高度（格）
    float        slopeDegrees = 0.0F;  ///< 该点坡度（度，`[0, 90]`）
    LandformKind landform     = LandformKind::Plains;
    float        temperature  = 0.5F;  ///< 温度（`[0, 1]`；V0.6 C7）
    float        humidity     = 0.5F;  ///< 湿度（`[0, 1]`；V0.6 C7）
};

/// 纯函数：地貌是否被规则允许。
[[nodiscard]] inline bool LandformAllowed(const PlacementRule& rule, LandformKind landform) noexcept {
    switch (landform) {
        case LandformKind::Plains:    return rule.allowPlains;
        case LandformKind::Hills:     return rule.allowHills;
        case LandformKind::Mountains: return rule.allowMountains;
    }
    return false;  // 枚举之外的取值：一律拒绝（不静默放行）
}

/// 纯函数（红线 7）：采样点是否满足规则的全部判据。
///
/// 语义：坡度 / 高度 / 温度 / 湿度都用**闭区间**判定（含端点）；`NaN` 一律**拒绝**（比较为假 ⇒ 走拒绝分支）。
[[nodiscard]] inline bool IsPlacementAllowed(const PlacementRule& rule, const PlacementSample& sample) noexcept {
    if (!(sample.slopeDegrees >= rule.minSlopeDegrees && sample.slopeDegrees <= rule.maxSlopeDegrees)) {
        return false;
    }
    if (!(sample.heightBlocks >= rule.minHeightBlocks && sample.heightBlocks <= rule.maxHeightBlocks)) {
        return false;
    }
    if (!(sample.temperature >= rule.minTemperature && sample.temperature <= rule.maxTemperature)) {
        return false;
    }
    if (!(sample.humidity >= rule.minHumidity && sample.humidity <= rule.maxHumidity)) {
        return false;
    }
    return LandformAllowed(rule, sample.landform);
}

namespace object_placement_detail {

/// 候选点的**格内位置比例下界**：点落在 `[0.25, 0.75] × cell` ⇒ 相邻格点最小间距 > `0.5 × cell`。
inline constexpr double kCellInset = 0.25;

/// 格子键：`seed` 与 `(tileX, tileZ, cellIx, cellIz)` 混合 ⇒ 每格抖动 / 朝向独立且可复现。
///
/// **tile 局部** ⇒ 一个格点只属于一个 tile（相邻 tile 永不重复计算同一点，见 [ADR 0033] 决策二）。
[[nodiscard]] inline std::uint64_t TileCellKey(std::uint64_t seed, int tileX, int tileZ, int cellIx,
                                               int cellIz) noexcept {
    const std::uint64_t a = object_scatter_detail::Hash(seed ^ 0x243F6A8885A308D3ULL);
    const std::uint64_t b = object_scatter_detail::Hash(a ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(tileX)));
    const std::uint64_t c =
        object_scatter_detail::Hash(b ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tileZ)) << 32U));
    const std::uint64_t d =
        object_scatter_detail::Hash(c ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellIx)) << 16U));
    return object_scatter_detail::Hash(d ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellIz)) << 48U));
}

}  // namespace object_placement_detail

/// **分块确定性候选点**（[ADR 0033](../../docs/adr/0033-world-content-placement-and-streaming.md) 决策二；**纯函数、确定性**）：
/// 在**给定 tile 的列范围**内铺抖动网格，产出该 tile 的全部候选点（未做地形过滤 —— 由 `IsPlacementAllowed` 在调用侧判定）。
///
/// 关键性质（流式安全的前提）：
///   1. **tile 局部**：点全部落在 `[tile·64, tile·64 + 64)` 内 ⇒ **相邻 tile 不重复**；
///   2. **无状态可重建**：只由 `(rule, seed, tile)` 决定 ⇒ 同一 tile 任何时候都得到**逐位相同**的点
///      （红线 7 / 15：只看"当前 tile + 种子"即可重建）；
///   3. **不重叠**：每格至多一个点、格内位置 ∈ `[0.25, 0.75] × cell` ⇒ 最小间距 > `0.5 × cell`。
///
/// 非法 / 退化规则（`cellBlocks <= 0`，或 `cellBlocks` 大于 tile 边长 ⇒ 一格都放不下）⇒ 返回**空表**
///（配置层已拦，不在此抛）。
[[nodiscard]] inline std::vector<ScatterPoint> PlanTileCandidates(const PlacementRule& rule, std::uint64_t seed,
                                                                  const TileCoord& tile) {
    std::vector<ScatterPoint> points;
    if (!(rule.cellBlocks > 0.0F)) {
        return points;
    }

    const double cell = static_cast<double>(rule.cellBlocks);
    const int    cellsPerSide = static_cast<int>(std::floor(static_cast<double>(kTerrainTileSize) / cell));
    if (cellsPerSide <= 0) {
        return points;
    }

    const double originX = static_cast<double>(tile.x) * static_cast<double>(kTerrainTileSize);
    const double originZ = static_cast<double>(tile.z) * static_cast<double>(kTerrainTileSize);

    points.reserve(static_cast<std::size_t>(cellsPerSide) * static_cast<std::size_t>(cellsPerSide));
    for (int cz = 0; cz < cellsPerSide; ++cz) {
        for (int cx = 0; cx < cellsPerSide; ++cx) {
            const std::uint64_t key = object_placement_detail::TileCellKey(seed, tile.x, tile.z, cx, cz);
            const double        u   = object_scatter_detail::UnitFrom(object_scatter_detail::Hash(key));
            const double        v =
                object_scatter_detail::UnitFrom(object_scatter_detail::Hash(key ^ 0xABCDEF0123456789ULL));
            const double w =
                object_scatter_detail::UnitFrom(object_scatter_detail::Hash(key ^ 0x1234567890ABCDEFULL));

            // 格内位置 ∈ [0.25, 0.75] × cell（含下界、不含上界）⇒ 点恒在格内 ⇒ 恒在 tile 内。
            const double fx = object_placement_detail::kCellInset + (1.0 - 2.0 * object_placement_detail::kCellInset) * u;
            const double fz = object_placement_detail::kCellInset + (1.0 - 2.0 * object_placement_detail::kCellInset) * v;

            ScatterPoint point;
            point.x          = static_cast<float>(originX + (static_cast<double>(cx) + fx) * cell);
            point.z          = static_cast<float>(originZ + (static_cast<double>(cz) + fz) * cell);
            point.yawDegrees = static_cast<float>(w * 360.0);
            points.push_back(point);
        }
    }
    return points;
}

}  // namespace vx
