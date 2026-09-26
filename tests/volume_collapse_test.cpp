// 破坏后的塌落（T29 / ADR 0012 第二节）单元测试。
//
// 判据（阶段计划 T29）：
//   ① **稳定面不塌**：由高度场初始化的平坦地表上，塌落不移动任何体素；
//   ② **失去支撑必塌**：悬空石板（正下方为空、且同层内够不到任何"支撑体素"）整体下落，
//      且下落**质量守恒**、落点堆在腔底；
//   ③ 摊开幅度不破坏质量守恒；
//   ④ 参数非法即抛异常（配置表校验）。

#include "core/clock.hpp"
#include "dig/collapse_table.hpp"
#include "dig/dig_volume.hpp"
#include "dig/volume_collapse.hpp"
#include "dig/volume_mesher.hpp"

#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::BlockCoord;
using vx::CollapseResult;
using vx::CollapseSeed;
using vx::CollapseSpec;
using vx::CollapseTable;
using vx::DensityRegion;
using vx::DigRegion;
using vx::DigRegionTable;
using vx::DigVolumeWorld;
using vx::MapEdit;
using vx::MapEditMode;
using vx::MapPreset;
using vx::TerrainMaterialTable;
using vx::TerrainWorld;

constexpr int kFlatHeightBlocks = 120;
/// 地表所在的块（块 y = 3 ⇒ 覆盖世界高度 [96, 128)，正好含 120 格平地）。
constexpr BlockCoord kSurfaceBlock { 0, 3, 0 };
/// "悬空石板"场景的块范围：**必须含更低的块 2**，否则"地板"接不到区域底面（= 判据里的地面）。
constexpr BlockCoord kSceneBlockMin { 0, 2, 0 };
constexpr BlockCoord kSceneBlockMax { 0, 3, 0 };
/// 场景覆盖的世界高度起点（= 块 2 的原点）：石板场景的采样范围是 [64, 128]。
constexpr int kSceneOriginY = 64;

[[nodiscard]] DigRegion MakeRegion(BlockCoord minimum, BlockCoord maximum) {
    DigRegion region;
    region.name     = "collapse_test";
    region.diggable = true;
    region.priority = 0;
    region.blockMin = minimum;
    region.blockMax = maximum;
    return region;
}

[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "volume collapse test（整图压平）";
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

/// "地板 + 空腔 + 悬空石板"的合成密度场（世界高度 [64, 128]，即两个块的采样范围）：
///   地板 = 高度 < 105 全实心（**自区域底面 64 起连续实心 ⇒ 接地**）；空腔 = 105..108；
///   石板 = 109..110、世界列 x/z ∈ [4, 12)；其上空。
[[nodiscard]] DensityRegion BuildFloatingSlabScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor = worldY < 105;
                const bool slab  = (worldY >= 109) && (worldY < 111) && (worldX >= 4) && (worldX < 12) &&
                                  (worldZ >= 4) && (worldZ < 12);
                region.values[region.Index(i, j, k)] =
                    (floor || slab) ? static_cast<std::int8_t>(vx::kDensityMin)
                                    : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

[[nodiscard]] std::size_t CountSolid(const DensityRegion& region) {
    std::size_t count = 0;
    for (const std::int8_t value : region.values) {
        if (value < 0) {
            ++count;
        }
    }
    return count;
}

/// 世界高度 `y` 上实心体素的个数（只统计测试块内的采样）。
[[nodiscard]] std::size_t CountSolidAtHeight(const DensityRegion& region, int worldY) {
    const int localY = worldY - region.minY;
    if (localY < 0 || localY >= region.sizeY) {
        return 0;
    }
    std::size_t count = 0;
    for (int k = 0; k < region.sizeZ; ++k) {
        for (int i = 0; i < region.sizeX; ++i) {
            if (region.values[region.Index(i, localY, k)] < 0) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

// ① 稳定面不塌：平坦地表（由高度场初始化）上跑塌落，不应移动任何体素。
TEST(VolumeCollapse, FlatGroundIsStable) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion(kSurfaceBlock, kSurfaceBlock) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const CollapseSpec   spec { true, 4.0F, 0, 0 };
    const CollapseResult result = vx::ApplyCollapse(volumes, CollapseSeed::FromBlocks(kSurfaceBlock, kSurfaceBlock), spec);
    EXPECT_EQ(result.movedVoxels, 0U) << "平地处处有支撑，不该有任何体素下落";
    EXPECT_TRUE(result.dirty.empty());
}

// ② 悬空石板必塌：整体落到腔底，质量守恒，且不再有悬空实心体。
TEST(VolumeCollapse, FloatingSlabFallsAndConservesMass) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const DensityRegion scene = BuildFloatingSlabScene();
    volumes.WriteDensityRegion(scene);

    const std::size_t slabVoxels = (CountSolidAtHeight(scene, 109) + CountSolidAtHeight(scene, 110));
    ASSERT_GT(slabVoxels, 0U) << "前置条件：场景里必须有悬空石板";

    const DensityRegion written =
        volumes.ReadDensityRegion(scene.minX, scene.minY, scene.minZ, scene.sizeX, scene.sizeY, scene.sizeZ);
    EXPECT_EQ(CountSolidAtHeight(written, 109), CountSolidAtHeight(scene, 109)) << "前置条件：写回必须生效";
    EXPECT_EQ(CountSolidAtHeight(written, 110), CountSolidAtHeight(scene, 110)) << "前置条件：写回必须生效";

    const CollapseSpec   spec { true, 4.0F, 0, 0 };
    const CollapseResult result = vx::ApplyCollapse(volumes, CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec);
    EXPECT_EQ(result.movedVoxels, slabVoxels) << "整块石板都失去支撑，应整体下落";
    EXPECT_FALSE(result.dirty.empty());

    const DensityRegion after = volumes.ReadDensityRegion(scene.minX, scene.minY, scene.minZ, scene.sizeX,
                                                          scene.sizeY, scene.sizeZ);
    EXPECT_EQ(CountSolid(after), CountSolid(scene)) << "下落必须**质量守恒**（碎块不凭空消失）";
    EXPECT_EQ(CountSolidAtHeight(after, 109), 0U) << "原高度不该再有实心体（它已落下）";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 0U);
    EXPECT_EQ(CountSolidAtHeight(after, 105) + CountSolidAtHeight(after, 106), slabVoxels)
        << "石板应堆在腔底（105 起往上）";
}

// ③ 摊开不破坏质量守恒（堆顶会被推给相邻列，但总数不变）。
TEST(VolumeCollapse, PileSpreadKeepsMass) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const DensityRegion scene = BuildFloatingSlabScene();
    volumes.WriteDensityRegion(scene);

    const CollapseSpec   spec { true, 4.0F, 2, 0 };
    const CollapseResult result = vx::ApplyCollapse(volumes, CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec);
    EXPECT_GE(result.movedVoxels, 1U);

    const DensityRegion after = volumes.ReadDensityRegion(scene.minX, scene.minY, scene.minZ, scene.sizeX,
                                                          scene.sizeY, scene.sizeZ);
    EXPECT_EQ(CountSolid(after), CountSolid(scene)) << "摊开只搬运、不消灭";
}

// ④ 关闭时不动数据。
TEST(VolumeCollapse, DisabledSpecMovesNothing) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const DensityRegion scene = BuildFloatingSlabScene();
    volumes.WriteDensityRegion(scene);

    const CollapseSpec   spec { false, 4.0F, 1, 0 };
    const CollapseResult result = vx::ApplyCollapse(volumes, CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec);
    EXPECT_EQ(result.movedVoxels, 0U);

    const DensityRegion after = volumes.ReadDensityRegion(scene.minX, scene.minY, scene.minZ, scene.sizeX,
                                                          scene.sizeY, scene.sizeZ);
    EXPECT_EQ(CountSolidAtHeight(after, 109), CountSolidAtHeight(scene, 109));
}

// ⑤ T30 基准：**真实游戏规模**下一次爆炸的 CPU 耗时分解。
//
// 规模取自 `assets/config/dig_regions.toml`：竖直 = 整个体积的块范围（块 y ∈ [0, 8] ⇒ 289 个采样），
// 水平 = 种子块 ± `neighborhood_margin_blocks` 1 块（3 块 ⇒ 97 个采样）⇒ 邻域 97 × 289 × 97 ≈ 271.9 万采样。
// 球心放在**地表以下 8 格**（= 从山体侧面射入的洞穴），是"挖了洞但往往不塌"的典型场景 ——
// 正是它决定了绝大多数爆炸的实际开销。只打印、不断言时间（避免 CI 上产生 flaky 断言）。
TEST(VolumeCollapse, ProfileGameScaleExplosion) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    for (int tileZ = -1; tileZ <= 0; ++tileZ) {
        for (int tileX = -1; tileX <= 0; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(BlockCoord { -2, 0, -2 }, BlockCoord { 0, 8, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const CollapseSpec spec { true, 4.0F, 1, 0 };

    vx::Clock clock;
    (void)clock.Tick();  // 构造后的首值无意义（计数器从 0 起步），丢弃
    const auto lapMs = [&clock]() { return clock.Tick() * 1000.0; };

    const glm::dvec3 centers[3] = { { -16.0, 112.0, -16.0 }, { -8.0, 112.0, -16.0 }, { -24.0, 112.0, -16.0 } };
    double           carveMs = 0.0;
    double           remeshMs = 0.0;
    double           collapseMs = 0.0;
    double           collapseRemeshMs = 0.0;
    std::size_t      regionSamples = 0;
    std::size_t      unsupported = 0;
    std::size_t      moved = 0;
    std::size_t      dirtyBlocks = 0;

    for (const glm::dvec3& center : centers) {
        std::vector<BlockCoord> dirty;
        vx::VoxelBounds         carvedBounds;
        const bool              carved = volumes.CarveSphere(center, 3.0F, dirty, &carvedBounds);
        carveMs += lapMs();
        ASSERT_TRUE(carved) << "前置条件：球心必须落在实心岩体里";

        (void)volumes.RemeshDirtyBlocks(dirty);
        remeshMs += lapMs();

        const CollapseResult collapse = vx::ApplyCollapse(volumes, CollapseSeed { carvedBounds }, spec);
        collapseMs += lapMs();

        (void)volumes.RemeshDirtyBlocks(collapse.dirty);
        collapseRemeshMs += lapMs();

        regionSamples = collapse.regionSamples;
        unsupported += collapse.unsupportedVoxels;
        moved += collapse.movedVoxels;
        dirtyBlocks += collapse.dirty.size();
    }

    std::printf("[T30 基准] 塌落邻域 %zu 采样 / 3 次爆炸平均：挖除 %.2f ms + 网格化 %.2f ms + 塌落 %.2f ms "
                "+ 塌落后网格化 %.2f ms = 合计 %.2f ms（3 次合计：失去支撑 %zu、移动 %zu、脏块 %zu）\n",
                regionSamples, carveMs / 3.0, remeshMs / 3.0, collapseMs / 3.0, collapseRemeshMs / 3.0,
                (carveMs + remeshMs + collapseMs + collapseRemeshMs) / 3.0, unsupported, moved, dirtyBlocks);
    std::fflush(stdout);
}

// 配置表：仓库内已提交的表必须能加载，且与 `Default()` 取值一致（防"配置与默认值漂移"）。
TEST(CollapseTable, LoadsShippedTableAndMatchesDefault) {
    const CollapseTable fromFile =
        CollapseTable::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/collapse.toml");
    EXPECT_EQ(fromFile.SchemaVersion(), CollapseTable::kSchemaVersion);
    EXPECT_TRUE(fromFile.Spec().enabled);
    EXPECT_FLOAT_EQ(fromFile.Spec().maxCantileverBlocks, 4.0F);
    EXPECT_EQ(fromFile.Spec().pileSpreadBlocks, 1);
    EXPECT_EQ(fromFile.Spec().neighborhoodMarginBlocks, 0);

    const CollapseTable defaults = CollapseTable::Default();
    EXPECT_EQ(defaults.Spec().enabled, fromFile.Spec().enabled);
    EXPECT_FLOAT_EQ(defaults.Spec().maxCantileverBlocks, fromFile.Spec().maxCantileverBlocks);
    EXPECT_EQ(defaults.Spec().pileSpreadBlocks, fromFile.Spec().pileSpreadBlocks);
    EXPECT_EQ(defaults.Spec().neighborhoodMarginBlocks, fromFile.Spec().neighborhoodMarginBlocks);

    EXPECT_THROW(static_cast<void>(CollapseTable::LoadFromFile("no_such_collapse.toml")), std::runtime_error);
}
