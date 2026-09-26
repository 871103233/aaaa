#include "dig/terrain_brush.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using vx::ApplyTerrainBrush;
using vx::BrushPose;
using vx::BrushResult;
using vx::Height;
using vx::kHeightUnitsPerBlock;
using vx::kMaxTerrainHeightUnits;
using vx::kMinTerrainHeightUnits;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainTileMesh;
using vx::TerrainWorld;
using vx::TileCoord;

constexpr std::uint64_t kSeed = 0x5EED0001ULL;

using HeightGrid = std::array<Height, static_cast<std::size_t>(vx::kTerrainTileVertexCount) *
                                          static_cast<std::size_t>(vx::kTerrainTileVertexCount)>;

void LoadTiles(TerrainWorld& world, int minTileX, int maxTileX, int minTileZ, int maxTileZ) {
    for (int tileZ = minTileZ; tileZ <= maxTileZ; ++tileZ) {
        for (int tileX = minTileX; tileX <= maxTileX; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }
}

[[nodiscard]] HeightGrid Snapshot(const TerrainTile& tile) {
    HeightGrid copy {};
    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            copy[TerrainTile::Index(i, j)] = tile.At(i, j);
        }
    }
    return copy;
}

/// 与笔刷实现相同的圆盘判据（世界列坐标 = tile(0,0) 的本地坐标）。
[[nodiscard]] bool InsideDisc(int x, int z, const BrushPose& brush) {
    const double dx = static_cast<double>(x) - static_cast<double>(brush.centerX);
    const double dz = static_cast<double>(z) - static_cast<double>(brush.centerZ);
    return dx * dx + dz * dz <= static_cast<double>(brush.radius) * static_cast<double>(brush.radius);
}

[[nodiscard]] bool Contains(const std::vector<TileCoord>& tiles, TileCoord coord) {
    return std::find(tiles.begin(), tiles.end(), coord) != tiles.end();
}

}  // namespace

// T6 ①：挖掘只改动圆盘内的列，圆盘外一律不变。
TEST(TerrainBrush, DigChangesExactlyColumnsInsideRadius) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, -1, 1, -1, 1);

    const TerrainTile* before = world.FindTile(0, 0);
    ASSERT_NE(before, nullptr);
    const HeightGrid snapshot = Snapshot(*before);

    const BrushPose brush { 32.5F, 32.5F, 5.0F };
    constexpr int  kDigUnits = -3 * kHeightUnitsPerBlock;
    const BrushResult result = ApplyTerrainBrush(world, brush, kDigUnits);

    const TerrainTile* after = world.FindTile(0, 0);
    ASSERT_NE(after, nullptr);

    std::size_t expectedChanged = 0;
    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            const Height oldHeight = snapshot[TerrainTile::Index(i, j)];
            const Height newHeight = after->At(i, j);
            if (InsideDisc(i, j, brush)) {
                ASSERT_EQ(newHeight, static_cast<Height>(oldHeight + kDigUnits)) << "本地坐标 (" << i << ", " << j << ")";
                ++expectedChanged;
            } else {
                ASSERT_EQ(newHeight, oldHeight) << "圆盘外的列 (" << i << ", " << j << ") 不得改动";
            }
        }
    }
    EXPECT_EQ(result.changedColumns, expectedChanged);
}

// T6 ②：堆建把圆盘内的列抬高，圆盘外不变。
TEST(TerrainBrush, PileRaisesColumnsInsideRadius) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, -1, 1, -1, 1);

    const TerrainTile* before = world.FindTile(0, 0);
    ASSERT_NE(before, nullptr);
    const HeightGrid snapshot = Snapshot(*before);

    const BrushPose brush { 20.0F, 40.0F, 4.0F };
    constexpr int  kPileUnits = 4 * kHeightUnitsPerBlock;
    const BrushResult result  = ApplyTerrainBrush(world, brush, kPileUnits);

    const TerrainTile* after = world.FindTile(0, 0);
    ASSERT_NE(after, nullptr);

    std::size_t expectedChanged = 0;
    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            const Height oldHeight = snapshot[TerrainTile::Index(i, j)];
            const Height newHeight = after->At(i, j);
            if (InsideDisc(i, j, brush)) {
                ASSERT_EQ(newHeight, static_cast<Height>(oldHeight + kPileUnits)) << "本地坐标 (" << i << ", " << j << ")";
                ++expectedChanged;
            } else {
                ASSERT_EQ(newHeight, oldHeight) << "圆盘外的列 (" << i << ", " << j << ") 不得改动";
            }
        }
    }
    EXPECT_GT(expectedChanged, 0U);
    EXPECT_EQ(result.changedColumns, expectedChanged);
}

// T6 ③：脏 tile 集合恰好等于受影响的 tile —— 完全落在单个 tile 内的笔刷不弄脏邻 tile。
TEST(TerrainBrush, DirtyTilesAreExactlyAffectedAndKeepNeighboursClean) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, -1, 1, -1, 1);

    const BrushPose  inside { 32.0F, 32.0F, 4.0F };
    const BrushResult result = ApplyTerrainBrush(world, inside, -2 * kHeightUnitsPerBlock);

    ASSERT_EQ(result.dirtyTiles.size(), 1U);
    EXPECT_EQ(result.dirtyTiles.front(), (TileCoord { 0, 0 }));
    EXPECT_FALSE(Contains(result.dirtyTiles, TileCoord { -1, 0 }));
    EXPECT_FALSE(Contains(result.dirtyTiles, TileCoord { 0, -1 }));
    EXPECT_FALSE(Contains(result.dirtyTiles, TileCoord { 1, 0 }));
    EXPECT_FALSE(Contains(result.dirtyTiles, TileCoord { 0, 1 }));
}

// T6 ③（边界）：笔刷触及 tile 共享边界列时，相邻 tile 一并被标脏。
TEST(TerrainBrush, BrushCrossingSharedBoundaryDirtiesBothTiles) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, -1, 1, -1, 1);

    const BrushPose  onBoundary { 0.0F, 32.0F, 4.0F };  // 圆心落在世界列 0（tile(-1,0) 与 tile(0,0) 的交界）
    const BrushResult result = ApplyTerrainBrush(world, onBoundary, -2 * kHeightUnitsPerBlock);

    ASSERT_EQ(result.dirtyTiles.size(), 2U);
    EXPECT_TRUE(Contains(result.dirtyTiles, TileCoord { -1, 0 }));
    EXPECT_TRUE(Contains(result.dirtyTiles, TileCoord { 0, 0 }));
}

// T6 ④：高度在垂直范围两端被正确钳制，且钳制后不再重复计入改动。
TEST(TerrainBrush, ClampsAtVerticalLimits) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);

    const BrushPose brush { 32.0F, 32.0F, 3.0F };

    // 抬到上限
    (void)ApplyTerrainBrush(world, brush, kMaxTerrainHeightUnits);
    Height height = 0;
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, height));
    EXPECT_EQ(height, kMaxTerrainHeightUnits);

    // 再抬：已在上限，不应有任何列变化
    const BrushResult up = ApplyTerrainBrush(world, brush, 10 * kHeightUnitsPerBlock);
    EXPECT_EQ(up.changedColumns, 0U);
    EXPECT_TRUE(up.dirtyTiles.empty());
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, height));
    EXPECT_EQ(height, kMaxTerrainHeightUnits);

    // 挖到下限
    (void)ApplyTerrainBrush(world, brush, -kMaxTerrainHeightUnits);
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, height));
    EXPECT_EQ(height, kMinTerrainHeightUnits);

    const BrushResult down = ApplyTerrainBrush(world, brush, -10 * kHeightUnitsPerBlock);
    EXPECT_EQ(down.changedColumns, 0U);
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, height));
    EXPECT_EQ(height, kMinTerrainHeightUnits);
}

// T6 ⑤：`remesh dirty tiles` 只重建指定的 tile，且重建后网格顶点与新高度一致。
TEST(TerrainBrush, RemeshDirtyTilesRegeneratesOnlyThoseTiles) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, 0, 1, 0, 0);

    const TerrainTileMesh* neighbourBefore = world.FindMesh(1, 0);
    ASSERT_NE(neighbourBefore, nullptr);
    const float neighbourHeightBefore = neighbourBefore->mesh.vertices[TerrainTileMesh::VertexIndex(10, 10)].position[1];

    const BrushPose  brush  = { 32.0F, 32.0F, 4.0F };
    const BrushResult result = ApplyTerrainBrush(world, brush, -2 * kHeightUnitsPerBlock);
    ASSERT_EQ(result.dirtyTiles.size(), 1U);

    const std::size_t remeshed = world.RemeshDirtyTiles(result.dirtyTiles);
    EXPECT_EQ(remeshed, 1U);

    Height height = 0;
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, height));
    const TerrainTileMesh* mesh = world.FindMesh(0, 0);
    ASSERT_NE(mesh, nullptr);
    EXPECT_NEAR(mesh->mesh.vertices[TerrainTileMesh::VertexIndex(32, 32)].position[1], vx::HeightToBlocks(height),
                1e-5F);

    // 未标脏的邻 tile 网格原样保留（未被重网格）。
    const TerrainTileMesh* neighbourAfter = world.FindMesh(1, 0);
    ASSERT_NE(neighbourAfter, nullptr);
    EXPECT_FLOAT_EQ(neighbourAfter->mesh.vertices[TerrainTileMesh::VertexIndex(10, 10)].position[1],
                    neighbourHeightBefore);
}
