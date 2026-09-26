// 可挖体积 → 物理碰撞体（T28 / ADR 0012）单元测试。
//
// 判据（阶段计划 T28）：
//   ① 有等值面的块能建出静态三角网碰撞体，句柄有效；
//   ② 同一块再次同步（挖除后）不崩溃，且仍然有碰撞体；
//   ③ 没有网格的块 / 不存在的块 ⇒ 不建碰撞体；
//   ④ 移除后 `HasBlock` 为假。

#include "core/clock.hpp"
#include "dig/dig_volume.hpp"
#include "dig/volume_collision.hpp"

#include "physics/physics_world.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::BlockCoord;
using vx::DigRegion;
using vx::DigRegionTable;
using vx::DigVolumeWorld;
using vx::MapEdit;
using vx::MapEditMode;
using vx::MapPreset;
using vx::PhysicsWorld;
using vx::TerrainMaterialTable;
using vx::TerrainWorld;
using vx::VolumeCollision;

constexpr int kFlatHeightBlocks = 120;

[[nodiscard]] DigRegion MakeRegion(BlockCoord minimum, BlockCoord maximum) {
    DigRegion region;
    region.name     = "collision_test";
    region.diggable = true;
    region.priority = 0;
    region.blockMin = minimum;
    region.blockMax = maximum;
    return region;
}

[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "volume collision test（整图压平）";
    preset.seed = 20260927;

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

}  // namespace

// 地表所在的块有等值面 ⇒ 能建出碰撞体；重建（挖除后）仍保有碰撞体。
TEST(VolumeCollision, SyncsSurfaceBlockAndRebuildsAfterCarve) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    // 地表 120 格 ⇒ y 块 3（覆盖 [96, 128)）含等值面。
    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const BlockCoord surface { 0, 3, 0 };
    ASSERT_NE(volumes.FindMesh(surface), nullptr);
    EXPECT_FALSE(volumes.FindMesh(surface)->indices.empty()) << "地表所在块必须能网格化（前置条件）";

    PhysicsWorld    physics;
    VolumeCollision collision(physics);

    EXPECT_TRUE(collision.SyncBlock(volumes, surface));
    EXPECT_TRUE(collision.HasBlock(surface));
    EXPECT_EQ(collision.BlockBodyCount(), 1U);

    // 挖一个洞后重网格，再同步：形状被重建，碰撞体仍在（否则会出现"看得见的新洞、走不进去"）。
    std::vector<BlockCoord> dirty;
    ASSERT_TRUE(volumes.CarveSphere(glm::dvec3(16.0, 120.0, 16.0), 4.0F, dirty));
    ASSERT_TRUE(volumes.RemeshDirtyBlocks(dirty) > 0U);
    EXPECT_TRUE(collision.SyncBlock(volumes, surface));
    EXPECT_TRUE(collision.HasBlock(surface));
    EXPECT_EQ(collision.BlockBodyCount(), 1U);

    collision.RemoveBlock(surface);
    EXPECT_FALSE(collision.HasBlock(surface));
    EXPECT_EQ(collision.BlockBodyCount(), 0U);
}

// 不存在的块 ⇒ 不建碰撞体；`SyncBlocks` 去重后返回最终拥有碰撞体的块数。
TEST(VolumeCollision, IgnoresMissingBlocksAndDeduplicates) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    PhysicsWorld    physics;
    VolumeCollision collision(physics);

    const BlockCoord missing { 9, 9, 9 };
    EXPECT_FALSE(collision.SyncBlock(volumes, missing));
    EXPECT_FALSE(collision.HasBlock(missing));

    const BlockCoord surface { 0, 3, 0 };
    const std::vector<BlockCoord> blocks { surface, surface, missing, surface };
    EXPECT_EQ(collision.SyncBlocks(volumes, blocks), 1U);
    EXPECT_EQ(collision.BlockBodyCount(), 1U);

    collision.RemoveAll();
    EXPECT_EQ(collision.BlockBodyCount(), 0U);
}

// ⑤ T30 基准：**单块三角网碰撞体的重建耗时**。
// Jolt 的 `MeshShape` 不可变 ⇒ 每次挖除 / 塌落都必须整块重建形状（Sanitize + BVH），
// 这是"爆炸后掉落帧"的另一个候选主因；只打印、不断言时间。
TEST(VolumeCollision, ProfileBlockShapeRebuild) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const BlockCoord surface { 0, 3, 0 };
    PhysicsWorld    physics;
    VolumeCollision collision(physics);
    ASSERT_TRUE(collision.SyncBlock(volumes, surface)) << "前置条件：地表块必须有等值面";

    const vx::MeshData* mesh = volumes.FindMesh(surface);
    ASSERT_NE(mesh, nullptr);

    constexpr int kRebuilds = 20;
    vx::Clock     clock;
    (void)clock.Tick();
    for (int i = 0; i < kRebuilds; ++i) {
        (void)collision.SyncBlock(volumes, surface);
    }
    const double totalMs = clock.Tick() * 1000.0;

    std::printf("[T30 基准] 单块三角网碰撞体重建：%zu 顶点 / %zu 三角形，%d 次共 %.2f ms ⇒ 每次 %.3f ms\n",
                mesh->vertices.size(), mesh->indices.size() / 3, kRebuilds, totalMs,
                totalMs / static_cast<double>(kRebuilds));
    std::fflush(stdout);
}
