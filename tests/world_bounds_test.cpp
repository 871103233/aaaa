// T18 世界边界的**纯函数**测试：
//   - 边界盒由 tile 半径推导（两个不同半径、两轴不等半径、单 tile 退化）；
//   - 四周墙的放置（数量 / 与边界盒表面齐平 / 四角封口 / 竖直覆盖地形范围）；
//   - 出界判定（内 / 外 / 恰好落在余量边界），以及"救援一次后下一帧不再触发"这一性质。

#include "out_of_bounds.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/world_bounds.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

using vx::BoundaryWall;
using vx::ComputeBoundaryWalls;
using vx::ComputeWorldBounds;
using vx::IsCharacterOutOfBounds;
using vx::kBoundaryWallThickness;
using vx::kMaxTerrainHeightBlocks;
using vx::kOutOfBoundsMargin;
using vx::kTerrainTileSize;
using vx::kWorldBoundsVerticalHeadroom;
using vx::WorldBounds;

/// 边界盒与墙的判据都取精确值（输入是整数半径与固定常量，无累积误差）。
constexpr double kTolerance = 1e-9;

}  // namespace

// 半径 1（当前测试地图，3×3 个 tile）：世界列 [-64, 128]（含共享边界列），高度 0~512 外扩余量。
TEST(WorldBounds, DerivesExtentFromTileRadiusOne) {
    const WorldBounds bounds = ComputeWorldBounds(1, 1);

    EXPECT_NEAR(bounds.min.x, -static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.x, 2.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.min.z, -static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.z, 2.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.min.y, -kWorldBoundsVerticalHeadroom, kTolerance);
    EXPECT_NEAR(bounds.max.y, static_cast<double>(kMaxTerrainHeightBlocks) + kWorldBoundsVerticalHeadroom, kTolerance);
}

// 半径 3（7×7 个 tile）：范围随半径线性外扩，不依赖半径 1 的任何硬编码值。
TEST(WorldBounds, DerivesExtentFromTileRadiusThree) {
    const WorldBounds bounds = ComputeWorldBounds(3, 3);

    EXPECT_NEAR(bounds.min.x, -3.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.x, 4.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.min.z, -3.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.z, 4.0 * static_cast<double>(kTerrainTileSize), kTolerance);
}

// 两轴半径不同：各自独立推导（X 半径 2、Z 半径 0）。
TEST(WorldBounds, DerivesExtentPerAxis) {
    const WorldBounds bounds = ComputeWorldBounds(2, 0);

    EXPECT_NEAR(bounds.min.x, -2.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.x, 3.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.min.z, 0.0, kTolerance);
    EXPECT_NEAR(bounds.max.z, static_cast<double>(kTerrainTileSize), kTolerance);
}

// 退化：半径 0（单 tile）——覆盖世界列 [0, 64]，不得出现负半径导致的空盒。
TEST(WorldBounds, DerivesExtentForSingleTile) {
    const WorldBounds bounds = ComputeWorldBounds(0, 0);

    EXPECT_NEAR(bounds.min.x, 0.0, kTolerance);
    EXPECT_NEAR(bounds.max.x, static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.min.z, 0.0, kTolerance);
    EXPECT_NEAR(bounds.max.z, static_cast<double>(kTerrainTileSize), kTolerance);
}

// 墙放置：4 堵，内表面与边界盒表面齐平，四角交叠，竖直覆盖整个地形范围。
TEST(WorldBounds, PlacesFourFlushInvisibleWalls) {
    const WorldBounds bounds = ComputeWorldBounds(1, 1);
    const std::array<BoundaryWall, 4> walls = ComputeBoundaryWalls(bounds, kBoundaryWallThickness);

    ASSERT_EQ(walls.size(), static_cast<std::size_t>(4));

    const double halfThickness = kBoundaryWallThickness * 0.5;
    const double centerX       = (bounds.min.x + bounds.max.x) * 0.5;
    const double centerY       = (bounds.min.y + bounds.max.y) * 0.5;
    const double centerZ       = (bounds.min.z + bounds.max.z) * 0.5;
    const double halfY         = (bounds.max.y - bounds.min.y) * 0.5;

    // -X / +X：内表面齐平于 min.x / max.x，厚度方向向外。
    EXPECT_NEAR(walls[0].center.x, bounds.min.x - halfThickness, kTolerance);
    EXPECT_NEAR(walls[0].center.x + walls[0].halfExtents.x, bounds.min.x, kTolerance);
    EXPECT_NEAR(walls[1].center.x, bounds.max.x + halfThickness, kTolerance);
    EXPECT_NEAR(walls[1].center.x - walls[1].halfExtents.x, bounds.max.x, kTolerance);

    // -Z / +Z：内表面齐平于 min.z / max.z。
    EXPECT_NEAR(walls[2].center.z, bounds.min.z - halfThickness, kTolerance);
    EXPECT_NEAR(walls[2].center.z + walls[2].halfExtents.z, bounds.min.z, kTolerance);
    EXPECT_NEAR(walls[3].center.z, bounds.max.z + halfThickness, kTolerance);
    EXPECT_NEAR(walls[3].center.z - walls[3].halfExtents.z, bounds.max.z, kTolerance);

    // 竖直：中心在范围中点，半长覆盖整个边界盒高度（下探到最低地形之下、上探到最高地形之上）。
    for (const BoundaryWall& wall : walls) {
        EXPECT_NEAR(wall.center.y, centerY, kTolerance);
        EXPECT_NEAR(wall.halfExtents.y, halfY, kTolerance);
    }

    // 四角封口：平行于 Z 的墙在 Z 向覆盖到边界外加一个墙厚，平行于 X 的墙同理。
    EXPECT_NEAR(walls[0].halfExtents.z, (bounds.max.z - bounds.min.z) * 0.5 + kBoundaryWallThickness, kTolerance);
    EXPECT_NEAR(walls[2].halfExtents.x, (bounds.max.x - bounds.min.x) * 0.5 + kBoundaryWallThickness, kTolerance);
    EXPECT_NEAR(walls[0].center.z, centerZ, kTolerance);
    EXPECT_NEAR(walls[2].center.x, centerX, kTolerance);

    // 退化厚度：非正厚度按默认厚度（`kBoundaryWallThickness`）处理，不会退化成零宽墙。
    const std::array<BoundaryWall, 4> degenerate = ComputeBoundaryWalls(bounds, 0.0);
    EXPECT_NEAR(degenerate[0].halfExtents.x, halfThickness, kTolerance);
}

// 出界判定：边界盒内一律不救援。
TEST(OutOfBounds, InsideBoundsIsNotRescued) {
    const WorldBounds bounds = ComputeWorldBounds(1, 1);

    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(0.0, 120.0, 0.0), bounds, kOutOfBoundsMargin));
    EXPECT_FALSE(IsCharacterOutOfBounds(bounds.min, bounds, kOutOfBoundsMargin));
    EXPECT_FALSE(IsCharacterOutOfBounds(bounds.max, bounds, kOutOfBoundsMargin));
}

// 出界判定：超出余量之外才救援；**恰好**落在余量边界不算出界（严格不等式，边界确定）。
TEST(OutOfBounds, RescuesOnlyBeyondMarginAndIsExactAtBoundary) {
    const WorldBounds bounds = ComputeWorldBounds(1, 1);

    // 水平越界（X 与 Z 各自）：超出余量一点点 → 救援。
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x + kOutOfBoundsMargin + 0.001, 100.0, 0.0), bounds,
                                       kOutOfBoundsMargin));
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(bounds.min.x - kOutOfBoundsMargin - 0.001, 100.0, 0.0), bounds,
                                       kOutOfBoundsMargin));
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(0.0, 100.0, bounds.min.z - kOutOfBoundsMargin - 0.001), bounds,
                                       kOutOfBoundsMargin));

    // 恰好落在余量边界：不出界。
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x + kOutOfBoundsMargin, 100.0, 0.0), bounds,
                                        kOutOfBoundsMargin));
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(0.0, bounds.min.y - kOutOfBoundsMargin, 0.0), bounds,
                                        kOutOfBoundsMargin));

    // 竖直：低于下沿余量之外 → 救援（坠落）。
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(0.0, bounds.min.y - kOutOfBoundsMargin - 0.001, 0.0), bounds,
                                       kOutOfBoundsMargin));

    // 高于上沿**不**救援：飞行模式允许升到边界盒之上。
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(0.0, bounds.max.y + 1000.0, 0.0), bounds, kOutOfBoundsMargin));

    // 余量为非正时按 0 处理：仍只对"盒外"救援。
    EXPECT_FALSE(IsCharacterOutOfBounds(bounds.max, bounds, -5.0));
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x + 0.001, 100.0, 0.0), bounds, -5.0));
}

// 救援后不重复触发：把出界角色送回出生点（与 main.cpp 一致：位置即出生点、速度为 0），
// 下一帧的判定必须为"不出界"——因此日志天然每次出界只记一次，不会逐帧刷屏。
TEST(OutOfBounds, SingleRescueDoesNotRetriggerOnNextFrame) {
    const WorldBounds bounds = ComputeWorldBounds(1, 1);
    const glm::dvec3   spawn(0.0, 120.5, 0.0);

    glm::dvec3 position(bounds.max.x + kOutOfBoundsMargin + 5.0, 40.0, 0.0);
    ASSERT_TRUE(IsCharacterOutOfBounds(position, bounds, kOutOfBoundsMargin));

    // 救援：`SetCharacterPosition(spawn)` 后位置即为出生点。
    position = spawn;
    EXPECT_FALSE(IsCharacterOutOfBounds(position, bounds, kOutOfBoundsMargin));

    // 反向（另一侧）同样一次即止。
    position = glm::dvec3(0.0, bounds.min.y - kOutOfBoundsMargin - 30.0, 0.0);
    ASSERT_TRUE(IsCharacterOutOfBounds(position, bounds, kOutOfBoundsMargin));
    position = spawn;
    EXPECT_FALSE(IsCharacterOutOfBounds(position, bounds, kOutOfBoundsMargin));
}

// ---- T62：**新尺寸（1×1 km，`tile_radius = [8, 8]`）**下的边界与出界自洽 ----
//
// 依据：`docs/plans/v0.2.md` T62。这三项的期望值**取自 T61 的冒烟日志**（`tile 半径 [8, 8]（289 个 tile）`
// 那一段打印的边界盒 / 4 堵墙 / 救援余量）⇒ "纯函数算的"与"运行时打的"必须逐值一致；
// 世界尺寸本身由地图预设的 tile 半径驱动，故换地图后这些期望值自动跟随（本组测试只是把当前出厂尺寸钉住）。

// 半径 8 = 地图预设上限（每边 512 列 ⇒ 世界跨 1088 列 ≈ 1.09 km），边界盒随之推到 ±512 / 576。
TEST(WorldBounds, DerivesOneKilometerBoundsAtMaxRadius) {
    const WorldBounds bounds = ComputeWorldBounds(8, 8);

    EXPECT_NEAR(bounds.min.x, -512.0, kTolerance);
    EXPECT_NEAR(bounds.max.x, 576.0, kTolerance);
    EXPECT_NEAR(bounds.min.z, -512.0, kTolerance);
    EXPECT_NEAR(bounds.max.z, 576.0, kTolerance);
    EXPECT_NEAR(bounds.min.y, -kWorldBoundsVerticalHeadroom, kTolerance);
    EXPECT_NEAR(bounds.max.y, static_cast<double>(kMaxTerrainHeightBlocks) + kWorldBoundsVerticalHeadroom, kTolerance);

    // 17×17 个 tile ⇒ 每边 1088 列（含共享边界列）。
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 17.0 * static_cast<double>(kTerrainTileSize), kTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 17.0 * static_cast<double>(kTerrainTileSize), kTolerance);
}

// 1 km 尺寸下 4 堵墙仍**内表面齐平 + 竖直盖满地**：数值与冒烟日志逐值一致
// （中心 x = ±513、Z 向半长 546、中心 y = 256、半长 264）。
TEST(WorldBounds, OneKilometerWallsAreFlushAndCoverTheVerticalRange) {
    const WorldBounds                 bounds = ComputeWorldBounds(8, 8);
    const std::array<BoundaryWall, 4> walls  = ComputeBoundaryWalls(bounds, kBoundaryWallThickness);

    ASSERT_EQ(walls.size(), static_cast<std::size_t>(4));

    // 齐平：内表面与边界盒表面重合（玩家走不到墙里、也漏不出去）。
    EXPECT_NEAR(walls[0].center.x + walls[0].halfExtents.x, bounds.min.x, kTolerance);
    EXPECT_NEAR(walls[1].center.x - walls[1].halfExtents.x, bounds.max.x, kTolerance);
    EXPECT_NEAR(walls[2].center.z + walls[2].halfExtents.z, bounds.min.z, kTolerance);
    EXPECT_NEAR(walls[3].center.z - walls[3].halfExtents.z, bounds.max.z, kTolerance);

    // 与运行期日志一致的中心 / 半长（四角靠"平行墙多铺一个墙厚"封口）。
    EXPECT_NEAR(walls[0].center.x, -513.0, kTolerance);
    EXPECT_NEAR(walls[1].center.x, 577.0, kTolerance);
    EXPECT_NEAR(walls[0].halfExtents.z, 546.0, kTolerance);
    EXPECT_NEAR(walls[2].halfExtents.x, 546.0, kTolerance);
    EXPECT_NEAR(walls[0].center.y, 256.0, kTolerance);
    EXPECT_NEAR(walls[0].halfExtents.y, 264.0, kTolerance);
}

// 1 km 世界里的出界救援：贴着墙**内侧**不救援（玩家能沿边走），越过余量才送回出生点（T18 口径不变）。
TEST(OutOfBounds, RescuesAtOneKilometerOnlyBeyondTheMargin) {
    const WorldBounds bounds = ComputeWorldBounds(8, 8);
    const glm::dvec3  spawn(0.0, 120.5, 0.0);

    // 墙内侧一格仍是可玩区（墙挡人、不替人判定出界）。
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(bounds.min.x + 0.5, 120.0, bounds.min.z + 0.5), bounds,
                                        kOutOfBoundsMargin));
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x - 0.5, 120.0, bounds.max.z - 0.5), bounds,
                                        kOutOfBoundsMargin));
    // 恰好落在余量上不算出界；多一毫米才算。
    EXPECT_FALSE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x + kOutOfBoundsMargin, 120.0, 0.0), bounds,
                                        kOutOfBoundsMargin));
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(bounds.max.x + kOutOfBoundsMargin + 0.001, 120.0, 0.0), bounds,
                                       kOutOfBoundsMargin));
    // 坠到盒底之下、飞越 -Z 侧同样被抓回。
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(0.0, bounds.min.y - kOutOfBoundsMargin - 1.0, 0.0), bounds,
                                       kOutOfBoundsMargin));
    EXPECT_TRUE(IsCharacterOutOfBounds(glm::dvec3(0.0, 120.0, bounds.min.z - kOutOfBoundsMargin - 0.001), bounds,
                                       kOutOfBoundsMargin));

    // 救援（送回出生点）后下一帧不再触发。
    EXPECT_FALSE(IsCharacterOutOfBounds(spawn, bounds, kOutOfBoundsMargin));
}
