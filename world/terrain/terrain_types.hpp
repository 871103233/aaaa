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

/// 一张**满地表** tile 的索引数上界：每格两个三角形 = `64×64×6 = 24576`。
///
/// **顶点数是常数**（`kTerrainTileVertexCount²`，与四边形过滤无关），而**索引数会随层间接管（ADR 0011）升降**：
/// 被可挖体积接管的四边形不发射（网格变小），接管退去后又长回来。
/// ⇒ 这个上界正是"tile 网格缓冲**永远够用**"的容量（T82：地表 tile 的兜底重建按它预留，
/// 使该 tile 之后无论接管如何翻转都不再触发重建）。
inline constexpr int kTerrainTileIndexCount = kTerrainTileSize * kTerrainTileSize * 6;

// ---------------------------------------------------------------
// 地表 LOD 分环（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)「二、LOD 分环」）
// ---------------------------------------------------------------

/// 地表 LOD 环数：Ring 0 = 步长 1（全细节）/ Ring 1 = 步长 2（高模）/ Ring 2 = 步长 4（低模）。
inline constexpr int kTerrainLodLevelCount = 3;

/// LOD 环 `lod` 的**列步长**：相邻网格顶点在世界列上相隔多少列（`1, 2, 4`）。
[[nodiscard]] constexpr int TerrainLodStep(int lod) noexcept { return 1 << lod; }

/// LOD 环 `lod` 的**父级网格间距**（`2 × 步长`，即 `2, 4, 8`）。
///
/// CDLOD 的顶点过渡把细网格顶点向**父级（更粗一级）网格**的对应列靠拢（ADR 0024「接缝策略：顶点过渡 morph」）；
/// 父级网格间距恰为本级步长的两倍，故 morph 目标列 = `floor(列 / 父级间距) × 父级间距`（见 `terrain_mesher.cpp`）。
[[nodiscard]] constexpr int TerrainLodSnapStep(int lod) noexcept { return 2 * TerrainLodStep(lod); }

/// LOD 环 `lod` 的**顶点每边数量**：`kTerrainTileSize / 步长 + 1`（`65 / 33 / 17`）。
/// 多出的一行 / 一列仍是**共享边界采样**（与 LOD0 同口径），故相邻环在边界列上高度一致。
[[nodiscard]] constexpr int TerrainLodVertexSide(int lod) noexcept {
    return kTerrainTileSize / TerrainLodStep(lod) + 1;
}

/// LOD 环 `lod` 的**顶点总数**（`side²`；`4225 / 1089 / 289`）。
[[nodiscard]] constexpr int TerrainLodVertexCount(int lod) noexcept {
    return TerrainLodVertexSide(lod) * TerrainLodVertexSide(lod);
}

/// LOD 环 `lod` 的**索引数**：每格两个三角形 = `(kTerrainTileSize / 步长)² × 6`（`24576 / 6144 / 1536`）。
[[nodiscard]] constexpr int TerrainLodIndexCount(int lod) noexcept {
    const int quadsPerSide = kTerrainTileSize / TerrainLodStep(lod);
    return quadsPerSide * quadsPerSide * 6;
}

/// 高度定点精度：`int16`，1/16 格（ADR 0008）。
/// 定点而非 `float`，是为了让高度存储与比较跨调用完全确定。
inline constexpr int kHeightUnitsPerBlock = 16;

/// 世界垂直范围 `0 ~ 512` 格（ADR 0008），换算为定点单位。
inline constexpr int kMinTerrainHeightUnits = 0;
inline constexpr int kMaxTerrainHeightBlocks = 512;
inline constexpr int kMaxTerrainHeightUnits = kMaxTerrainHeightBlocks * kHeightUnitsPerBlock;

/// 一列的存储高度：1/16 格定点。**不是** `float`（ADR 0008）。
using Height = std::int16_t;

/// **一列的高度改动样本**（列坐标 + 改前 / 改后高度）。
///
/// 用途（V0.11 / I3）：让"改地形"这一动作**可逆** —— 落点模式 ①压平 / ③填充会改高度场，
/// 撤销该建筑时必须把地形恢复原样，否则会留下"建筑没了、地面还平"的**世界不自洽**
/// （见 `.trae/skills/voxel-engine-dev-standards/SKILL.md`「世界内一致性」）。
struct TerrainColumnEdit {
    int    x      = 0;  ///< 世界列 X
    int    z      = 0;  ///< 世界列 Z
    Height before = 0;  ///< 改动前的定点高度
    Height after  = 0;  ///< 改动后的定点高度
};

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
