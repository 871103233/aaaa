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
