#include "streaming/terrain_tile_residency.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using vx::kTerrainResidencyPrefetchTiles;
using vx::kTerrainTileSize;
using vx::kTerrainWindowHysteresisBlocks;
using vx::MakeTerrainTileRange;
using vx::PlanTerrainTileResidency;
using vx::TerrainHysteresisCenterTile;
using vx::TerrainLodLevelForTileDistance;
using vx::TerrainLodMorphRange;
using vx::TerrainLodMorphRangeForLevel;
using vx::TerrainLodRings;
using vx::TerrainMaterialTable;
using vx::TerrainResidencyWindowForPlayerBlocks;
using vx::TerrainTileRange;
using vx::TerrainTileResidencyPlan;
using vx::TerrainTileScheduler;
using vx::TerrainTileWindow;
using vx::TerrainWindowForPlayerBlocks;
using vx::TerrainWorld;
using vx::TileCoord;

/// 世界内可用的 tile（`[minX, maxX] × [minZ, maxZ]`），升序。
[[nodiscard]] std::vector<TileCoord> MakeAvailable(int minX, int maxX, int minZ, int maxZ) {
    std::vector<TileCoord> tiles;
    for (int x = minX; x <= maxX; ++x) {
        for (int z = minZ; z <= maxZ; ++z) {
            tiles.push_back(TileCoord { x, z });
        }
    }
    return tiles;
}

/// 世界列坐标 `tileIndex` 的中心（格）—— 用于把"玩家在第 i 个 tile 中心"写得直观。
[[nodiscard]] double TileCenterBlocks(int tileIndex) {
    return (static_cast<double>(tileIndex) + 0.5) * static_cast<double>(kTerrainTileSize);
}

}  // namespace

// 活动窗口：中心 tile = floor(world / 64)，含负坐标；半径与 Chebyshev 距离正确。
TEST(TerrainTileResidency, WindowCentersOnPlayerTileAndCoversTheRadius) {
    const TerrainTileWindow window = TerrainWindowForPlayerBlocks(100.0, -1.0, 2);

    EXPECT_EQ(window.centerTileX, 1);   // floor(100 / 64) = 1
    EXPECT_EQ(window.centerTileZ, -1);  // floor(-1 / 64) = -1（负坐标向下取整）
    EXPECT_EQ(window.radiusTiles, 2);
    EXPECT_EQ(window.MinTileX(), -1);
    EXPECT_EQ(window.MaxTileX(), 3);
    EXPECT_EQ(window.MinTileZ(), -3);
    EXPECT_EQ(window.MaxTileZ(), 1);

    EXPECT_TRUE(window.ContainsTile(0, 0));
    EXPECT_TRUE(window.Contains(TileCoord { 3, -3 }));
    EXPECT_FALSE(window.ContainsTile(4, -1));

    EXPECT_EQ(window.TileDistanceFromCenter(TileCoord { 3, -3 }), 2);  // Chebyshev（不是曼哈顿）
    EXPECT_EQ(window.TileCount(), 25U);                               // (2·2+1)²
}

// 常驻窗口 = 活动半径 + 预取环宽；常驻量只由窗口决定（与世界总大小无关）。
TEST(TerrainTileResidency, ResidencyWindowAddsPrefetchRingAndStaysBounded) {
    const TerrainTileWindow active    = TerrainWindowForPlayerBlocks(0.0, 0.0, 2);
    const TerrainTileWindow residency = TerrainResidencyWindowForPlayerBlocks(0.0, 0.0, 2, kTerrainResidencyPrefetchTiles);

    EXPECT_EQ(residency.centerTileX, active.centerTileX);  // 共用同一个中心 tile
    EXPECT_EQ(residency.centerTileZ, active.centerTileZ);
    EXPECT_EQ(residency.radiusTiles, 2 + kTerrainResidencyPrefetchTiles);
    EXPECT_EQ(residency.TileCount(), 49U);  // (2·3+1)²

    // 常驻集合是活动集合的超集（预取环）。
    EXPECT_TRUE(residency.ContainsTile(active.MaxTileX() + 1, 0));

    // **与世界总大小无关**：把 radiusTiles 换成 0，常驻量只由窗口决定（1 个 tile），不会随 available 增大。
    const TerrainTileWindow single = TerrainResidencyWindowForPlayerBlocks(0.0, 0.0, 0, 0);
    EXPECT_EQ(single.TileCount(), 1U);
}

// 滞回：越界不足 `kTerrainWindowHysteresisBlocks` 不挪；越过后挪一格；相差 ≥ 2 格直接跳；带宽 0 = 无滞回。
TEST(TerrainTileResidency, HysteresisKeepsCenterUntilBoundaryIsCrossed) {
    const int center = 0;

    EXPECT_EQ(TerrainHysteresisCenterTile(center, 0, 10.0, kTerrainWindowHysteresisBlocks), 0) << "同 tile：不变";

    // 正方向：边界在 center+1 = 64 格。
    EXPECT_EQ(TerrainHysteresisCenterTile(center, 1, 70.0, kTerrainWindowHysteresisBlocks), 0)
        << "越界 6 格 < 16 ⇒ 保持不变";
    EXPECT_EQ(TerrainHysteresisCenterTile(center, 1, 64.0 + kTerrainWindowHysteresisBlocks, kTerrainWindowHysteresisBlocks), 1)
        << "越界 == 带宽 ⇒ 挪一格";

    // 负方向：边界在 center = 0 格。
    EXPECT_EQ(TerrainHysteresisCenterTile(center, -1, -10.0, kTerrainWindowHysteresisBlocks), 0);
    EXPECT_EQ(TerrainHysteresisCenterTile(center, -1, -kTerrainWindowHysteresisBlocks, kTerrainWindowHysteresisBlocks), -1);

    // 相差 ≥ 2（传送 / 远跳）：一次到底，不逐格挪。
    EXPECT_EQ(TerrainHysteresisCenterTile(center, 5, 320.0, kTerrainWindowHysteresisBlocks), 5);
    EXPECT_EQ(TerrainHysteresisCenterTile(center, -3, -200.0, kTerrainWindowHysteresisBlocks), -3);

    // 带宽 0 = 无滞回：越过边界即挪。
    EXPECT_EQ(TerrainHysteresisCenterTile(center, 1, 64.5, 0.0), 1);
}

// 目标集合：进入窗口的 tile 进 `toLoad`、离开窗口的进 `toUnload`；三个清单升序（确定序）。
TEST(TerrainTileResidency, PlanLoadsEnteringTilesAndUnloadsLeavingOnesAscending) {
    const std::vector<TileCoord> available = MakeAvailable(0, 2, 0, 2);
    const TerrainTileWindow      window    = TerrainWindowForPlayerBlocks(TileCenterBlocks(1), TileCenterBlocks(1), 0);
    ASSERT_EQ(window.centerTileX, 1);
    ASSERT_EQ(window.centerTileZ, 1);

    const std::vector<TileCoord> resident = { TileCoord { 0, 0 }, TileCoord { 0, 1 } };
    const auto isEdited = [](const TileCoord&) { return false; };

    const TerrainTileResidencyPlan plan = PlanTerrainTileResidency(window, available, resident, isEdited, 256U);

    ASSERT_EQ(plan.toLoad.size(), 1U);
    EXPECT_EQ(plan.toLoad[0], (TileCoord { 1, 1 }));

    ASSERT_EQ(plan.toUnload.size(), 2U);
    EXPECT_EQ(plan.toUnload[0], (TileCoord { 0, 0 }));
    EXPECT_EQ(plan.toUnload[1], (TileCoord { 0, 1 }));

    EXPECT_TRUE(plan.keptEdited.empty());
    EXPECT_TRUE(plan.evictedEdited.empty());

    EXPECT_TRUE(std::is_sorted(plan.toLoad.begin(), plan.toLoad.end()));
    EXPECT_TRUE(std::is_sorted(plan.toUnload.begin(), plan.toUnload.end()));
}

// 「世界内一致性」：**编辑过的 tile 离开窗口不得被卸**（ADR 0020 决策五口径）。
TEST(TerrainTileResidency, PlanKeepsEditedTilesResidentWhenTheyLeaveTheWindow) {
    const std::vector<TileCoord> available = MakeAvailable(0, 2, 0, 2);
    const TerrainTileWindow      window    = TerrainWindowForPlayerBlocks(TileCenterBlocks(1), TileCenterBlocks(1), 0);

    const std::vector<TileCoord> resident = { TileCoord { 0, 0 }, TileCoord { 0, 1 } };
    const auto isEdited = [](const TileCoord& coord) { return coord == (TileCoord { 0, 1 }); };

    const TerrainTileResidencyPlan plan = PlanTerrainTileResidency(window, available, resident, isEdited, 256U);

    ASSERT_EQ(plan.toUnload.size(), 1U);
    EXPECT_EQ(plan.toUnload[0], (TileCoord { 0, 0 })) << "未被编辑的照常卸";

    ASSERT_EQ(plan.keptEdited.size(), 1U);
    EXPECT_EQ(plan.keptEdited[0], (TileCoord { 0, 1 })) << "被编辑的必须常驻";
    EXPECT_TRUE(std::is_sorted(plan.keptEdited.begin(), plan.keptEdited.end()));
}

// 编辑块超上限 ⇒ 淘汰"最远者"（并留痕供调用方 WARN），最近者留下；淘汰清单升序。
TEST(TerrainTileResidency, PlanEvictsTheFarthestEditedTilesBeyondTheCap) {
    const std::vector<TileCoord> available = MakeAvailable(0, 3, 0, 0);
    const TerrainTileWindow      window    = TerrainWindowForPlayerBlocks(0.0, 0.0, 0);  // 中心 (0,0)、半径 0

    const std::vector<TileCoord> resident = {
        TileCoord { 0, 0 }, TileCoord { 1, 0 }, TileCoord { 2, 0 }, TileCoord { 3, 0 },
    };
    const auto isEdited = [](const TileCoord&) { return true; };

    const TerrainTileResidencyPlan plan = PlanTerrainTileResidency(window, available, resident, isEdited, 1U);

    ASSERT_EQ(plan.keptEdited.size(), 1U);
    EXPECT_EQ(plan.keptEdited[0], (TileCoord { 1, 0 })) << "最近的编辑块留下";

    ASSERT_EQ(plan.evictedEdited.size(), 2U);
    EXPECT_EQ(plan.evictedEdited[0], (TileCoord { 2, 0 }));
    EXPECT_EQ(plan.evictedEdited[1], (TileCoord { 3, 0 }));
    EXPECT_TRUE(std::is_sorted(plan.evictedEdited.begin(), plan.evictedEdited.end()));

    EXPECT_TRUE(plan.toUnload.empty()) << "编辑块不得走卸载路径";
    EXPECT_TRUE(plan.toLoad.empty());
}

// 确定性（红线 7）：输入顺序（resident / available 的排列）不同，结果**逐值相同**。
TEST(TerrainTileResidency, PlanIsDeterministicRegardlessOfInputOrder) {
    const std::vector<TileCoord> available  = MakeAvailable(-2, 2, -2, 2);
    std::vector<TileCoord>       availableB = available;
    std::reverse(availableB.begin(), availableB.end());

    const TerrainTileWindow window = TerrainWindowForPlayerBlocks(0.0, 0.0, 1);  // 中心 (0,0)、半径 1

    const std::vector<TileCoord> residentA = {
        TileCoord { 1, 1 }, TileCoord { -2, 0 }, TileCoord { 0, 0 }, TileCoord { 2, -2 },
    };
    std::vector<TileCoord> residentB = residentA;
    std::reverse(residentB.begin(), residentB.end());

    const auto isEdited = [](const TileCoord&) { return false; };

    const TerrainTileResidencyPlan planA = PlanTerrainTileResidency(window, available, residentA, isEdited, 256U);
    const TerrainTileResidencyPlan planB = PlanTerrainTileResidency(window, availableB, residentB, isEdited, 256U);

    EXPECT_TRUE(planA.toLoad == planB.toLoad);
    EXPECT_TRUE(planA.toUnload == planB.toUnload);
    EXPECT_TRUE(planA.keptEdited == planB.keptEdited);
    EXPECT_TRUE(planA.evictedEdited == planB.evictedEdited);
}

// 范围版（O(窗口)）与清单版语义一致，且 `desiredCount` = 窗口 ∩ 世界存在。
TEST(TerrainTileResidency, RangeVersionMatchesListVersionAndCountsDesired) {
    const TerrainTileWindow window = TerrainWindowForPlayerBlocks(0.0, 0.0, 2);  // 中心 (0,0)、半径 2
    const TerrainTileRange  range  = MakeTerrainTileRange(-1, -1, 3, 3);        // x, z ∈ [-1, 1]

    const std::vector<TileCoord> resident = { TileCoord { -1, -1 }, TileCoord { 5, 5 } };
    const auto                   isEdited = [](const TileCoord&) { return false; };

    const TerrainTileResidencyPlan byRange = PlanTerrainTileResidency(
        window, [&range](const TileCoord& coord) { return range.Contains(coord); }, resident, isEdited, 256U);

    std::vector<TileCoord> available;
    for (int x = -1; x <= 1; ++x) {
        for (int z = -1; z <= 1; ++z) {
            available.push_back(TileCoord { x, z });
        }
    }
    available.push_back(TileCoord { 5, 5 });  // 世界内存在但**不在窗口内**
    const TerrainTileResidencyPlan byList = PlanTerrainTileResidency(window, available, resident, isEdited, 256U);

    EXPECT_EQ(byRange.desiredCount, 9U);
    EXPECT_EQ(byList.desiredCount, 9U);
    EXPECT_TRUE(byRange.toLoad == byList.toLoad);
    EXPECT_TRUE(byRange.toUnload == byList.toUnload);
    ASSERT_EQ(byRange.toUnload.size(), 1U);
    EXPECT_EQ(byRange.toUnload[0], (TileCoord { 5, 5 })) << "离开窗口的必须卸";
}

// `TerrainWorld::UnloadTile`：高度与网格一并释放；`ResidentTiles` 升序；重复卸载返回 false。
TEST(TerrainTileResidency, WorldUnloadReleasesTileAndResidentListIsAscending) {
    TerrainWorld world(1234U, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);
    world.LoadTile(0, 1);
    EXPECT_EQ(world.ResidentTileCount(), 3U);

    const std::vector<TileCoord> resident = world.ResidentTiles();
    ASSERT_EQ(resident.size(), 3U);
    EXPECT_TRUE(std::is_sorted(resident.begin(), resident.end()));

    EXPECT_TRUE(world.UnloadTile(1, 0));
    EXPECT_EQ(world.ResidentTileCount(), 2U);
    EXPECT_FALSE(world.HasTile(1, 0));
    EXPECT_EQ(world.FindMesh(1, 0), nullptr) << "网格也必须一并释放";
    EXPECT_FALSE(world.UnloadTile(1, 0)) << "本就不常驻 ⇒ 返回 false（不静默当成功）";
}

// 调度器：进入窗口的 tile 被**分帧**加载；玩家换 tile 后离开窗口的被卸载；常驻量 = 窗口 ∩ 世界存在。
TEST(TerrainTileResidency, SchedulerLoadsWindowThenUnloadsTilesLeavingTheWindow) {
    TerrainWorld world(7U, TerrainMaterialTable::Default());
    // 世界 = tile [0,4] × [0,4]（5×5）；活动半径 1、预取 1 ⇒ 常驻半径 2；滞回 0（便于断言）。
    TerrainTileScheduler scheduler(MakeTerrainTileRange(0, 0, 5, 5), 1, 1, 0.0);

    // 玩家在 tile (2,2) 中心：常驻窗口 ±2 ⇒ 覆盖世界全部 25 个 tile。
    scheduler.Update(world, TileCenterBlocks(2), TileCenterBlocks(2));
    EXPECT_EQ(scheduler.ResidencyWindow().centerTileX, 2);
    EXPECT_EQ(scheduler.ResidencyWindow().radiusTiles, 2);
    EXPECT_EQ(scheduler.DesiredCount(), 25U);
    EXPECT_EQ(scheduler.PendingLoadCount(), 25U);
    EXPECT_EQ(world.ResidentTileCount(), 0U) << "Update 只出计划，不建（建活必须由 Step 分帧执行）";

    std::vector<TileCoord> changed;
    std::size_t            frames = 0;
    while (!scheduler.Step(world, 4U, changed)) {
        ASSERT_LT(++frames, 100U) << "分帧推进必须收敛";
    }
    EXPECT_EQ(world.ResidentTileCount(), 25U);
    EXPECT_EQ(world.ResidentTileCount(), scheduler.DesiredCount()) << "常驻量 = 目标集合";

    // 玩家走到 tile (0,0)：常驻窗口 ±2 ⇒ x, z ∈ [0,2] ⇒ 9 个；其余 16 个离开窗口。
    scheduler.Update(world, TileCenterBlocks(0), TileCenterBlocks(0));
    EXPECT_EQ(scheduler.DesiredCount(), 9U);
    EXPECT_EQ(scheduler.PendingLoadCount(), 0U) << "新窗口内的 tile 都还在常驻集合里";
    EXPECT_EQ(scheduler.PendingUnloadCount(), 16U);

    frames = 0;
    while (!scheduler.Step(world, 4U, changed)) {
        ASSERT_LT(++frames, 100U);
    }
    EXPECT_EQ(world.ResidentTileCount(), 9U);
    EXPECT_EQ(world.ResidentTileCount(), scheduler.DesiredCount());
    EXPECT_FALSE(scheduler.HasPendingWork());
}

// 幂等：中心 tile 未变时 `Update` 无操作（不得清空分帧推进中的待办）。
TEST(TerrainTileResidency, SchedulerUpdateIsIdempotentWhileCenterTileIsUnchanged) {
    TerrainWorld world(9U, TerrainMaterialTable::Default());
    TerrainTileScheduler scheduler(MakeTerrainTileRange(0, 0, 5, 5), 1, 1, kTerrainWindowHysteresisBlocks);

    scheduler.Update(world, TileCenterBlocks(2), TileCenterBlocks(2));
    const std::size_t pending = scheduler.PendingLoadCount();
    ASSERT_GT(pending, 0U);

    // 仅前进 4 格（远小于 16 格滞回带宽）⇒ 中心 tile 不变 ⇒ 待办**保持**。
    const bool hasWork = scheduler.Update(world, TileCenterBlocks(2) + 4.0, TileCenterBlocks(2));
    EXPECT_TRUE(hasWork);
    EXPECT_EQ(scheduler.PendingLoadCount(), pending) << "中心未变 ⇒ 不得重置待办";
    EXPECT_EQ(scheduler.ResidencyWindow().centerTileX, 2);
}

// 每帧动作数**必须**受 `maxActions` 约束（"所有重活都必须离开渲染帧"）；累计动作数 = 目标集合大小。
TEST(TerrainTileResidency, SchedulerRespectsPerFrameActionBudget) {
    TerrainWorld world(11U, TerrainMaterialTable::Default());
    TerrainTileScheduler scheduler(MakeTerrainTileRange(0, 0, 5, 5), 2, 0, 0.0);  // 常驻半径 2 ⇒ 5×5

    scheduler.Update(world, TileCenterBlocks(2), TileCenterBlocks(2));
    ASSERT_EQ(scheduler.DesiredCount(), 25U);

    std::vector<TileCoord> changed;
    std::size_t            total = 0;
    std::size_t            frames = 0;
    for (;;) {
        changed.clear();
        const bool done = scheduler.Step(world, 3U, changed);
        EXPECT_LE(changed.size(), 3U) << "单帧动作数不得超过预算";
        total += changed.size();
        if (done) {
            break;
        }
        ASSERT_LT(++frames, 100U);
    }
    EXPECT_EQ(total, 25U);
    EXPECT_EQ(world.ResidentTileCount(), 25U);
}

// LOD 分环纯函数：环边界 8/9/16/17/32 正确，且 > 32 钳制到最外环的 LOD（ADR 0024 决策二）。
TEST(TerrainTileResidency, TerrainLodLevelForTileDistanceHonorsRingBoundaries) {
    const TerrainLodRings rings;  // 默认 { 8, 16, 32 } / { 0, 1, 2 }

    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 0), 0);
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 8), 0) << "Ring 0 = 0..8";
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 9), 1) << "Ring 1 = 9..16";
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 16), 1);
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 17), 2) << "Ring 2 = 17..32";
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 32), 2);
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 33), 2) << "> 32 钳制到最外环的 LOD";
    EXPECT_EQ(TerrainLodLevelForTileDistance(rings, 100000), 2);
}

// CDLOD morph 参数：LOD0 / LOD1 的 step 与 start/end；最外环（LOD2）不启用 morph（step = 0）。
TEST(TerrainTileResidency, TerrainLodMorphRangeMatchesRingBoundaries) {
    const TerrainLodRings rings;  // 默认 { 8, 16, 32 } / { 0, 1, 2 }

    const TerrainLodMorphRange lod0 = TerrainLodMorphRangeForLevel(rings, 0);
    EXPECT_FLOAT_EQ(lod0.morphStep, 2.0F) << "TerrainLodSnapStep(0)";
    EXPECT_FLOAT_EQ(lod0.endDistance, 544.0F) << "(8 + 0.5) × 64";
    EXPECT_FLOAT_EQ(lod0.startDistance, 480.0F) << "544 − 64";

    const TerrainLodMorphRange lod1 = TerrainLodMorphRangeForLevel(rings, 1);
    EXPECT_FLOAT_EQ(lod1.morphStep, 4.0F) << "TerrainLodSnapStep(1)";
    EXPECT_FLOAT_EQ(lod1.endDistance, 1056.0F) << "(16 + 0.5) × 64";
    EXPECT_FLOAT_EQ(lod1.startDistance, 992.0F) << "1056 − 64";

    const TerrainLodMorphRange lod2 = TerrainLodMorphRangeForLevel(rings, 2);
    EXPECT_FLOAT_EQ(lod2.morphStep, 0.0F) << "最外环没有更粗的环可 morph";
}

// 分环调度器：真建 tile 后，每个常驻 tile 的 `world` 网格 `lodLevel` 必须等于调度器给出的目标 LOD。
//
// 用**紧凑环**（radii { 1, 2, 3 }）覆盖三环：与默认环（半径 32）同构，但不必真建 33 半径的世界
//（默认环需 65×65 的窗口，单测过慢 —— ADR 0024 的环口径已由上面的纯函数测试逐值钉住）。
TEST(TerrainTileResidency, RingedSchedulerMeshesTilesAtTheirRingLod) {
    TerrainWorld world(21U, TerrainMaterialTable::Default());
    const TerrainLodRings rings { { 1, 2, 3 }, { 0, 1, 2 } };
    // 世界 = x∈[-3,3]、z∈[-1,1]（7×3）；活动半径 = rings.radii[2] = 3、预取 0、滞回 0。
    TerrainTileScheduler scheduler(MakeTerrainTileRange(-3, -1, 7, 3), rings, 0, 0.0);
    ASSERT_TRUE(scheduler.HasLodRings());

    // 环归属（代表点）：距离 0/1 ⇒ LOD0、2 ⇒ LOD1、3 ⇒ LOD2。
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 0, 0 }), 0);
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 1, 0 }), 0);
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 2, 0 }), 1);
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 3, 0 }), 2);
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { -3, 1 }), 2);

    scheduler.Update(world, TileCenterBlocks(0), TileCenterBlocks(0));
    EXPECT_EQ(scheduler.DesiredCount(), 21U);
    EXPECT_EQ(scheduler.PendingRelodCount(), 0U) << "首次建集合：新加载的 tile 不进 relod 清单";

    std::vector<TileCoord> changed;
    std::size_t            frames = 0;
    while (!scheduler.Step(world, 8U, changed)) {
        ASSERT_LT(++frames, 100U) << "分帧推进必须收敛";
    }
    ASSERT_EQ(world.ResidentTileCount(), scheduler.DesiredCount());

    // 常驻集合里每个 tile 的网格 LOD == 调度器目标 LOD（加载时已按目标 LOD 建）。
    for (const TileCoord& coord : world.ResidentTiles()) {
        const vx::TerrainTileMesh* mesh = world.FindMesh(coord.x, coord.z);
        ASSERT_NE(mesh, nullptr) << coord.x << ", " << coord.z;
        EXPECT_EQ(mesh->lodLevel, scheduler.LodLevelForTile(coord)) << coord.x << ", " << coord.z;
    }
}

// 中心迁移后必须报出 relod：清单非空、`StepRelod` 分批取出、升序、预算生效，取完后归零。
TEST(TerrainTileResidency, SchedulerReportsRelodWhenCenterMoves) {
    TerrainWorld world(22U, TerrainMaterialTable::Default());
    const TerrainLodRings rings { { 1, 2, 3 }, { 0, 1, 2 } };
    TerrainTileScheduler scheduler(MakeTerrainTileRange(-3, -1, 7, 3), rings, 0, 0.0);

    scheduler.Update(world, TileCenterBlocks(0), TileCenterBlocks(0));
    std::vector<TileCoord> changed;
    std::size_t            frames = 0;
    while (!scheduler.Step(world, 8U, changed)) {
        ASSERT_LT(++frames, 100U);
    }
    ASSERT_EQ(scheduler.PendingRelodCount(), 0U);

    // 中心从 tile 0 挪到 tile 1（滞回 0 ⇒ 越过边界即挪）。tile (3,0) 由距离 3(LOD2) 变 2(LOD1) ⇒ 需 relod。
    scheduler.Update(world, TileCenterBlocks(1), TileCenterBlocks(0));
    ASSERT_EQ(scheduler.Window().centerTileX, 1);

    const std::size_t total = scheduler.PendingRelodCount();
    ASSERT_GT(total, 0U) << "中心迁移后必须报出 relod";

    std::vector<TileCoord> relod;
    std::vector<TileCoord> all;
    std::size_t            calls = 0;
    for (;;) {
        relod.clear();
        const bool done = scheduler.StepRelod(2U, relod);
        EXPECT_LE(relod.size(), 2U) << "单次 relod 数不得超过预算";
        all.insert(all.end(), relod.begin(), relod.end());
        if (done) {
            break;
        }
        ASSERT_LT(++calls, 1000U) << "分帧 relod 必须收敛";
    }
    EXPECT_EQ(all.size(), total);
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end())) << "relod 清单必须升序";
    EXPECT_EQ(scheduler.PendingRelodCount(), 0U);
}

// 单半径构造：`HasLodRings() == false`、任意位置 `PendingRelodCount() == 0`、LOD 恒为 0（既有语义不动）。
TEST(TerrainTileResidency, LegacyConstructorNeverReportsRelod) {
    TerrainWorld world(23U, TerrainMaterialTable::Default());
    TerrainTileScheduler scheduler(MakeTerrainTileRange(0, 0, 5, 5), 1, 1, 0.0);  // 旧单半径构造

    EXPECT_FALSE(scheduler.HasLodRings());
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 0, 0 }), 0);
    EXPECT_EQ(scheduler.LodLevelForTile(TileCoord { 5, 5 }), 0);

    scheduler.Update(world, TileCenterBlocks(2), TileCenterBlocks(2));
    EXPECT_EQ(scheduler.PendingRelodCount(), 0U);

    std::vector<TileCoord> changed;
    std::size_t            frames = 0;
    while (!scheduler.Step(world, 4U, changed)) {
        ASSERT_LT(++frames, 100U);
    }

    // 挪动中心后仍无 relod（旧语义：所有 tile 恒 LOD0，永不 relod）。
    scheduler.Update(world, TileCenterBlocks(0), TileCenterBlocks(0));
    EXPECT_EQ(scheduler.PendingRelodCount(), 0U);

    frames = 0;
    while (!scheduler.Step(world, 4U, changed)) {
        ASSERT_LT(++frames, 100U);
    }
    EXPECT_EQ(scheduler.PendingRelodCount(), 0U);
}

