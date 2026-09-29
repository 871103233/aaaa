// T60：可挖体积的**常驻调度**（窗口计算 / 集合差 / 脏块保护 / 分帧推进）单测。
//
// 口径依据：docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md 决策二 / 五
//           （玩家窗口内常驻、**已改动的块不得卸载**、脏块超上限时淘汰最远者并 WARN）。
//
// 覆盖：块 → tile 映射（含负坐标）/ 窗口计算（含负坐标）/ 窗口包含判定 /
//       集合差（该建 / 该卸）/ **脏块不卸载** / 脏块超上限时淘汰**最远**者 /
//       调度器端到端（初始只常驻窗口 + 走出一格后建新的、卸旧的、**挖过的块仍在**）。

#include "dig/dig_volume.hpp"
#include "streaming/dig_volume_residency.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace {

using vx::BlockCoord;
using vx::DigRegion;
using vx::DigRegionTable;
using vx::DigVolumeScheduler;
using vx::DigVolumeWindow;
using vx::DigVolumeWorld;
using vx::MapEdit;
using vx::MapEditMode;
using vx::MapPreset;
using vx::PlanDigVolumeResidency;
using vx::TerrainMaterialTable;
using vx::TerrainWorld;
using vx::TileOfBlockIndex;
using vx::WindowForPlayerBlocks;

constexpr int kFlatHeightBlocks = 120;  ///< 测试地形：整张图压平到 120 格

[[nodiscard]] DigRegion MakeRegion(const char* name, BlockCoord minimum, BlockCoord maximum) {
    DigRegion region;
    region.name     = name;
    region.diggable = true;
    region.priority = 0;
    region.blockMin = minimum;
    region.blockMax = maximum;
    return region;
}

/// 平坦世界（整图压平到 `kFlatHeightBlocks`）。
/// 与 `dig_volume_test.cpp` 的同名夹具保持一致（该夹具是文件局部的，这里按需复制一份最小版本）。
[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "dig volume residency test（整图压平）";
    preset.seed = 20260929;

    MapEdit flatten;
    flatten.name        = "flat";
    flatten.mode        = MapEditMode::Flatten;
    flatten.minX        = -64;
    flatten.maxX        = 64;
    flatten.minZ        = -64;
    flatten.maxZ        = 64;
    flatten.heightUnits = kFlatHeightBlocks * vx::kHeightUnitsPerBlock;
    preset.edits.push_back(flatten);
    return preset;
}

/// 反复单步直到完成（单测里不需要分帧，但要走**同一条**分步接口）。
void StepUntilDone(DigVolumeWorld& volumes, std::size_t maxSteps = 100000U) {
    std::size_t steps = 0;
    while (!volumes.StepInitFromHeightField(1U) && steps < maxSteps) {
        ++steps;
    }
}

/// 覆盖**3×3 tile**（tile ∈ [-1, 1]）、单层 y 的体积块区域表：块 x/z ∈ [-2, 3]、y = 3。
[[nodiscard]] DigRegionTable FlatRegions() {
    return DigRegionTable::FromRegions(
        { MakeRegion("flat_all", BlockCoord { -2, 3, -2 }, BlockCoord { 3, 3, 3 }) });
}

/// 判断 `coords` 是否含某块。
[[nodiscard]] bool Contains(const std::vector<BlockCoord>& coords, const BlockCoord& wanted) {
    for (const BlockCoord& coord : coords) {
        if (coord == wanted) {
            return true;
        }
    }
    return false;
}

}  // namespace

// 块 → tile 是**精确**映射（块 32 格、tile 64 格 ⇒ 一个块完全落在一个 tile 内）；负坐标必须向下取整。
TEST(DigVolumeResidency, TileOfBlockIndexIsExactAndFloorsNegatives) {
    EXPECT_EQ(TileOfBlockIndex(0), 0);
    EXPECT_EQ(TileOfBlockIndex(1), 0);
    EXPECT_EQ(TileOfBlockIndex(2), 1);
    EXPECT_EQ(TileOfBlockIndex(3), 1);
    EXPECT_EQ(TileOfBlockIndex(-1), -1);
    EXPECT_EQ(TileOfBlockIndex(-2), -1);
    EXPECT_EQ(TileOfBlockIndex(-3), -2);
    EXPECT_EQ(TileOfBlockIndex(-4), -2);
}

// 窗口由**玩家世界坐标**算出（floor(world / 64)），负坐标同样向下取整。
TEST(DigVolumeResidency, WindowFollowsThePlayerTile) {
    const DigVolumeWindow origin = WindowForPlayerBlocks(0.0, 0.0, 2);
    EXPECT_EQ(origin.centerTileX, 0);
    EXPECT_EQ(origin.centerTileZ, 0);
    EXPECT_EQ(origin.MinTileX(), -2);
    EXPECT_EQ(origin.MaxTileZ(), 2);

    const DigVolumeWindow negative = WindowForPlayerBlocks(-100.0, 100.0, 1);
    EXPECT_EQ(negative.centerTileX, -2);  // floor(-100 / 64) = -2
    EXPECT_EQ(negative.centerTileZ, 1);   // floor( 100 / 64) =  1
    EXPECT_EQ(negative.MinTileX(), -3);
    EXPECT_EQ(negative.MaxTileX(), -1);
}

// 窗口包含判定走"块所属 tile"，因此落在窗口 tile 内的**所有**块都算在里面。
TEST(DigVolumeResidency, WindowContainsBlocksOfItsTiles) {
    const DigVolumeWindow window = WindowForPlayerBlocks(0.0, 0.0, 1);  // tile ∈ [-1, 1]

    EXPECT_TRUE(window.ContainsBlock(BlockCoord { 0, 3, 0 }));
    EXPECT_TRUE(window.ContainsBlock(BlockCoord { 1, 3, 1 }));
    EXPECT_TRUE(window.ContainsBlock(BlockCoord { -2, 3, -2 }));  // tile -1
    EXPECT_TRUE(window.ContainsBlock(BlockCoord { 2, 3, 0 }));    // tile 1（上界，含）
    EXPECT_TRUE(window.ContainsBlock(BlockCoord { 3, 3, 1 }));    // tile 1（上界，含）
    EXPECT_FALSE(window.ContainsBlock(BlockCoord { 4, 3, 0 }));   // tile 2 ⇒ 出窗口
    EXPECT_FALSE(window.ContainsBlock(BlockCoord { 0, 3, -4 }));  // tile -2 ⇒ 出窗口
}

// 纯集合差：新进入窗口的要建、离开的要卸；**已被改动的离开者必须留下**（ADR 0020 决策五）。
TEST(DigVolumeResidency, PlanBuildsUnloadsAndKeepsDirtyBlocks) {
    const DigVolumeWindow window = WindowForPlayerBlocks(0.0, 0.0, 0);  // tile (0,0) ⇒ 块 x/z ∈ {0, 1}

    // 区域块：块 x/z ∈ [-2, 3]（3×3 tile 的全部块），单层 y = 3。
    std::vector<BlockCoord> regionBlocks;
    for (int x = -2; x <= 3; ++x) {
        for (int z = -2; z <= 3; ++z) {
            regionBlocks.push_back(BlockCoord { x, 3, z });
        }
    }

    // 常驻集合 = 整张区域（相当于"旧口径：全量常驻"）；其中 (-2,3,-2) 已被玩家挖过。
    const std::vector<BlockCoord> resident = regionBlocks;
    const std::function<bool(const BlockCoord&)> isDirty = [](const BlockCoord& coord) {
        return coord == BlockCoord { -2, 3, -2 };
    };

    const vx::DigVolumeResidencyPlan plan =
        PlanDigVolumeResidency(window, regionBlocks, resident, isDirty, /*maxKeptDirty*/ 256U);

    EXPECT_TRUE(plan.toCreate.empty()) << "窗口内的块本来就常驻 ⇒ 无需新建";
    // 离开窗口的一共 36 − 4 = 32 块，其中 1 块被改动 ⇒ 卸 31、常驻 1。
    EXPECT_EQ(plan.toUnload.size(), 31U);
    EXPECT_EQ(plan.keptDirty.size(), 1U);
    EXPECT_TRUE(plan.evictedDirty.empty());
    EXPECT_TRUE(Contains(plan.keptDirty, BlockCoord { -2, 3, -2 }));
    EXPECT_FALSE(Contains(plan.toUnload, BlockCoord { -2, 3, -2 })) << "改动过的块**不得**出现在卸载清单里";

    // 窗口向 +x 移一格（tile (1,0) ⇒ 块 x ∈ {2, 3}）⇒ 该建 4 块（x∈{2,3}
    // 原本就在常驻里 ⇒ 这里以"只常驻窗口"为前提再算一次）。
    const std::vector<BlockCoord> onlyWindow { { 0, 3, 0 }, { 1, 3, 0 }, { 0, 3, 1 }, { 1, 3, 1 } };
    const DigVolumeWindow         moved = WindowForPlayerBlocks(64.0, 0.0, 0);
    const vx::DigVolumeResidencyPlan second =
        PlanDigVolumeResidency(moved, regionBlocks, onlyWindow, isDirty, /*maxKeptDirty*/ 256U);
    EXPECT_EQ(second.toCreate.size(), 4U);
    EXPECT_EQ(second.toUnload.size(), 4U);
    EXPECT_TRUE(second.keptDirty.empty()) << "旧窗口那 4 块都没被改动过 ⇒ 全部可卸";
}

// 脏块超上限 ⇒ 淘汰**最远**的那些（且交给调用方 WARN）；留下的仍在 `keptDirty` 里（不得卸载）。
TEST(DigVolumeResidency, PlanEvictsTheFarthestDirtyBlocksWhenOverTheCap) {
    // 窗口在 tile (0,0)、半径 0：离开窗口的脏块分别位于 tile -3 与 tile 3（离中心等距 3），
    // 以及 tile -1（离中心 1，最近）。上限 2 ⇒ 必须淘汰最远的两个。
    const DigVolumeWindow window = WindowForPlayerBlocks(0.0, 0.0, 0);

    std::vector<BlockCoord> regionBlocks;
    for (int x = -8; x <= 8; ++x) {
        regionBlocks.push_back(BlockCoord { x, 3, 0 });
    }
    const std::vector<BlockCoord> resident = regionBlocks;

    const std::function<bool(const BlockCoord&)> isDirty = [](const BlockCoord& coord) {
        return coord.x == -6 || coord.x == -2 || coord.x == 6;  // tile -3 / -1 / 3
    };

    const vx::DigVolumeResidencyPlan plan =
        PlanDigVolumeResidency(window, regionBlocks, resident, isDirty, /*maxKeptDirty*/ 1U);

    // 三个脏块里留下"最近的"一个（tile -1 ⇒ 块 -2），淘汰另两个（tile -3 ⇒ 块 -6、tile 3 ⇒ 块 6）。
    ASSERT_EQ(plan.keptDirty.size(), 1U);
    EXPECT_TRUE(Contains(plan.keptDirty, BlockCoord { -2, 3, 0 }));
    ASSERT_EQ(plan.evictedDirty.size(), 2U);
    EXPECT_TRUE(Contains(plan.evictedDirty, BlockCoord { -6, 3, 0 }));
    EXPECT_TRUE(Contains(plan.evictedDirty, BlockCoord { 6, 3, 0 }));
}

// T60 / T61：**淘汰**是"脏块不得卸载"这条硬规则的**唯一**出口 —— 普通通道拒绝脏块，淘汰通道放行。
// 这条契约把调度器 `Step` 的末段（调用 `EvictBlock` 而不是 `UnloadBlock`）钉死：
// 否则超上限的脏块既卸不掉、又会被反复重新计划 ⇒ WARN 逐次刷屏且内存上界失效。
TEST(DigVolumeResidency, EvictBlockIsTheOnlyWayToDropADirtyBlock) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    for (int tileZ = -1; tileZ <= 1; ++tileZ) {
        for (int tileX = -1; tileX <= 1; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }

    DigVolumeWorld volumes(world, FlatRegions());

    const BlockCoord              block { 0, 3, 0 };
    const std::vector<BlockCoord> only { block };
    volumes.BeginInitFromHeightField(only);
    StepUntilDone(volumes);
    ASSERT_EQ(volumes.ResidentBlocks().size(), 1U);

    std::vector<BlockCoord> dirty;
    ASSERT_TRUE(volumes.CarveSphere(glm::dvec3(8.0, 116.0, 8.0), 4.0F, dirty));
    ASSERT_TRUE(volumes.IsBlockDirty(block));

    EXPECT_FALSE(volumes.UnloadBlock(block)) << "普通通道必须拒绝脏块（ADR 0020 决策五：洞不得消失）";
    EXPECT_TRUE(volumes.EvictBlock(block)) << "淘汰通道必须能卸下脏块（内存上界的出口）";
    EXPECT_TRUE(volumes.ResidentBlocks().empty());
    EXPECT_FALSE(volumes.EvictBlock(block)) << "块已不在 ⇒ 返回 false（幂等）";
}

// 调度器端到端：**初始只常驻窗口**；窗口前移一格 ⇒ 建新的、卸旧的；**被挖过的块留在常驻里**。
TEST(DigVolumeResidency, SchedulerKeepsWindowResidentAndNeverUnloadsCarvedBlocks) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    for (int tileZ = -1; tileZ <= 1; ++tileZ) {
        for (int tileX = -1; tileX <= 1; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }

    const DigRegionTable regions = FlatRegions();  // 36 块（3×3 tile × 单层）
    ASSERT_EQ(regions.Blocks().size(), 36U);

    DigVolumeWorld     volumes(world, regions);
    DigVolumeScheduler scheduler(regions, /*radiusTiles*/ 0);  // 只保留玩家所在的那一个 tile

    // 初始：由调度器的窗口算出"该常驻哪些块"，**只初始化它们**（而不是 36 块全量）。
    const DigVolumeWindow window = vx::WindowForPlayerBlocks(0.0, 0.0, 0);
    std::vector<BlockCoord> initial;
    for (const BlockCoord& coord : regions.Blocks()) {
        if (window.ContainsBlock(coord)) {
            initial.push_back(coord);
        }
    }
    ASSERT_EQ(initial.size(), 4U);
    volumes.BeginInitFromHeightField(initial);
    StepUntilDone(volumes);
    ASSERT_EQ(volumes.ResidentBlocks().size(), 4U) << "初始常驻 = 窗口，而不是全部区域块";

    // 调度器首次 Update：窗口已达成 ⇒ 无待办。
    EXPECT_FALSE(scheduler.Update(volumes, 0.0, 0.0));
    EXPECT_EQ(scheduler.DesiredCount(), 4U);
    EXPECT_EQ(scheduler.PendingCreateCount(), 0U);
    EXPECT_EQ(scheduler.PendingUnloadCount(), 0U);

    // 把窗口内的一个块挖一下（⇒ 变成脏块，必须永不卸载）。
    const BlockCoord carvedBlock { 0, 3, 0 };
    std::vector<BlockCoord> dirty;
    ASSERT_TRUE(volumes.CarveSphere(glm::dvec3(8.0, 116.0, 8.0), 4.0F, dirty));
    EXPECT_TRUE(volumes.IsBlockDirty(carvedBlock));

    // 玩家前进一格 tile（世界列 +64）⇒ 该建 4 块（x ∈ {2,3}）、该卸 3 块（旧的 4 块里被挖过的那块留下）。
    ASSERT_TRUE(scheduler.Update(volumes, 64.0, 0.0));
    EXPECT_EQ(scheduler.PendingCreateCount(), 4U);
    EXPECT_EQ(scheduler.PendingUnloadCount(), 3U);
    EXPECT_EQ(scheduler.KeptDirtyCount(), 1U);

    std::vector<BlockCoord> changed;
    EXPECT_TRUE(scheduler.Step(volumes, 100U, changed));
    EXPECT_FALSE(scheduler.HasPendingWork());
    EXPECT_EQ(changed.size(), 7U) << "4 建 + 3 卸";

    // 常驻 = 新窗口的 4 块 + 留下来的一块脏块。
    const std::vector<BlockCoord> resident = volumes.ResidentBlocks();
    EXPECT_EQ(resident.size(), 5U);
    EXPECT_TRUE(Contains(resident, carvedBlock)) << "**挖过的块必须仍在常驻里**（洞不会消失）";
    EXPECT_TRUE(volumes.IsBlockDirty(carvedBlock));

    // 反方向再走一格（回到 tile (0,0)）⇒ 该建 3 块（旧窗口那 3 块已卸）、该卸新窗口那 4 块。
    ASSERT_TRUE(scheduler.Update(volumes, 0.0, 0.0));
    EXPECT_EQ(scheduler.PendingCreateCount(), 3U);
    EXPECT_EQ(scheduler.PendingUnloadCount(), 4U);
}
