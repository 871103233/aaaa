// 可挖区域标记表（ADR 0006 的数据文件部分）与可挖体积世界（T8）单元测试。
//
// 判据（阶段计划 T8）：
//   ① 区域包围盒**向外吸附到 32 格整数倍**，且包含判定与吸附结果一致；
//   ② 优先级 / sealed 覆盖语义、块数上限校验；
//   ③ SDF 初始化「地下为负 / 空中为正 / 地表处跨零」；
//   ④ 球体挖除后球心为空、球外采样逐值不变；
//   ⑤ 脏块重网格产出非空网格。

#include "dig/destruction_table.hpp"
#include "dig/dig_region.hpp"
#include "dig/dig_volume.hpp"

#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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

// T42：**体素级材质持久化**（ADR 0014 的切换条件被触发）—— 写入的材质优先于"该列地表派生"，
// 未写入处仍逐位回落派生（零回归），且只有被写过的块才付出内存（懒分配 33³ / 块）。
// 这是"塌落残骸落地后颜色不变"的存储前提。
TEST(DigVolume, PersistedMaterialOverridesColumnDerivationAndIsLazy) {
    const MapPreset preset = FlatPreset();

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const std::size_t blockMaterialBytes = static_cast<std::size_t>(vx::kVolumeSampleCount) *
                                           static_cast<std::size_t>(vx::kVolumeSampleCount) *
                                           static_cast<std::size_t>(vx::kVolumeSampleCount);
    const std::uint8_t derived = volumes.SampleMaterialSlot(5, 110, 5);
    EXPECT_EQ(volumes.MaterialBytes(), 0U) << "从未写过材质 ⇒ **零内存**（与引入存储之前完全一致）";

    const std::uint8_t marked = static_cast<std::uint8_t>((static_cast<unsigned>(derived) + 1U) % 4U);
    ASSERT_NE(marked, derived) << "本用例要求标记材质与列派生不同，否则判据无意义";

    volumes.SetMaterialSlot(5, 110, 5, marked);
    EXPECT_EQ(volumes.SampleMaterialSlot(5, 110, 5), marked) << "已写入的体素材质优先";
    EXPECT_EQ(volumes.SampleMaterialSlot(5, 111, 5), derived) << "同列未写入的样本仍回落列派生";
    EXPECT_EQ(volumes.MaterialBytes(), blockMaterialBytes) << "只分配被写过的那个块（每块 33³ 字节）";

    volumes.SetMaterialSlot(-100, -100, -100, marked);  // 区域外（无块）⇒ 忽略，不分配
    EXPECT_EQ(volumes.MaterialBytes(), blockMaterialBytes);

    // 区域读：与 `DensityRegion` 同布局；已写入的优先、未写入的回落派生。
    const std::vector<std::uint8_t> column = volumes.ReadMaterialRegion(5, 110, 5, 1, 2, 1);
    ASSERT_EQ(column.size(), 2U);
    EXPECT_EQ(column[0], marked);
    EXPECT_EQ(column[1], derived);
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

// T36 / SKILL「不冻结画面」：**分步**初始化必须与一次性初始化**逐位一致** —— 分帧只允许改变
// "何时可见"，不得改变结果（红线 7）。这条是启动加载"每帧只走一步"的正确性凭据。
//
// 区域取 2×1×2 = 4 块（相邻块共享边界采样），因此不等价于"各块独立生成"：若分步实现让邻块
// 尚未填充密度时就去网格化，边界采样会回落到未取整的高度场推导值，本用例会立刻失败。
TEST(DigVolume, SteppedInitMatchesOneShotInitExactly) {
    const MapPreset preset = FlatPreset();

    TerrainWorld worldOneShot(preset.seed, TerrainMaterialTable::Default());
    worldOneShot.SetMapPreset(preset);
    worldOneShot.LoadTile(0, 0);

    TerrainWorld worldStepped(preset.seed, TerrainMaterialTable::Default());
    worldStepped.SetMapPreset(preset);
    worldStepped.LoadTile(0, 0);

    const DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion("blocks", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 1, 3, 1 }) });

    DigVolumeWorld oneShot(worldOneShot, regions);
    oneShot.InitFromHeightField();

    DigVolumeWorld stepped(worldStepped, regions);
    stepped.BeginInitFromHeightField();
    EXPECT_EQ(stepped.InitCompletedSteps(), 0U);
    EXPECT_EQ(stepped.InitTotalSteps(), regions.Blocks().size() * 2U)
        << "步骤总数 = 2 × 块数（先逐块填密度、再逐块网格化）";

    // 每批只走一步（最细分帧），期间进度必须单调前进且未完成时不到 100%。
    std::size_t previous = stepped.InitCompletedSteps();
    while (!stepped.StepInitFromHeightField(1)) {
        EXPECT_GT(stepped.InitCompletedSteps(), previous);
        EXPECT_LT(stepped.InitCompletedSteps(), stepped.InitTotalSteps());
        previous = stepped.InitCompletedSteps();
    }
    EXPECT_EQ(stepped.InitCompletedSteps(), stepped.InitTotalSteps());

    ASSERT_EQ(stepped.Blocks().size(), oneShot.Blocks().size());
    for (const auto& entry : oneShot.Blocks()) {
        const auto found = stepped.Blocks().find(entry.first);
        ASSERT_NE(found, stepped.Blocks().end()) << "分步初始化缺少块";
        EXPECT_EQ(found->second.density, entry.second.density) << "密度数组必须逐位相同";
        EXPECT_EQ(found->second.fill, entry.second.fill);
        EXPECT_EQ(found->second.carved, entry.second.carved);
        EXPECT_EQ(found->second.mesh.indices, entry.second.mesh.indices);

        ASSERT_EQ(found->second.mesh.vertices.size(), entry.second.mesh.vertices.size());
        for (std::size_t i = 0; i < entry.second.mesh.vertices.size(); ++i) {
            const vx::MeshVertex& expected = entry.second.mesh.vertices[i];
            const vx::MeshVertex& actual   = found->second.mesh.vertices[i];
            EXPECT_FLOAT_EQ(actual.position[0], expected.position[0]);
            EXPECT_FLOAT_EQ(actual.position[1], expected.position[1]);
            EXPECT_FLOAT_EQ(actual.position[2], expected.position[2]);
            EXPECT_FLOAT_EQ(actual.normal[0], expected.normal[0]);
            EXPECT_FLOAT_EQ(actual.normal[1], expected.normal[1]);
            EXPECT_FLOAT_EQ(actual.normal[2], expected.normal[2]);
            EXPECT_FLOAT_EQ(actual.material, expected.material);
        }
    }
}

// ============================================================================================
// T31 / [ADR 0013](../../docs/adr/0013-destructible-elements.md)：材质坚固度 × 伤害预算的**逐格³** 挖除
// ============================================================================================
namespace {

/// 造一个"整图压平到 120 格 + 单块可挖区域（y ∈ [96, 128)）"的体积世界。
struct FlatVolume {
    TerrainWorld    world;
    DigRegionTable  regions;
    DigVolumeWorld  volumes;

    explicit FlatVolume(const TerrainMaterialTable& materials = TerrainMaterialTable::Default())
        : world(FlatPreset().seed, materials),
          regions(DigRegionTable::FromRegions(
              { MakeRegion("block", true, 0, BlockCoord { 0, 3, 0 }, BlockCoord { 0, 3, 0 }) })),
          volumes(world, regions) {
        const MapPreset preset = FlatPreset();
        world.SetMapPreset(preset);
        world.LoadTile(0, 0);
        volumes.InitFromHeightField();
    }
};

/// 把中心附近一个立方盒内的**采样**显式打上材质槽位（`SampleMaterialSlot` 走"已写入优先"路径）。
void MarkBox(DigVolumeWorld& volumes, int minX, int minY, int minZ, int maxX, int maxY, int maxZ,
             std::uint8_t slot) {
    for (int z = minZ; z <= maxZ; ++z) {
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                volumes.SetMaterialSlot(x, y, z, slot);
            }
        }
    }
}

/// 该点沿 +X 方向"第一个仍是实心的采样"的距离（= 腔体半径的可判定度量）。
[[nodiscard]] int FirstSolidDistanceAlongX(const DigVolumeWorld& volumes, const glm::dvec3& center) {
    for (int distance = 1; distance <= 12; ++distance) {
        if (volumes.IsSolid(center.x + static_cast<double>(distance), center.y, center.z)) {
            return distance;
        }
    }
    return 13;  // 12 格内全是空（超出本用例的量程）
}

/// 与上者同口径，但沿 **−X**（T53 的"墙前近侧"就是 −X 侧）。
[[nodiscard]] int FirstSolidDistanceAlongNegativeX(const DigVolumeWorld& volumes, const glm::dvec3& center) {
    for (int distance = 1; distance <= 12; ++distance) {
        if (volumes.IsSolid(center.x - static_cast<double>(distance), center.y, center.z)) {
            return distance;
        }
    }
    return 13;
}

/// 该块网格里"绕序反转"（正面朝向实体侧）的三角形数 —— 几何法线与三个顶点法线（密度梯度，指向**空侧**）
/// 之均值的点积 < 0。T55 的探针判据：这样的面会被**背面剔除**，屏幕表现与"面消失 / 透明"完全一致。
[[nodiscard]] std::size_t CountBackwardTriangles(const vx::MeshData& mesh) {
    std::size_t backward = 0;
    const auto  position = [&mesh](std::uint32_t index) {
        const vx::MeshVertex& vertex = mesh.vertices[index];
        return glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
    };
    const auto normal = [&mesh](std::uint32_t index) {
        const vx::MeshVertex& vertex = mesh.vertices[index];
        return glm::vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
    };
    for (std::size_t triangle = 0; triangle + 2U < mesh.indices.size(); triangle += 3U) {
        const std::uint32_t ia = mesh.indices[triangle];
        const std::uint32_t ib = mesh.indices[triangle + 1U];
        const std::uint32_t ic = mesh.indices[triangle + 2U];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) {
            continue;
        }
        const glm::vec3 face = glm::cross(position(ib) - position(ia), position(ic) - position(ia));
        if (glm::dot(face, face) <= 1.0e-12F) {
            continue;  // 退化：由 `CountDegenerateTriangles` 负责
        }
        if (glm::dot(face, normal(ia) + normal(ib) + normal(ic)) < 0.0F) {
            ++backward;
        }
    }
    return backward;
}

}  // namespace

// 判据 1（ADR 0013 §二.4 的**手感锚点**）：伤害 10 × 换算系数 271 = 2710 点；纯泥土坚固度 3
// ⇒ 破坏 ≈ 903 格³ ⇒ 腔体 r ≈ 6 格（允许 ±1 格离散误差）。
// 默认材质表下，压平地形的地下材质由"该列表层 → 次表层"派生为**土**（地表是草 ⇒ dirt）。
TEST(DigVolume, CarveByDamageMatchesDirtAnchor) {
    FlatVolume scene;

    const glm::dvec3 center(16.0, 112.0, 16.0);  // 8 格深于地表（120）⇒ 半径 6 的候选球全在地下
    ASSERT_TRUE(scene.volumes.IsSolid(center.x, center.y, center.z)) << "前置：球心原本是实心";
    ASSERT_EQ(scene.volumes.SampleMaterialSlot(16, 112, 16), 1U) << "前置：地下派生材质应为土（槽位 1）";

    std::vector<BlockCoord> dirty;
    const int               budget = static_cast<int>(std::lround(10.0 * 271.0));  // damage × points_per_cubic_block
    ASSERT_TRUE(scene.volumes.CarveByDamage(center, 6.0F, budget, dirty));

    EXPECT_FALSE(scene.volumes.IsSolid(center.x, center.y, center.z)) << "球心必须被挖空";
    const int radius = FirstSolidDistanceAlongX(scene.volumes, center);
    EXPECT_GE(radius, 5) << "腔体半径应 ≈ 6 格（实测 " << radius << "）；不得明显偏小";
    EXPECT_LE(radius, 7) << "腔体半径应 ≈ 6 格（实测 " << radius << "）；不得明显偏大";
}

// 判据 2（**T52 修订，2026-09-28**）：**岩 = 完全不可破坏** —— 同一预算下**零改动**、不标脏任何块
//（口径来自 `materials.toml` 的 `indestructible = true`；项目所有者指定"岩石无法击毁、无法挖洞"）。
// **对照**：同一场景把该处标成**土**（可破坏）⇒ 立刻被挖出 r ≈ 6 的腔体
//（证明"零改动"是**材质**造成的，而不是爆炸参数 / 采样出的别的问题）。
TEST(DigVolume, CarveByDamageCannotTouchRockAnymore) {
    FlatVolume scene;
    MarkBox(scene.volumes, 4, 100, 4, 28, 127, 28, 2U);  // 岩（槽位 2）
    ASSERT_EQ(scene.volumes.SampleMaterialSlot(16, 112, 16), 2U) << "前置：该处材质应为岩";
    ASSERT_TRUE(scene.volumes.Materials().Layer(2).indestructible)
        << "前提：岩在材质表里是 indestructible（T52）";

    const glm::dvec3        center(16.0, 112.0, 16.0);
    std::vector<BlockCoord> dirty;
    const int               budget = static_cast<int>(std::lround(10.0 * 271.0));
    EXPECT_FALSE(scene.volumes.CarveByDamage(center, 6.0F, budget, dirty)) << "岩体必须**零改动**（无法挖洞）";
    EXPECT_TRUE(dirty.empty()) << "零改动 ⇒ 不得标脏任何块";
    EXPECT_TRUE(scene.volumes.IsSolid(center.x, center.y, center.z)) << "岩体不得被挖除";
    EXPECT_EQ(FirstSolidDistanceAlongX(scene.volumes, center), 1) << "球心邻域仍是实心";

    // 对照：同一场景、同一预算，只把材质换成**土**（可破坏）⇒ 正常挖出 r ≈ 6 的腔体。
    FlatVolume              control;
    MarkBox(control.volumes, 4, 100, 4, 28, 127, 28, 1U);
    ASSERT_EQ(control.volumes.SampleMaterialSlot(16, 112, 16), 1U) << "对照：该处材质应为土";
    std::vector<BlockCoord> controlDirty;
    EXPECT_TRUE(control.volumes.CarveByDamage(center, 6.0F, budget, controlDirty)) << "对照：土必须被挖开";
    const int controlRadius = FirstSolidDistanceAlongX(control.volumes, center);
    EXPECT_GE(controlRadius, 5) << "对照腔体半径应 ≈ 6（实测 " << controlRadius << "）";
    EXPECT_LE(controlRadius, 7) << "对照腔体半径应 ≈ 6（实测 " << controlRadius << "）";
}

// ============================================================================================
// T53（所有者 2026-09-28 指定）：**岩石要遮挡爆炸波，保护岩石后的东西不被破坏**
//
// 契约：爆炸波是**标量扩散**，不能穿过**不可破坏材质（岩）** ⇒ 岩后（含整片被围住的空腔）**零改动**，
// 且被遮挡的格**不消耗预算**。判据 = 同一场景下"有岩墙 / 把岩墙换成土"的对照。
// ============================================================================================

// 判据 ①②：岩墙之后的土腔**零改动**；对照（岩墙换成土）被正常挖通。
TEST(DigVolume, CarveByDamageIsBlockedByRockWall) {
    // 场景：整片先标成**土**，再在 `x ∈ [18, 19]` 立一道 y / z 都贯穿掩码范围的**岩墙**（不可破坏）。
    // 爆心放在墙前（`x = 12.5`）、半径 10 ⇒ 若没有遮挡，球面会把墙后的土一并挖掉（改前就是这样）。
    FlatVolume wall;
    MarkBox(wall.volumes, 0, 96, 0, 31, 127, 31, 1U);
    MarkBox(wall.volumes, 18, 96, 0, 19, 127, 31, 2U);

    const glm::dvec3 center(12.5, 110.5, 16.5);
    ASSERT_EQ(wall.volumes.SampleMaterialSlot(18, 110, 16), 2U) << "前置：墙处材质应为岩";
    ASSERT_TRUE(wall.volumes.Materials().Layer(2).indestructible) << "前提：岩不可破坏（T52）";
    ASSERT_TRUE(wall.volumes.IsSolid(22.0, 110.0, 16.0)) << "前置：墙后的土原本是实心";

    std::vector<BlockCoord> dirty;
    ASSERT_TRUE(wall.volumes.CarveByDamage(center, 10.0F, 40000, dirty));

    EXPECT_TRUE(wall.volumes.IsSolid(22.0, 110.0, 16.0))
        << "岩墙**之后**的土必须零改动（爆炸波被岩遮挡，T53）";
    EXPECT_TRUE(wall.volumes.IsSolid(21.0, 110.0, 16.0)) << "紧贴墙后也必须是零改动";
    EXPECT_FALSE(wall.volumes.IsSolid(10.0, 110.0, 16.0)) << "墙**前**近侧仍必须被正常挖开";

    // 对照：同一场景、同一预算，只把"岩墙"那段换成**土** ⇒ 墙后照旧被挖通。
    // （否则"零改动"可能是预算不足 / 参数不对造成的，判据不成立。）
    FlatVolume control;
    MarkBox(control.volumes, 0, 96, 0, 31, 127, 31, 1U);
    std::vector<BlockCoord> controlDirty;
    ASSERT_TRUE(control.volumes.CarveByDamage(center, 10.0F, 40000, controlDirty));
    EXPECT_FALSE(control.volumes.IsSolid(22.0, 110.0, 16.0))
        << "对照：没有岩墙时，墙后的土必须被挖掉（证明零改动是**遮挡**造成的）";
}

// 判据 ③：被遮挡的格**不消耗预算** —— 墙前近侧的腔体必须与"没有墙"时**同样大**。
TEST(DigVolume, CarveByDamageOccludedCellsDoNotConsumeBudget) {
    FlatVolume wall;
    MarkBox(wall.volumes, 0, 96, 0, 31, 127, 31, 1U);
    MarkBox(wall.volumes, 18, 96, 0, 19, 127, 31, 2U);

    FlatVolume open;
    MarkBox(open.volumes, 0, 96, 0, 31, 127, 31, 1U);

    const glm::dvec3        center(12.5, 110.5, 16.5);
    std::vector<BlockCoord> dirtyLeft;
    std::vector<BlockCoord> dirtyRight;
    ASSERT_TRUE(wall.volumes.CarveByDamage(center, 10.0F, 40000, dirtyLeft));
    ASSERT_TRUE(open.volumes.CarveByDamage(center, 10.0F, 40000, dirtyRight));

    const glm::dvec3 probe(12.0, 110.0, 16.0);
    EXPECT_EQ(FirstSolidDistanceAlongNegativeX(wall.volumes, probe),
              FirstSolidDistanceAlongNegativeX(open.volumes, probe))
        << "墙前近侧的腔体大小必须一致 ⇒ 被遮挡的格没有烧掉预算";
}

// 判据 ⑤：**立方岩壳**把爆炸完全围住 ⇒ 只挖壳体内部、壳外零改动；
// 且**掩码切断处**（"按掩码栅格化"在掩码边界留下的硬台阶）不得产生退化 / 绕序反转的三角形 ——
// 那正是"某个面透明"的高危形状（T55 的两条判据在这里复用）。
TEST(DigVolume, CarveByDamageInsideRockShellCarvesOnlyTheInterior) {
    FlatVolume scene;
    MarkBox(scene.volumes, 0, 96, 0, 31, 127, 31, 1U);  // 整片 = 土

    const glm::dvec3 center(16.0, 108.0, 16.0);
    constexpr int    kShell = 5;  // `max(|dx|, |dy|, |dz|) == kShell` 的一层岩 = 1 格厚的立方壳（6 邻域不可穿）
    for (int z = -kShell; z <= kShell; ++z) {
        for (int y = -kShell; y <= kShell; ++y) {
            for (int x = -kShell; x <= kShell; ++x) {
                const int chebyshev = std::max(std::abs(x), std::max(std::abs(y), std::abs(z)));
                if (chebyshev != kShell) {
                    continue;
                }
                scene.volumes.SetMaterialSlot(static_cast<int>(center.x) + x, static_cast<int>(center.y) + y,
                                              static_cast<int>(center.z) + z, 2U);
            }
        }
    }
    ASSERT_EQ(scene.volumes.SampleMaterialSlot(21, 108, 16), 2U) << "前置：壳壁应为岩";

    std::vector<BlockCoord> dirty;
    // 半径 8 ⇒ 球面（含过渡带）远大于壳体 ⇒ 若不按掩码栅格化，球面会把壳外的土一并挖掉。
    ASSERT_TRUE(scene.volumes.CarveByDamage(center, 8.0F, 40000, dirty));

    EXPECT_FALSE(scene.volumes.IsSolid(16.0, 108.0, 16.0)) << "壳内必须被挖空";
    EXPECT_FALSE(scene.volumes.IsSolid(16.0, 105.0, 16.0)) << "壳内（贴壳）也必须被挖空";
    EXPECT_TRUE(scene.volumes.IsSolid(16.0, 100.0, 16.0)) << "壳**外**（正下方）必须零改动";
    EXPECT_TRUE(scene.volumes.IsSolid(16.0, 108.0, 25.0)) << "壳**外**（侧向）必须零改动";

    // 网格质量：掩码切断处不得出现画不出来的面（退化）或朝里的面（背面剔除 ⇒ 看着透明）。
    (void)scene.volumes.RemeshDirtyBlocks(dirty);
    std::size_t degenerate = 0;
    std::size_t backward   = 0;
    for (const BlockCoord& coord : dirty) {
        const vx::MeshData* mesh = scene.volumes.FindMesh(coord);
        if (mesh == nullptr) {
            continue;
        }
        degenerate += vx::CountDegenerateTriangles(*mesh);
        backward += CountBackwardTriangles(*mesh);
    }
    EXPECT_EQ(degenerate, 0U) << "掩码切断处不得出现零面积三角形";
    EXPECT_EQ(backward, 0U) << "掩码切断处不得出现绕序反转（会被背面剔除 ⇒ 看着透明）的三角形";
}

// 判据 ④：**爆心落在岩体内部 ⇒ 波不外泄** —— 连岩块之外的土也必须零改动。
TEST(DigVolume, CarveByDamageEpicenterInsideRockLeaksNothing) {
    FlatVolume scene;
    MarkBox(scene.volumes, 0, 96, 0, 31, 127, 31, 1U);     // 整片 = 土
    MarkBox(scene.volumes, 10, 105, 10, 14, 114, 20, 2U);  // 中央一块岩，爆心在其中

    const glm::dvec3 center(12.0, 110.0, 15.0);
    ASSERT_EQ(scene.volumes.SampleMaterialSlot(12, 110, 15), 2U) << "前置：爆心处材质应为岩";

    std::vector<BlockCoord> dirty;
    EXPECT_FALSE(scene.volumes.CarveByDamage(center, 10.0F, 40000, dirty))
        << "岩体内爆炸 ⇒ 洪泛一格都进不去 ⇒ 无改动是正确结果";
    EXPECT_TRUE(dirty.empty()) << "零改动 ⇒ 不得标脏任何块";
    EXPECT_TRUE(scene.volumes.IsSolid(16.0, 110.0, 15.0)) << "岩块之外的土不得被挖（波不外泄）";
}

// 判据 4：**不可破坏材质**（`indestructible = true`）⇒ 爆炸落在其中**零改动**、且不标脏。
// T52 之后岩默认就是不可破坏（见判据 2），故这里改用**临时材质表**把**土**改成不可破坏 ——
// 走的是"显式开关"这条路（与 `toughness = 0` 这个数值写法语义等价，见 materials.toml 表头）。
TEST(DigVolume, CarveByDamageSkipsIndestructibleMaterial) {
    const std::filesystem::path shipped = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/materials.toml";
    std::ifstream input(shipped);
    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();
    // 定位**土层**块（`name = "dirt"`）里的 `indestructible` 行，把它翻成 true
    // （整份表里只有土层那一行属于土层；草 / 沙的同名行在更前面，故必须从土层头开始找）。
    const std::string dirtHeader = "name         = \"dirt\"";
    const std::size_t headerPos  = text.find(dirtHeader);
    ASSERT_NE(headerPos, std::string::npos) << "前置：仓库材质表里应能定位「土」层";
    const std::string flagLine = "indestructible = false";
    const std::size_t flagPos  = text.find(flagLine, headerPos);
    ASSERT_NE(flagPos, std::string::npos) << "前置：土层里应有 indestructible 行";
    // 等长替换（22 字符）⇒ 不改动其余排版。
    text.replace(flagPos, flagLine.size(), "indestructible = true ");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vx_t31_indestructible.toml";
    {
        std::ofstream out(path);
        out << text;
    }

    const TerrainMaterialTable materials = TerrainMaterialTable::LoadFromFile(path);
    FlatVolume                 scene(materials);
    ASSERT_TRUE(scene.volumes.Materials().Layer(1).indestructible) << "前置：临时表里「土」已置为不可破坏";
    MarkBox(scene.volumes, 4, 100, 4, 28, 127, 28, 1U);  // 全标成"不可破坏的土"

    const glm::dvec3        center(16.0, 112.0, 16.0);
    std::vector<BlockCoord> dirty;
    EXPECT_FALSE(scene.volumes.CarveByDamage(center, 6.0F, 2710, dirty)) << "不可破坏材质里必须零改动";
    EXPECT_TRUE(dirty.empty());
    EXPECT_TRUE(scene.volumes.IsSolid(16.0, 112.0, 16.0)) << "不可破坏材质不得被挖除";

    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // 对照：仓库原表（土**可**破坏）在同一场景、同一预算 ⇒ 立刻被挖开。
    FlatVolume              control;
    MarkBox(control.volumes, 4, 100, 4, 28, 127, 28, 1U);
    std::vector<BlockCoord> controlDirty;
    EXPECT_TRUE(control.volumes.CarveByDamage(center, 6.0F, 2710, controlDirty)) << "对照：可破坏的土必须被挖开";
    EXPECT_FALSE(controlDirty.empty());
}

// 判据 5：确定性 —— 同输入两次 ⇒ 逐体素相同（含同距格的次序，红线 7）。
TEST(DigVolume, CarveByDamageIsDeterministic) {
    FlatVolume first;
    FlatVolume second;

    const glm::dvec3        center(16.0, 112.0, 16.0);
    std::vector<BlockCoord> dirtyLeft;
    std::vector<BlockCoord> dirtyRight;
    ASSERT_TRUE(first.volumes.CarveByDamage(center, 6.0F, 2710, dirtyLeft));
    ASSERT_TRUE(second.volumes.CarveByDamage(center, 6.0F, 2710, dirtyRight));
    EXPECT_EQ(dirtyLeft, dirtyRight) << "脏块列表必须逐项相同";

    const vx::DensityRegion left  = first.volumes.ReadDensityRegion(0, 100, 0, 33, 29, 33);
    const vx::DensityRegion right = second.volumes.ReadDensityRegion(0, 100, 0, 33, 29, 33);
    EXPECT_EQ(left.values, right.values) << "密度必须逐位相同";
}

// T31：破坏表（destruction.toml）——仓库文件可加载、与内置默认一致；非法配置抛异常（禁止静默回退）。
TEST(DestructionTable, LoadsShippedTableAndMatchesDefault) {
    const vx::DestructionTable fromFile =
        vx::DestructionTable::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/destruction.toml");
    const vx::DestructionTable defaults = vx::DestructionTable::Default();

    EXPECT_EQ(fromFile.SchemaVersion(), vx::DestructionTable::kSchemaVersion);
    EXPECT_FLOAT_EQ(fromFile.Spec().pointsPerCubicBlock, defaults.Spec().pointsPerCubicBlock);
    EXPECT_FLOAT_EQ(fromFile.Spec().propDamageThreshold, defaults.Spec().propDamageThreshold);
    EXPECT_FLOAT_EQ(fromFile.Spec().pointsPerCubicBlock, 271.0F) << "手感锚点：271（ADR 0013 §二.4）";

    EXPECT_THROW(static_cast<void>(
                     vx::DestructionTable::LoadFromFile(std::filesystem::path("no_such_destruction.toml"))),
                 std::runtime_error);
}

TEST(DestructionTable, RejectsInvalidConfig) {
    const auto writeTemp = [](const std::string& content) {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "vx_t31_destruction.toml";
        std::ofstream               out(path);
        out << content;
        return path;
    };
    const std::string tail = "\nprop_damage_threshold = 0.0\nprop_broken_tint = [0.05, 0.05, 0.05]\n";

    // 换算系数必须 > 0。
    EXPECT_THROW(static_cast<void>(vx::DestructionTable::LoadFromFile(
                     writeTemp("schema_version = 1\npoints_per_cubic_block = 0.0" + tail))),
                 std::runtime_error);
    // 缺字段。
    EXPECT_THROW(static_cast<void>(vx::DestructionTable::LoadFromFile(
                     writeTemp("schema_version = 1\nprop_damage_threshold = 0.0\nprop_broken_tint = [0, 0, 0]\n"))),
                 std::runtime_error);
    // 版本不匹配。
    EXPECT_THROW(static_cast<void>(vx::DestructionTable::LoadFromFile(
                     writeTemp("schema_version = 2\npoints_per_cubic_block = 271.0" + tail))),
                 std::runtime_error);
    // 表现色越界。
    EXPECT_THROW(static_cast<void>(vx::DestructionTable::LoadFromFile(writeTemp(
                     "schema_version = 1\npoints_per_cubic_block = 271.0\nprop_damage_threshold = 0.0\n"
                     "prop_broken_tint = [1.5, 0.0, 0.0]\n"))),
                 std::runtime_error);
}

// ============================================================================================
// T55 探针（**临时诊断**，定案后改成回归判据）：用**真实挖除路径**复现"爆炸处偶发透明面"，
// 把现象钉到可测量的量上（观测先于结论）。
//
// 两条判据都能表现成屏幕上的"透明 / 没染色"：
//   ① **退化三角形**（零面积）⇒ 光栅化不产生片元 ⇒ 那一片"看得见后面"；
//   ② **绕序反转三角形**（正面朝向实体侧）⇒ 被**背面剔除** ⇒ 效果与"面消失"完全一致
//      （片元着色器恒写 `alpha = 1`，整条管线没有混合透明 ⇒ "透明"只可能是"没有片元"）。
//
// 与 `volume_mesher_test.cpp` 里纯解析球面探针的区别：这里走 `CarveByDamage` 的三条真实路径
// （**CSG 取 max + 过渡带 + 不可破坏材质跳过**），并**按块重网格**后逐块统计 —— 覆盖多种球心 / 半径 /
// 预算，因为用户报的是"**有概率**"，即形状相关的偶发。
// ============================================================================================
TEST(DigVolumeProbe, CarvedBlocksReportDegenerateAndBackwardTriangles) {
    struct Config {
        const char* name;
        glm::dvec3  center;
        float       radius;
        int         budget;
        int         rockSlabMinX;  ///< `>= 0` 时在 `x ∈ [rockSlabMinX, rockSlabMinX + 3]` 放一层**岩板**
                                   ///< （不可破坏）⇒ 走 `RasterizeBall` 的"跳过不可破坏采样"这条路径
    };
    const Config configs[] = {
        { "整数心 / r=6 / 全挖", glm::dvec3(16.0, 112.0, 16.0), 6.0F, 2710, -1 },
        { "半格心 / r=6 / 全挖", glm::dvec3(16.5, 112.5, 16.5), 6.0F, 2710, -1 },
        { "偏移心 / r=7.25", glm::dvec3(15.25, 111.75, 16.75), 7.25F, 5000, -1 },
        { "偏移心 / r=10 / 足预算", glm::dvec3(16.75, 112.5, 15.5), 10.0F, 20000, -1 },
        { "偏移心 / r=10 / 半预算", glm::dvec3(16.25, 111.5, 16.25), 10.0F, 9000, -1 },
        { "贴块边界 / r=8", glm::dvec3(6.25, 112.75, 7.5), 8.0F, 12000, -1 },
        { "近地表 / r=6", glm::dvec3(16.5, 119.25, 16.5), 6.0F, 8000, -1 },
        { "岩板遮挡 / x=19", glm::dvec3(12.5, 112.25, 16.5), 10.0F, 20000, 19 },
    };

    for (const Config& config : configs) {
        FlatVolume scene;
        // 该场景地下派生材质 = 土（可破坏）。先把候选范围整片显式标成**土**（走"已写入优先"路径），
        // 再按需放一层岩板 —— 岩板在材质表里不可破坏，会让栅格化走"跳过不可破坏采样"的分支。
        MarkBox(scene.volumes, 0, 100, 0, 31, 127, 31, 1U);
        if (config.rockSlabMinX >= 0) {
            MarkBox(scene.volumes, config.rockSlabMinX, 100, 0, config.rockSlabMinX + 3, 127, 31, 2U);
        }

        std::vector<BlockCoord> dirty;
        const bool changed = scene.volumes.CarveByDamage(config.center, config.radius, config.budget, dirty);
        (void)scene.volumes.RemeshDirtyBlocks(dirty);

        std::size_t degenerate = 0;
        std::size_t backward   = 0;
        std::size_t triangles  = 0;
        for (const BlockCoord& coord : dirty) {
            const vx::MeshData* mesh = scene.volumes.FindMesh(coord);
            if (mesh == nullptr) {
                continue;
            }
            triangles += mesh->indices.size() / 3U;
            degenerate += vx::CountDegenerateTriangles(*mesh);
            backward += CountBackwardTriangles(*mesh);
        }
        std::printf("[T55 探针·真实路径] %-22s 改动=%s 脏块 %2zu 三角 %5zu **退化 %3zu 绕序反转 %3zu**\n",
                    config.name, changed ? "是" : "否", dirty.size(), triangles, degenerate, backward);
    }
    std::fflush(stdout);
}

