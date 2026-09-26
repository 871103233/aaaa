#pragma once

#include <cstdint>

namespace vx {

// ---------------------------------------------------------------
// 地表高度厂尺寸与精度（冻结口径见 docs/adr/0008-sizes-precision-budget.md）
// ---------------------------------------------------------------

/// 地表 tile 每边的列数。tile 是加载 / 重网格 / 脏标记的最小单位（ADR 0008：64×64 列）。
inline constexpr int kTerrainTileSize = 64;

/// 网格顶点每边数量：`64` 列需要 `65×65` 个顶点。
///
/// 多出的一行 / 一列是**共享边界采样**：相邻 tile 在同一世界列上取到**完全相同**的高度，
/// 因此拼接处的顶点位置逐位相等，不会出现裂缝（red line 12 / references/chunk-and-streaming.md §3）。
inline constexpr int kTerrainTileVertexCount = kTerrainTileSize + 1;

/// 高度定点精度：`int16`，1/16 格（ADR 0008）。
/// 定点而非 `float`，是为了让高度存储与比较跨调用完全确定。
inline constexpr int kHeightUnitsPerBlock = 16;

/// 世界垂直范围 `0 ~ 512` 格（ADR 0008），换算为定点单位。
inline constexpr int kMinTerrainHeightUnits = 0;
inline constexpr int kMaxTerrainHeightBlocks = 512;
inline constexpr int kMaxTerrainHeightUnits = kMaxTerrainHeightBlocks * kHeightUnitsPerBlock;

/// 一列的存储高度：1/16 格定点。**不是** `float`（ADR 0008）。
using Height = std::int16_t;

/// 把定点高度换算为「格」。仅用于网格顶点与材质混合；世界定位仍走整数 / `double`（red line 6）。
[[nodiscard]] constexpr float HeightToBlocks(Height height) noexcept {
    return static_cast<float>(height) / static_cast<float>(kHeightUnitsPerBlock);
}

/// 地表 tile 的整数坐标（单位：tile），可作有序容器的键。
struct TileCoord {
    int x = 0;
    int z = 0;

    [[nodiscard]] friend constexpr bool operator==(const TileCoord& lhs, const TileCoord& rhs) noexcept {
        return lhs.x == rhs.x && lhs.z == rhs.z;
    }

    [[nodiscard]] friend constexpr bool operator<(const TileCoord& lhs, const TileCoord& rhs) noexcept {
        return (lhs.x != rhs.x) ? (lhs.x < rhs.x) : (lhs.z < rhs.z);
    }
};

/// tile 原点（本地顶点 0 号）对应的世界列坐标（单位：列）。
[[nodiscard]] constexpr int TileOriginColumn(int tileIndex) noexcept {
    return tileIndex * kTerrainTileSize;
}

}  // namespace vx
