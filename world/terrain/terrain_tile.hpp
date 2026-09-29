#pragma once

#include "generation/terrain_noise.hpp"
#include "terrain/terrain_types.hpp"

#include <array>
#include <cstddef>

namespace vx {

/// 一个地表 tile 的高度数据：`65×65` 个 `int16` 定点高度（1/16 格）。
///
/// 为什么是 65 而不是 64：网格顶点比"列数"多一行 / 一列，多出的那层是**共享边界采样** ——
/// 相邻 tile 在同一世界列上取到同一个值（都由纯生成函数决定，或由同一份被修改的数据决定），
/// 因此拼接处顶点位置逐位相等，不产生裂缝。单 tile 约 8.25 KB，与 ADR 0008 的 ≈8 KB 一致。
///
/// 线程约定：生成 / 笔刷阶段由**单个写者**填充；填充完成后可被多个网格化线程只读访问。
struct TerrainTile {
    TileCoord coord {};

    std::array<Height, static_cast<std::size_t>(kTerrainTileVertexCount) *
                           static_cast<std::size_t>(kTerrainTileVertexCount)>
        heights {};

    /// 本 tile 内**最高的地表高度（格）** —— 由 `TerrainWorld` 在生成 / 重网格后写回的**缓存**。
    ///
    /// 为什么缓存：`TerrainWorld::MaxSurfaceHeightBlocks()` 每帧被调用（CSM 的投射体扩展要用），
    /// 若每次都遍历"全部 tile × 全部顶点"，成本会与**世界总量**成正比（16×16 tile 下约 108 万次/帧，
    /// debug 下可达数十毫秒）—— 违反 SKILL 第四节的「每帧工作不得与总量成正比」。
    /// 缓存后只需遍历 tile（与 tile 数成正比）。
    ///
    /// 语义：只在生成 / 重网格时刷新，故**可能偏大**（该 tile 最高的那列被削低后，
    /// 直到重网格前仍报旧值）。方向是**安全**的：它只用来放大阴影投射体盒，偏大不会漏阴影。
    float maxSurfaceBlocks = 0.0F;

    /// 本地顶点 `(i, j)` 对应的世界列坐标（单位：列）。
    [[nodiscard]] int WorldColumnX(int i) const noexcept { return TileOriginColumn(coord.x) + i; }
    [[nodiscard]] int WorldColumnZ(int j) const noexcept { return TileOriginColumn(coord.z) + j; }

    /// 本地顶点高度（定点）。前置条件：`i`、`j` ∈ `[0, kTerrainTileSize]`。
    [[nodiscard]] Height At(int i, int j) const noexcept { return heights[Index(i, j)]; }
    void SetAt(int i, int j, Height height) noexcept { heights[Index(i, j)] = height; }

    [[nodiscard]] static constexpr std::size_t Index(int i, int j) noexcept {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(kTerrainTileVertexCount) +
               static_cast<std::size_t>(i);
    }
};

/// 用纯生成函数填充一个 tile（含共享边界层）。
///
/// 生成与线程顺序无关：每个顶点只依赖 `(全局种子, 世界整数坐标)`（red line 7）。
/// 前置条件：`tile.coord` 已设置，且 `noise` 的生命周期覆盖本调用。
void GenerateTerrainTile(TerrainTile& tile, const TerrainNoiseGenerator& noise) noexcept;

/// 世界列 `(worldX, worldZ)` 是否落在本 tile 的顶点范围内（**含**共享边界层）。
[[nodiscard]] bool TileContainsColumn(const TerrainTile& tile, int worldX, int worldZ) noexcept;

}  // namespace vx
