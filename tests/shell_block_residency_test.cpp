// W7-S3b① / [ADR 0024]：**Ring 0 地表壳区域的常驻规划**（纯函数）单测。
//
// 验收判据（见 docs/plans/v0.4.md §1.13）：
//   ① 区域正确 —— 列边界是 `kTerrainTileSize` 的倍数、钳制到世界范围；空范围 ⇒ 空区域；
//   ② 确定性 —— 同一入参两次调用逐值相同，且三清单升序；
//   ③ 集合语义 —— `toLoad = 目标 \ 常驻`、`toUnload = 常驻 \ 目标`；
//   ④ 重建语义 —— `regionChanged == false` ⇒ `toRebuild` 为空；`true` ⇒ 最外圈列块 ⊆ `toRebuild`。

#include "shell/surface_shell_residency.hpp"

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "shell/surface_shell.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using vx::BlockCoord;
using vx::kTerrainTileSize;
using vx::kVolumeBlockSize;
using vx::ShellBlockTouchesRegion;
using vx::SurfaceShellParams;
using vx::SurfaceShellRegion;
using vx::SurfaceShellResidencyPlan;
using vx::SurfaceShellSpanCache;
using vx::SurfaceShellWindow;
using vx::TerrainGenerationParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED2024ULL;

[[nodiscard]] SurfaceShellParams MakeParams() {
    SurfaceShellParams params;
    params.bandHalfThicknessBlocks = 24.0F;
    params.edgeFadeBlocks           = 32.0F;
    return params;
}

/// 测试用壳区域：世界列 `[0, 256)²`（8×8 列块；正坐标 + 有内圈，便于验证"最外圈"语义）。
[[nodiscard]] SurfaceShellRegion MakeShellRegion() {
    SurfaceShellRegion region;
    region.minColumnX = 0;
    region.maxColumnX = 256;
    region.minColumnZ = 0;
    region.maxColumnZ = 256;
    return region;
}

/// 世界列范围 `[-78*64, 79*64)²`（与 10 km 世界的 tile 半径 78 同口径）。
[[nodiscard]] SurfaceShellRegion MakeWorldBounds() {
    SurfaceShellRegion bounds;
    bounds.minColumnX = -78 * kTerrainTileSize;
    bounds.maxColumnX = 79 * kTerrainTileSize;
    bounds.minColumnZ = -78 * kTerrainTileSize;
    bounds.maxColumnZ = 79 * kTerrainTileSize;
    return bounds;
}

[[nodiscard]] bool IsSortedAscending(const std::vector<BlockCoord>& blocks) {
    return std::is_sorted(blocks.begin(), blocks.end());
}

}  // namespace

// ① 区域正确：中心 ± 半径覆盖的 tile 列区间；边界是 64 的倍数。
TEST(SurfaceShellResidency, RegionIsTileAlignedAndClamped) {
    const SurfaceShellRegion world = MakeWorldBounds();
    const SurfaceShellWindow  window { 0, 0, 8 };

    const SurfaceShellRegion region = vx::MakeSurfaceShellRegion(window, world);
    EXPECT_EQ(region.minColumnX, -8 * kTerrainTileSize);
    EXPECT_EQ(region.maxColumnX, 9 * kTerrainTileSize);
    EXPECT_EQ(region.minColumnZ, -8 * kTerrainTileSize);
    EXPECT_EQ(region.maxColumnZ, 9 * kTerrainTileSize);
}

TEST(SurfaceShellResidency, RegionIsClampedToWorldBounds) {
    const SurfaceShellRegion world = MakeWorldBounds();
    const SurfaceShellWindow  window { 0, 0, 100 };  // 远大于世界范围 ⇒ 钳到全世界

    const SurfaceShellRegion region = vx::MakeSurfaceShellRegion(window, world);
    EXPECT_EQ(region.minColumnX, world.minColumnX);
    EXPECT_EQ(region.maxColumnX, world.maxColumnX);
    EXPECT_EQ(region.minColumnZ, world.minColumnZ);
    EXPECT_EQ(region.maxColumnZ, world.maxColumnZ);
}

TEST(SurfaceShellResidency, RegionFullyOutsideWorldIsEmpty) {
    const SurfaceShellRegion world = MakeWorldBounds();
    const SurfaceShellWindow  window { 1000, 1000, 8 };  // 世界外 ⇒ 空

    const SurfaceShellRegion region = vx::MakeSurfaceShellRegion(window, world);
    EXPECT_GE(region.minColumnX, region.maxColumnX);
    EXPECT_GE(region.minColumnZ, region.maxColumnZ);
    EXPECT_FALSE(region.ContainsColumn(1000 * kTerrainTileSize, 1000 * kTerrainTileSize));
}

TEST(SurfaceShellResidency, NegativeRadiusBehavesAsSingleTile) {
    const SurfaceShellRegion world = MakeWorldBounds();
    const SurfaceShellWindow  window { 0, 0, -5 };

    const SurfaceShellRegion region = vx::MakeSurfaceShellRegion(window, world);
    EXPECT_EQ(region.maxColumnX - region.minColumnX, kTerrainTileSize);
    EXPECT_EQ(region.maxColumnZ - region.minColumnZ, kTerrainTileSize);
}

// ② 重建厚度：`ceil(edgeFadeBlocks / 32)`；无淡出 ⇒ 0。
TEST(SurfaceShellResidency, RebuildRingThicknessFollowsEdgeFade) {
    SurfaceShellParams params = MakeParams();
    EXPECT_EQ(vx::SurfaceShellRebuildRingThickness(params), 1);

    params.edgeFadeBlocks = 48.0F;
    EXPECT_EQ(vx::SurfaceShellRebuildRingThickness(params), 2);

    params.edgeFadeBlocks = 0.0F;
    EXPECT_EQ(vx::SurfaceShellRebuildRingThickness(params), 0);
}

// ③ 确定性与升序：空常驻 ⇒ 目标全部进 `toLoad`，三清单升序且两次调用逐值相同。
TEST(SurfaceShellResidency, PlanIsDeterministicAndSorted) {
    const TerrainNoiseGenerator      noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams         params = MakeParams();
    const SurfaceShellResidencyPlan plan =
        vx::PlanSurfaceShellResidency(noise, params, MakeShellRegion(), false, {});

    EXPECT_GT(plan.desiredCount, 0U);
    EXPECT_EQ(plan.toLoad.size(), plan.desiredCount);
    EXPECT_TRUE(plan.toUnload.empty());
    EXPECT_TRUE(plan.toRebuild.empty());
    EXPECT_TRUE(IsSortedAscending(plan.toLoad));

    // 目标块都确实与区域相交（无越界块）。
    for (const BlockCoord& block : plan.toLoad) {
        EXPECT_TRUE(ShellBlockTouchesRegion(MakeShellRegion(), block.x, block.z));
    }

    const SurfaceShellResidencyPlan again =
        vx::PlanSurfaceShellResidency(noise, params, MakeShellRegion(), false, {});
    EXPECT_EQ(plan.toLoad, again.toLoad);
    EXPECT_EQ(plan.desiredCount, again.desiredCount);
}

// ③ 集合差：常驻 = 目标的一部分 ⇒ 其余进 `toLoad`、无 `toUnload`；区域外的常驻 ⇒ 进 `toUnload`。
TEST(SurfaceShellResidency, SetDifferenceSemantics) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    const SurfaceShellResidencyPlan full =
        vx::PlanSurfaceShellResidency(noise, params, MakeShellRegion(), false, {});
    ASSERT_GT(full.toLoad.size(), 3U);

    std::vector<BlockCoord> resident(full.toLoad.begin(), full.toLoad.begin() + 3);
    const BlockCoord        outside { 100, 0, 100 };  // 与区域不相交
    resident.push_back(outside);
    std::sort(resident.begin(), resident.end());

    const SurfaceShellResidencyPlan plan =
        vx::PlanSurfaceShellResidency(noise, params, MakeShellRegion(), false, resident);

    EXPECT_EQ(plan.toLoad.size(), full.toLoad.size() - 3U);
    ASSERT_EQ(plan.toUnload.size(), 1U);
    EXPECT_EQ(plan.toUnload.front(), outside);
    EXPECT_TRUE(plan.toRebuild.empty());
}

// ④ 重建语义：全部常驻 + `regionChanged = true` ⇒ 只有"最外圈"列块进入 `toRebuild`。
TEST(SurfaceShellResidency, RebuildOnlyOnRegionChangeAndOnlyOuterRing) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();
    const SurfaceShellRegion    region = MakeShellRegion();

    const SurfaceShellResidencyPlan full =
        vx::PlanSurfaceShellResidency(noise, params, region, false, {});
    ASSERT_GT(full.toLoad.size(), 0U);

    // 不移动区域 ⇒ 不重建。
    const SurfaceShellResidencyPlan same =
        vx::PlanSurfaceShellResidency(noise, params, region, false, full.toLoad);
    EXPECT_TRUE(same.toRebuild.empty());
    EXPECT_TRUE(same.toLoad.empty());
    EXPECT_TRUE(same.toUnload.empty());

    // 区域移动 ⇒ 最外圈列块（X/Z 落在首末块）重建。
    const SurfaceShellResidencyPlan moved =
        vx::PlanSurfaceShellResidency(noise, params, region, true, full.toLoad);
    EXPECT_FALSE(moved.toRebuild.empty());
    EXPECT_TRUE(IsSortedAscending(moved.toRebuild));

    const int blockXMin = region.minColumnX / kVolumeBlockSize;
    const int blockXMax = (region.maxColumnX / kVolumeBlockSize) - 1;
    const int blockZMin = region.minColumnZ / kVolumeBlockSize;
    const int blockZMax = (region.maxColumnZ / kVolumeBlockSize) - 1;
    for (const BlockCoord& block : moved.toRebuild) {
        const bool outer = block.x == blockXMin || block.x == blockXMax || block.z == blockZMin ||
                           block.z == blockZMax;
        EXPECT_TRUE(outer);
    }
}

// ④a 粗采样 Y 范围**必须保守**：⊇ 逐列口径（余量覆盖采样点之间的高度变化 ⇒ 不漏含表面的块）。
TEST(SurfaceShellResidency, CoarseSpanContainsExactSpan) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    for (int bz = -6; bz <= 6; ++bz) {
        for (int bx = -6; bx <= 6; ++bx) {
            const vx::ShellBlockSpan exact  = vx::ComputeShellBlockSpanY(noise, params, bx, bz);
            const vx::ShellBlockSpan coarse = vx::ComputeShellBlockSpanYCoarse(
                noise, params, bx, bz, vx::kShellSpanSampleStepBlocks, vx::kShellSpanMarginHeightBlocks);
            EXPECT_LE(coarse.minBlockY, exact.minBlockY) << "列块 (" << bx << ", " << bz << ")";
            EXPECT_GE(coarse.maxBlockY, exact.maxBlockY) << "列块 (" << bx << ", " << bz << ")";
        }
    }
}

// ④b **Y 范围缓存**：带缓存版与 `noise` 版（逐次重算）结果逐值相同；第二次规划不新增缓存项。
TEST(SurfaceShellResidency, SpanCacheMatchesNoisePlanAndReuses) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();
    const SurfaceShellRegion    region = MakeShellRegion();

    const SurfaceShellResidencyPlan withoutCache = vx::PlanSurfaceShellResidency(noise, params, region, true, {});
    SurfaceShellSpanCache           cache(noise, params);
    const SurfaceShellResidencyPlan withCache = vx::PlanSurfaceShellResidency(cache, params, region, true, {});

    EXPECT_EQ(withCache.desiredCount, withoutCache.desiredCount);
    EXPECT_EQ(withCache.toLoad, withoutCache.toLoad);
    EXPECT_EQ(withCache.toRebuild, withoutCache.toRebuild);
    EXPECT_GT(cache.CachedCount(), 0U);

    const std::size_t cached = cache.CachedCount();
    (void)vx::PlanSurfaceShellResidency(cache, params, region, false, {});
    EXPECT_EQ(cache.CachedCount(), cached) << "同区域的第二次规划不得新增列块缓存项";
}

// ③ 空区域 ⇒ 目标为空、全部常驻进 `toUnload`。
TEST(SurfaceShellResidency, EmptyRegionUnloadsEverything) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    SurfaceShellRegion empty;
    empty.minColumnX = 0;
    empty.maxColumnX = 0;
    empty.minColumnZ = 0;
    empty.maxColumnZ = 0;

    const std::vector<BlockCoord> resident { { 0, 0, 0 }, { 1, 0, 0 } };
    const SurfaceShellResidencyPlan plan = vx::PlanSurfaceShellResidency(noise, params, empty, false, resident);

    EXPECT_EQ(plan.desiredCount, 0U);
    EXPECT_TRUE(plan.toLoad.empty());
    EXPECT_EQ(plan.toUnload, resident);
}

// 负坐标区域（FloorDiv 口径）：目标块不得落到区域之外。
TEST(SurfaceShellResidency, NegativeRegionBlocksStayInside) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    SurfaceShellRegion region;
    region.minColumnX = -4 * kTerrainTileSize;
    region.maxColumnX = 0;
    region.minColumnZ = -4 * kTerrainTileSize;
    region.maxColumnZ = 0;

    const SurfaceShellResidencyPlan plan = vx::PlanSurfaceShellResidency(noise, params, region, false, {});
    EXPECT_GT(plan.desiredCount, 0U);
    for (const BlockCoord& block : plan.toLoad) {
        EXPECT_TRUE(ShellBlockTouchesRegion(region, block.x, block.z));
    }
}
