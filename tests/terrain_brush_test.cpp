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
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using vx::ApplyTerrainBrush;
using vx::ApplyTerrainCrater;
using vx::ApplyTerrainLevel;
using vx::BrushFalloff;
using vx::BrushPose;
using vx::BrushResult;
using vx::BrushSettings;
using vx::BrushTable;
using vx::Height;
using vx::HeightToBlocks;
using vx::kHeightUnitsPerBlock;
using vx::kMaxTerrainHeightUnits;
using vx::kMinTerrainHeightUnits;
using vx::LevelMode;
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

// ============================================================
// T26：笔刷能力升级（平整填充 / 削平破坏 / 平滑爆破）
// ============================================================

// ---- BrushFalloff 纯函数：端点、单调性、边界一阶连续 ----
TEST(TerrainBrushLevels, FalloffIsSmoothAndMonotonic) {
    const float radius = 6.0F;
    const float band   = 0.6F;

    EXPECT_FLOAT_EQ(BrushFalloff(0.0F, radius, band), 1.0F);
    EXPECT_FLOAT_EQ(BrushFalloff(radius, radius, band), 0.0F);
    EXPECT_FLOAT_EQ(BrushFalloff(radius * 1.5F, radius, band), 0.0F);

    // 区间内单调不增、值域 [0, 1]。
    float previous = 1.0F;
    for (int i = 0; i <= 600; ++i) {
        const float d = static_cast<float>(i) / 100.0F * radius;  // 0 .. 6
        const float f = BrushFalloff(d, radius, band);
        EXPECT_GE(f, 0.0F);
        EXPECT_LE(f, 1.0F);
        EXPECT_LE(f, previous + 1e-6F) << "d=" << d;
        previous = f;
    }

    // 边界一阶连续：d = r 两侧的单侧数值导数都趋于 0（硬边会在此处出现有限的跳变斜率）。
    const float h          = radius * 1e-3F;
    const float innerSlope = (BrushFalloff(radius, radius, band) - BrushFalloff(radius - h, radius, band)) / h;
    const float outerSlope = (BrushFalloff(radius + h, radius, band) - BrushFalloff(radius, radius, band)) / h;
    EXPECT_NEAR(outerSlope, 0.0F, 1e-6F);
    EXPECT_LT(std::abs(innerSlope), 0.05F) << "边界内侧斜率应趋于 0（无硬台阶）";

    // band = 0 退化为硬边：d < r → 1、d ≥ r → 0。
    EXPECT_FLOAT_EQ(BrushFalloff(radius * 0.999F, radius, 0.0F), 1.0F);
    EXPECT_FLOAT_EQ(BrushFalloff(radius, radius, 0.0F), 0.0F);
}

// ---- 爆破平滑性回归（本项核心判据）----
//
// 判据：① 直径剖面上高度的**二阶差分绝对值 ≤ 阈值**（无硬边）；
//       ② 半径外逐列不变、边界处差值与内侧最后一步**同量级**（无台阶）。
// 为使阈值可判定，另建一条"硬边对照"（falloff = 0）：其边界会出现台阶，用于证明本测确实在测平滑性。
TEST(TerrainBrushLevels, CraterIsSmoothAcrossBoundary) {
    constexpr std::uint64_t kSeedLocal = 0x5EED0002ULL;
    const float baseBlocks = 120.0F;
    const Height baseUnits = static_cast<Height>(std::lround(baseBlocks * static_cast<float>(kHeightUnitsPerBlock)));
    const float depth = 6.0F;
    const float rim   = 2.0F;
    const float radius = 8.0F;

    const auto buildProfile = [&](float falloffBand, std::array<float, 65>& heights) {
        TerrainWorld world(kSeedLocal, TerrainMaterialTable::Default());
        world.LoadTile(0, 0);
        std::vector<TileCoord> scratch;
        for (int x = 0; x <= 64; ++x) {
            world.WriteColumnHeight(x, 32, baseUnits, scratch);  // 沿 z = 32 铺一条平线
        }
        const BrushPose brush { 32.0F, 32.0F, radius };
        (void)ApplyTerrainCrater(world, brush, depth, rim, radius, falloffBand);
        for (int x = 0; x <= 64; ++x) {
            Height h = 0;
            EXPECT_TRUE(world.ReadColumnHeight(x, 32, h)) << "x=" << x;
            heights[x] = HeightToBlocks(h);
        }
    };

    std::array<float, 65> smooth {};
    std::array<float, 65> hard {};
    buildProfile(0.6F, smooth);
    buildProfile(0.0F, hard);  // 硬边对照

    const auto maxSecondDifference = [](const std::array<float, 65>& h) {
        float maxAbs = 0.0F;
        for (int x = 1; x <= 63; ++x) {
            maxAbs = std::max(maxAbs, std::abs(h[x - 1] - 2.0F * h[x] + h[x + 1]));
        }
        return maxAbs;
    };
    const float smoothSecond = maxSecondDifference(smooth);
    const float hardSecond   = maxSecondDifference(hard);

    // ① 二阶差分 ≤ 2 格：实测平滑剖面 1.3125 格、硬边对照 6.3125 格、坑深 6 格，
    //    故该阈值既远小于"整坑深量级的台阶"，又明显高于平滑剖面的固有曲率。
    EXPECT_LE(smoothSecond, 2.0F) << "smoothSecond=" << smoothSecond;
    EXPECT_LT(smoothSecond, hardSecond) << "smoothSecond=" << smoothSecond << " hardSecond=" << hardSecond;

    // ② 半径外（d ≥ R）逐列不变。
    for (int x = 0; x <= 64; ++x) {
        const float d = std::abs(static_cast<float>(x) - 32.0F);
        if (d >= radius - 1e-4F) {
            EXPECT_NEAR(smooth[static_cast<std::size_t>(x)], baseBlocks, 0.1F) << "半径外 x=" << x;
        }
    }

    // ② 边界处差值与内侧最后一步同量级（无台阶）。x=24/40 是 d=R 的边界列（应等于基准）。
    const float boundaryStep = std::abs(smooth[25] - smooth[24]);  // d=7 → d=8（最外一格）
    const float innerStep    = std::abs(smooth[26] - smooth[25]);  // d=6 → d=7
    EXPECT_LE(boundaryStep, 5.0F * innerStep + 0.2F)
        << "boundaryStep=" << boundaryStep << " innerStep=" << innerStep;
    // 硬边对照确实出现台阶 ⇒ 证明上面的"同量级"断言不是恒真。
    EXPECT_GT(hard[24] - hard[25], 2.5F) << "硬边对照应出现明显台阶";
}

// ---- 平整：半径内向目标收敛，且 Fill 只抬升、Shave 只削低 ----
TEST(TerrainBrushLevels, LevelFillConvergesWithinRadiusAndRaisesOnly) {
    TerrainWorld world(0x5EED0003ULL, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    std::vector<TileCoord> scratch;

    const float    target      = 120.0F;
    const BrushPose brush { 32.0F, 32.0F, 6.0F };

    // 半径内一律**低于**目标（三种不同高度），半径外固定 100 格。
    for (int z = 0; z <= 64; ++z) {
        for (int x = 0; x <= 64; ++x) {
            const float d = std::sqrt(static_cast<float>((x - 32) * (x - 32) + (z - 32) * (z - 32)));
            const float h = (d <= 6.0F) ? (target - 3.0F + static_cast<float>((x + z) % 3) * 1.0F) : 100.0F;
            world.WriteColumnHeight(x, z, static_cast<Height>(std::lround(h * kHeightUnitsPerBlock)), scratch);
        }
    }
    Height outsideBefore = 0;
    ASSERT_TRUE(world.ReadColumnHeight(0, 32, outsideBefore));

    for (int iter = 0; iter < 800; ++iter) {
        (void)ApplyTerrainLevel(world, brush, target, /*maxStepBlocks=*/1.0F, 0.6F, LevelMode::Fill);
    }

    float minimum = 1e9F;
    float maximum = -1e9F;
    for (int z = 0; z <= 64; ++z) {
        for (int x = 0; x <= 64; ++x) {
            const float d = std::sqrt(static_cast<float>((x - 32) * (x - 32) + (z - 32) * (z - 32)));
            if (d > 6.0F) {
                continue;
            }
            Height h = 0;
            ASSERT_TRUE(world.ReadColumnHeight(x, z, h));
            const float hb = HeightToBlocks(h);
            EXPECT_GE(hb, target - 1e-3F) << "Fill 不得把低处抬过目标 (x=" << x << ",z=" << z << ")";
            minimum = std::min(minimum, hb);
            maximum = std::max(maximum, hb);
        }
    }
    // 半径内高度极差 ≤ 1 个定点单位（收敛到目标；实测 = 0）。
    EXPECT_LE(maximum - minimum, 1.0F / static_cast<float>(kHeightUnitsPerBlock) + 1e-4F)
        << "平整后极差=" << (maximum - minimum);

    Height outsideAfter = 0;
    ASSERT_TRUE(world.ReadColumnHeight(0, 32, outsideAfter));
    EXPECT_EQ(outsideAfter, outsideBefore) << "半径外逐列不变";
}

TEST(TerrainBrushLevels, LevelShaveLowersOnlyHighColumnsAndDoesNotRaiseLow) {
    TerrainWorld world(0x5EED0004ULL, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    std::vector<TileCoord> scratch;

    const float     target = 120.0F;
    const BrushPose brush { 32.0F, 32.0F, 6.0F };

    // 半径内一律**高于**目标，仅在中心留一处低于目标的凹点，用于验证 Shave 不把它抬起。
    for (int z = 0; z <= 64; ++z) {
        for (int x = 0; x <= 64; ++x) {
            const float d = std::sqrt(static_cast<float>((x - 32) * (x - 32) + (z - 32) * (z - 32)));
            float       h = 100.0F;
            if (d <= 6.0F) {
                h = (x == 32 && z == 32) ? (target - 2.0F) : (target + 3.0F);
            }
            world.WriteColumnHeight(x, z, static_cast<Height>(std::lround(h * kHeightUnitsPerBlock)), scratch);
        }
    }

    for (int iter = 0; iter < 800; ++iter) {
        (void)ApplyTerrainLevel(world, brush, target, /*maxStepBlocks=*/1.0F, 0.6F, LevelMode::Shave);
    }

    // 高于目标处收敛到目标；低于目标的凹点**未被抬起**（Shave 单向）。
    for (int z = 0; z <= 64; ++z) {
        for (int x = 0; x <= 64; ++x) {
            const float d = std::sqrt(static_cast<float>((x - 32) * (x - 32) + (z - 32) * (z - 32)));
            if (d > 6.0F) {
                continue;
            }
            Height h = 0;
            ASSERT_TRUE(world.ReadColumnHeight(x, z, h));
            const float hb = HeightToBlocks(h);
            EXPECT_LE(hb, target + 1e-3F) << "Shave 不得把高处削过目标";
            if (x == 32 && z == 32) {
                EXPECT_NEAR(hb, target - 2.0F, 1e-3F) << "Shave 不得把低处抬起";
            } else {
                EXPECT_NEAR(hb, target, 1.0F / static_cast<float>(kHeightUnitsPerBlock)) << "高处应削到目标";
            }
        }
    }
}

// ---- 脏 tile 精确性：完全落在单 tile 内只弄脏该 tile；跨共享边界弄脏两个 ----
TEST(TerrainBrushLevels, LevelAndCraterDirtyExactlyAffectedTiles) {
    TerrainWorld world(0x5EED0005ULL, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(-1, 0);
    world.LoadTile(1, 0);
    // 先把可能被触及的范围铺成低地，使"向目标收敛"必然产生改动（与随机生成高度无关）。
    std::vector<TileCoord> scratch;
    const Height lowUnits = static_cast<Height>(100 * kHeightUnitsPerBlock);
    for (int z = 20; z <= 44; ++z) {
        for (int x = -10; x <= 44; ++x) {
            world.WriteColumnHeight(x, z, lowUnits, scratch);
        }
    }

    const BrushPose inside { 32.0F, 32.0F, 6.0F };
    const BrushResult levelResult = ApplyTerrainLevel(world, inside, 120.0F, 1.0F, 0.6F, LevelMode::Fill);
    ASSERT_EQ(levelResult.dirtyTiles.size(), 1U);
    EXPECT_EQ(levelResult.dirtyTiles.front(), (TileCoord { 0, 0 }));

    const BrushResult craterResult = ApplyTerrainCrater(world, inside, 6.0F, 2.0F, 8.0F, 0.6F);
    ASSERT_EQ(craterResult.dirtyTiles.size(), 1U);
    EXPECT_EQ(craterResult.dirtyTiles.front(), (TileCoord { 0, 0 }));

    // 圆心落在世界列 0（tile(-1,0) 与 tile(0,0) 的交界）⇒ 两个 tile 一并被标脏。
    const BrushPose onBoundary { 0.0F, 32.0F, 6.0F };
    const BrushResult crossing = ApplyTerrainCrater(world, onBoundary, 6.0F, 2.0F, 6.0F, 0.6F);
    EXPECT_TRUE(Contains(crossing.dirtyTiles, TileCoord { -1, 0 }));
    EXPECT_TRUE(Contains(crossing.dirtyTiles, TileCoord { 0, 0 }));
}

// ---- 垂直范围钳制：爆破 / 平整都不得越界（既有断言在长按测试中已覆盖，这里补新模式的边界）----
TEST(TerrainBrushLevels, CraterClampsAtVerticalLimits) {
    TerrainWorld world(0x5EED0006ULL, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    std::vector<TileCoord> scratch;
    for (int z = 20; z <= 44; ++z) {
        for (int x = 20; x <= 44; ++x) {
            world.WriteColumnHeight(x, z, static_cast<Height>(kMinTerrainHeightUnits), scratch);
        }
    }
    const BrushPose center { 32.0F, 32.0F, 8.0F };
    // 已在地板：再爆破不得下穿 0，也不得让外环把某列抬到 0 以下。
    const BrushResult result = ApplyTerrainCrater(world, center, 6.0F, 2.0F, 8.0F, 0.6F);
    for (int z = 20; z <= 44; ++z) {
        for (int x = 20; x <= 44; ++x) {
            Height h = 0;
            ASSERT_TRUE(world.ReadColumnHeight(x, z, h));
            EXPECT_GE(static_cast<int>(h), kMinTerrainHeightUnits);
            EXPECT_LE(static_cast<int>(h), kMaxTerrainHeightUnits);
        }
    }
    (void)result;
}

// ============================================================
// T26：brush.toml 配置解析与校验（口径与 materials.toml / lighting.toml 一致）
// ============================================================

namespace {

constexpr const char* kValidBrushConfig =
    "schema_version = 1\n"
    "radius = 6.0\n"
    "strength = 6.0\n"
    "falloff = 0.6\n"
    "crater_depth = 6.0\n"
    "crater_rim = 2.0\n"
    "crater_radius = 8.0\n";

[[nodiscard]] std::filesystem::path WriteBrushConfig(const char* name, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream               out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

void RemoveBrushConfig(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace

TEST(TerrainBrushConfig, LoadsCommittedConfigAndMatchesDefaults) {
    const std::filesystem::path path = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "brush.toml";
    const BrushTable            loaded = BrushTable::LoadFromFile(path);
    const BrushTable            builtin = BrushTable::Default();

    EXPECT_EQ(loaded.SchemaVersion(), BrushTable::kSchemaVersion);
    EXPECT_FLOAT_EQ(loaded.Settings().radius, 6.0F);
    EXPECT_FLOAT_EQ(loaded.Settings().strength, 6.0F);
    EXPECT_FLOAT_EQ(loaded.Settings().falloff, 0.6F);
    EXPECT_FLOAT_EQ(loaded.Settings().craterDepth, 6.0F);
    EXPECT_FLOAT_EQ(loaded.Settings().craterRim, 2.0F);
    EXPECT_FLOAT_EQ(loaded.Settings().craterRadius, 8.0F);

    EXPECT_FLOAT_EQ(loaded.Settings().radius, builtin.Settings().radius);
    EXPECT_FLOAT_EQ(loaded.Settings().strength, builtin.Settings().strength);
    EXPECT_FLOAT_EQ(loaded.Settings().falloff, builtin.Settings().falloff);
    EXPECT_FLOAT_EQ(loaded.Settings().craterDepth, builtin.Settings().craterDepth);
    EXPECT_FLOAT_EQ(loaded.Settings().craterRim, builtin.Settings().craterRim);
    EXPECT_FLOAT_EQ(loaded.Settings().craterRadius, builtin.Settings().craterRadius);
}

TEST(TerrainBrushConfig, ValidConfigParses) {
    const std::filesystem::path path = WriteBrushConfig("vx_brush_valid.toml", kValidBrushConfig);
    const BrushTable            table = BrushTable::LoadFromFile(path);
    EXPECT_FLOAT_EQ(table.Settings().radius, 6.0F);
    EXPECT_FLOAT_EQ(table.Settings().strength, 6.0F);
    EXPECT_FLOAT_EQ(table.Settings().falloff, 0.6F);
    EXPECT_FLOAT_EQ(table.Settings().craterDepth, 6.0F);
    EXPECT_FLOAT_EQ(table.Settings().craterRim, 2.0F);
    EXPECT_FLOAT_EQ(table.Settings().craterRadius, 8.0F);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, MissingFileThrows) {
    EXPECT_THROW((void)BrushTable::LoadFromFile("no_such_brush_file.toml"), std::runtime_error);
}

TEST(TerrainBrushConfig, InvalidSchemaThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "schema_version = 1";
    content.replace(content.find(from), from.size(), "schema_version = 99");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_schema.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, MissingFieldThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "strength = 6.0\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteBrushConfig("vx_brush_missing.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, NonPositiveRadiusThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "radius = 6.0";
    content.replace(content.find(from), from.size(), "radius = 0.0");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_radius.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, NonPositiveStrengthThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "strength = 6.0";
    content.replace(content.find(from), from.size(), "strength = -1.0");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_strength.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, FalloffOutOfRangeThrows) {
    for (const char* replacement : { "falloff = 0.0", "falloff = 1.5" }) {
        std::string       content = kValidBrushConfig;
        const std::string from    = "falloff = 0.6";
        content.replace(content.find(from), from.size(), replacement);
        const std::filesystem::path path = WriteBrushConfig("vx_brush_falloff.toml", content);
        EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error) << replacement;
        RemoveBrushConfig(path);
    }
}

TEST(TerrainBrushConfig, NonPositiveCraterDepthThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "crater_depth = 6.0";
    content.replace(content.find(from), from.size(), "crater_depth = 0.0");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_depth.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, NegativeCraterRimThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "crater_rim = 2.0";
    content.replace(content.find(from), from.size(), "crater_rim = -1.0");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_rim.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}

TEST(TerrainBrushConfig, NonPositiveCraterRadiusThrows) {
    std::string content = kValidBrushConfig;
    const std::string from = "crater_radius = 8.0";
    content.replace(content.find(from), from.size(), "crater_radius = 0.0");
    const std::filesystem::path path = WriteBrushConfig("vx_brush_crater_radius.toml", content);
    EXPECT_THROW((void)BrushTable::LoadFromFile(path), std::runtime_error);
    RemoveBrushConfig(path);
}
