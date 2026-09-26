// 可挖区域标记表（ADR 0006 的数据文件部分）与可挖体积世界（T8）单元测试。
//
// 判据（阶段计划 T8）：
//   ① 区域包围盒**向外吸附到 32 格整数倍**，且包含判定与吸附结果一致；
//   ② 优先级 / sealed 覆盖语义、块数上限校验；
//   ③ SDF 初始化「地下为负 / 空中为正 / 地表处跨零」；
//   ④ 球体挖除后球心为空、球外采样逐值不变；
//   ⑤ 脏块重网格产出非空网格。

#include "dig/dig_region.hpp"
#include "dig/dig_volume.hpp"

#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
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
using vx::TerrainMaterialTable;
using vx::TerrainQuad;
using vx::TerrainWorld;

constexpr int kFlatHeightBlocks = 120;  ///< 测试地形：整张图压平到 120 格

[[nodiscard]] DigRegion MakeRegion(const char* name, bool diggable, int priority, BlockCoord minimum,
                                   BlockCoord maximum) {
    DigRegion region;
    region.name     = name;
    region.diggable = diggable;
    region.priority = priority;
    region.blockMin = minimum;
    region.blockMax = maximum;
    return region;
}

/// 平坦世界（整图压平到 `kFlatHeightBlocks`），供体积测试使用。
[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "dig volume test（整图压平）";
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

// 块包含判定：块的世界范围是**半开区间** `[blockOrigin, blockOrigin + 32)`。
TEST(DigRegion, BlockContainmentUsesHalfOpenWorldRange) {
    const DigRegionTable table = DigRegionTable::FromRegions(
        { MakeRegion("r", true, 0, BlockCoord { 0, 0, 0 }, BlockCoord { 0, 0, 0 }) });

    ASSERT_EQ(table.Blocks().size(), 1U);
    EXPECT_EQ(table.Blocks()[0], (BlockCoord { 0, 0, 0 }));
    EXPECT_TRUE(table.IsDiggable(0.0, 0.0, 0.0));
    EXPECT_TRUE(table.IsDiggable(31.9, 31.9, 31.9));
    EXPECT_FALSE(table.IsDiggable(32.0, 5.0, 5.0)) << "块的范围是半开区间 [0, 32)";
    EXPECT_FALSE(table.IsDiggable(-0.001, 5.0, 5.0));
}

// 空表：任何点都不可挖（ADR 0004 硬约束 2）。
TEST(DigRegion, EmptyTableIsNeverDiggable) {
    const DigRegionTable table = DigRegionTable::Default();
    EXPECT_TRUE(table.Empty());
    EXPECT_FALSE(table.IsDiggable(0.0, 0.0, 0.0));
    EXPECT_TRUE(table.Blocks().empty());
}

// 优先级：高优先级的 sealed 覆盖低优先级的 diggable（ADR 0006 的合并语义）。
TEST(DigRegion, SealedOverridesLowerPriorityDiggable) {
    const DigRegionTable table = DigRegionTable::FromRegions({
        MakeRegion("big", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 3, 3, 3 }),
        MakeRegion("sealed_inside", false, 10, BlockCoord { 1, 3, 1 }, BlockCoord { 1, 3, 1 }),
    });

    EXPECT_TRUE(table.IsDiggable(5.0, 100.0, 5.0)) << "大区域内的普通块可挖";
    EXPECT_FALSE(table.IsDiggable(40.0, 100.0, 40.0)) << "sealed 块必须不可挖（覆盖低优先级）";
    EXPECT_EQ(table.Blocks().size(), 4U * 1U * 4U - 1U) << "sealed 块不产生体积块（大区域共 4×1×4 = 16 块）";
}

// 地表四边形交接：四角**全部**可挖才跳过（ADR 0011）。
TEST(DigRegion, QuadIsSkippedOnlyWhenAllFourCornersAreDiggable) {
    const DigRegionTable table = DigRegionTable::FromRegions(
        { MakeRegion("r", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });

    TerrainQuad inside;
    for (int corner = 0; corner < 4; ++corner) {
        inside.columnX[corner] = 4 + corner;
        inside.columnZ[corner] = 5;
        inside.height[corner]  = 100.0F;
    }
    EXPECT_TRUE(table.SkipQuad(inside));

    TerrainQuad oneCornerOutside = inside;
    oneCornerOutside.columnX[2]  = 64;  // 落在块外
    EXPECT_FALSE(table.SkipQuad(oneCornerOutside)) << "只要有一角不在区域内，地表网格仍须绘制该四边形";

    TerrainQuad heightOutside = inside;
    heightOutside.height[3]   = 400.0F;  // 高度超出区域（y ∈ [96, 128)）
    EXPECT_FALSE(table.SkipQuad(heightOutside)) << "高度越界即视为不在区域内";
}

// 块数上限：超出即报错（防一份配置把内存 / 启动时间拉爆）。
TEST(DigRegion, RejectsTableExceedingBlockCap) {
    EXPECT_THROW(static_cast<void>(DigRegionTable::FromRegions(
                     { MakeRegion("huge", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 8, 9, 8 }) })),  // 9 × 7 × 9 = 567
                 std::runtime_error);
}

// 垂直范围越界即报错（世界高度范围 0 ~ 512 格）。
TEST(DigRegion, RejectsRegionOutsideWorldHeightRange) {
    EXPECT_THROW(static_cast<void>(DigRegionTable::FromRegions(
                     { MakeRegion("too_high", true, 0, BlockCoord { 0, 16, 0 }, BlockCoord { 0, 16, 0 }) })),
                 std::runtime_error);
}

// 文件加载：缺失不报错（ADR 0006）；仓库内已提交的表必须能加载，且其生效块范围与注释一致。
TEST(DigRegion, LoadsShippedTableAndToleratesMissingFile) {
    const DigRegionTable fromFile =
        DigRegionTable::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/dig_regions.toml");
    ASSERT_FALSE(fromFile.Empty());
    ASSERT_EQ(fromFile.Regions().size(), 1U);
    EXPECT_EQ(fromFile.Regions()[0].name, "test_world_all");
    EXPECT_TRUE(fromFile.Regions()[0].diggable);
    // 吸附：min = [-64, 0, -64]、max = [128, 287, 128] ⇒ 块 x ∈ [-2, 4]、y ∈ [0, 8]、z ∈ [-2, 4]
    EXPECT_EQ(fromFile.Regions()[0].blockMin, (BlockCoord { -2, 0, -2 }));
    EXPECT_EQ(fromFile.Regions()[0].blockMax, (BlockCoord { 4, 8, 4 }));
    // 块 7 × 9 × 7 = 441 个（整张测试地图的地表都可挖，含世界边缘的共享边界列 128）
    EXPECT_EQ(fromFile.Blocks().size(), 441U);

    const DigRegionTable missing = DigRegionTable::LoadFromFile(std::filesystem::path("no_such_dig_regions.toml"));
    EXPECT_TRUE(missing.Empty()) << "文件缺失必须返回空表且不抛异常（ADR 0006）";
}

// 体积初始化：地下为负、空中为正、地表处跨零；区域外回退到同一公式（与地表连续）。
TEST(DigVolume, InitialDensityFollowsHeightField) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });  // y ∈ [96, 128)
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    EXPECT_LT(volumes.SampleDensity(5.0, 110.0, 5.0), 0.0F) << "地表以下必须是实心（密度为负）";
    EXPECT_GT(volumes.SampleDensity(5.0, 127.0, 5.0), 0.0F) << "地表以上必须是空（密度为正）";
    EXPECT_NEAR(volumes.SampleDensity(5.0, 120.0, 5.0), 0.0F, 1.0F) << "地表处密度应跨零";

    // 区域外（x = 40 已超出块 [0, 32)）：回退到高度场推导，故同样是"120 以下实心"。
    EXPECT_LT(volumes.SampleDensity(40.0, 110.0, 5.0), 0.0F);
    EXPECT_GT(volumes.SampleDensity(40.0, 130.0, 5.0), 0.0F);
    EXPECT_FALSE(volumes.IsInsideRegion(40.0, 110.0, 5.0));
}

// 球体挖除：球内变空、球外逐值不变；脏块重网格产出非空网格。
TEST(DigVolume, CarveSphereEmptiesInsideAndKeepsOutside) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const float untouchedBefore = volumes.SampleDensity(25.0, 110.0, 25.0);
    ASSERT_LT(untouchedBefore, 0.0F) << "前置：该点原本是实心";

    std::vector<BlockCoord> dirty;
    const bool              changed = volumes.CarveSphere(glm::dvec3(8.0, 116.0, 8.0), 4.0F, dirty);
    EXPECT_TRUE(changed);
    ASSERT_EQ(dirty.size(), 1U);
    EXPECT_EQ(volumes.CarvedBlockCount(), 1U);

    EXPECT_FALSE(volumes.IsSolid(8.0, 116.0, 8.0)) << "球心必须被挖空";
    EXPECT_FALSE(volumes.IsSolid(8.0, 113.0, 8.0)) << "球内（半径内）必须被挖空";
    EXPECT_FLOAT_EQ(volumes.SampleDensity(25.0, 110.0, 25.0), untouchedBefore) << "球外采样必须逐值不变";

    EXPECT_EQ(volumes.RemeshDirtyBlocks(dirty), 1U);
    const vx::MeshData* mesh = volumes.FindMesh(dirty[0]);
    ASSERT_NE(mesh, nullptr);
    EXPECT_FALSE(mesh->vertices.empty()) << "挖出洞体后该块必须产生等值面";
    EXPECT_FALSE(mesh->indices.empty());
}

// 挖除的空气球：不产生任何改动（避免把"打进天空"当成一次爆炸破坏）。
// ADR 0014：可挖体积的材质 = 该列地表**主槽位**经「表层 → 次表层」映射后的结果。
// 平坦草地（高度 120、坡度 0 ⇒ 草）必须映射为**土** ⇒ 洞里看到的应是土、不是草（绿）。
// 人工实测第 7 轮："破坏后内部底部是绿色跟现实逻辑不符"。
TEST(DigVolume, MaterialSlotInheritsSurfaceThenMapsToSubsurface) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    // 槽位口径取自材质表（Default 表）：0 = 草、1 = 土、2 = 岩。草地 ⇒ 主槽位草 ⇒ 映射后是土。
    EXPECT_EQ(volumes.SampleMaterialSlot(5, 110, 5), 1U)
        << "草地上挖开看到的必须是土（草 → 土 的表层映射），否则洞里会呈绿色";
    EXPECT_EQ(volumes.SampleMaterialSlot(5, 200, 5), 1U) << "材质按**列**派生，与高度无关（体积内无深度分层）";
}

TEST(DigVolume, CarvingAirChangesNothing) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    std::vector<BlockCoord> dirty;
    EXPECT_FALSE(volumes.CarveSphere(glm::dvec3(16.0, 127.5, 16.0), 1.0F, dirty))
        << "球体全在空处时不应有改动";
    EXPECT_TRUE(dirty.empty());
}
