// V0.10 / S3：**体积块存档差量**（`.voxr` 的 `VolumeDirtyBlock`）单测
// （[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策二 / 三 / 四）。
//
// 判据：① 载荷覆盖该块**拥有的** 32³ 采样（每块只导出自己那一格 ⇒ 每个世界采样**恰好一次**）；
// ② 读回后**逐字节一致**（导出 → 读回 → 再导出，两次载荷相同；网格逐顶点相同）；
// ③ 无材质时 `materialPresent = 0`（懒分配语义不变）；④ 非法载荷即抛 / 未常驻不猜。

#include "dig/dig_region.hpp"
#include "dig/dig_volume.hpp"

#include "render/mesh_renderer.hpp"  // `MeshData` / `MeshVertex`（逐顶点比对用）
#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
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
using vx::TerrainWorld;
using vx::VolumeDirtyPayload;

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

/// 平坦世界（整图压平到 `kFlatHeightBlocks`），与 `dig_volume_test.cpp` 同口径。
[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "dig volume save test（整图压平）";
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

/// 造一个「平坦地形 + 指定可挖块集合」的体积世界。
///
/// **前置条件**：`regions` 的生命周期必须覆盖返回的 `DigVolumeWorld`（后者只持引用 —— 见其构造函数）
/// ⇒ 调用方必须先落一个**具名**区域表，不能把临时表直接传进来（否则是悬垂引用）。
[[nodiscard]] std::unique_ptr<DigVolumeWorld> MakeVolumes(const MapPreset& preset, TerrainWorld& world,
                                                          const DigRegionTable& regions) {
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);
    auto volumes = std::make_unique<DigVolumeWorld>(world, regions);
    volumes->InitFromHeightField();
    return volumes;
}

/// 该块网格顶点缓冲的**原始字节**（逐顶点比对；不依赖 `MeshVertex` 是否提供 `operator==`）。
[[nodiscard]] std::vector<std::uint8_t> MeshVertexBytes(const DigVolumeWorld& volumes, const BlockCoord& coord) {
    const vx::MeshData* mesh = volumes.FindMesh(coord);
    if (mesh == nullptr || mesh->vertices.empty()) {
        return {};
    }
    const auto* begin = reinterpret_cast<const std::uint8_t*>(mesh->vertices.data());
    return std::vector<std::uint8_t>(begin, begin + mesh->vertices.size() * sizeof(vx::MeshVertex));
}

}  // namespace

TEST(DigVolumeSave, ExportsOwnedSamplesAndKeepsMaterialLazy) {
    const MapPreset  preset = FlatPreset();
    const BlockCoord coord { 0, 3, 0 };
    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion("block", coord, coord) });

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    const auto   volumes = MakeVolumes(preset, world, regions);

    VolumeDirtyPayload payload;
    ASSERT_TRUE(volumes->ExportBlockSave(coord, payload));
    EXPECT_EQ(payload.density.size(), vx::kVolumeSaveVoxelCount) << "只导出该块**拥有的** 32³ 采样";
    EXPECT_FALSE(payload.materialPresent) << "懒分配：从未写过材质 ⇒ materialPresent = 0";
    EXPECT_TRUE(payload.material.empty());

    // 写过一个体素材质 ⇒ 载荷带上材质（该块自此走「已写入优先」路径）。
    volumes->SetMaterialSlot(5, 110, 5, 3U);
    ASSERT_TRUE(volumes->ExportBlockSave(coord, payload));
    EXPECT_TRUE(payload.materialPresent);
    ASSERT_EQ(payload.material.size(), vx::kVolumeSaveVoxelCount);

    // 未常驻（区域外的块）⇒ 不给假数据。
    VolumeDirtyPayload missing;
    EXPECT_FALSE(volumes->ExportBlockSave(BlockCoord { 9, 9, 9 }, missing));
}

TEST(DigVolumeSave, RoundTripsDensityAndMeshByteForByte) {
    const MapPreset  preset = FlatPreset();
    const BlockCoord coord { 0, 3, 0 };
    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion("block", coord, coord) });

    VolumeDirtyPayload        payload;
    std::vector<std::uint8_t> sourceMesh;
    {
        // ---- 世界 A：挖洞 → 导出载荷 + 记住网格 ----
        TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
        const auto   volumes = MakeVolumes(preset, world, regions);

        std::vector<BlockCoord> dirty;
        ASSERT_TRUE(volumes->CarveSphere(glm::dvec3(8.0, 116.0, 8.0), 4.0F, dirty));
        ASSERT_FALSE(dirty.empty());
        (void)volumes->RemeshDirtyBlocks(dirty);

        ASSERT_TRUE(volumes->IsBlockDirty(coord));
        ASSERT_TRUE(volumes->ExportBlockSave(coord, payload));
        sourceMesh = MeshVertexBytes(*volumes, coord);
        ASSERT_FALSE(sourceMesh.empty());
    }

    // ---- 世界 B：同种子重建 → **生成之后**叠加载荷 ⇒ 密度与网格都要与 A 一致 ----
    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    const auto   volumes = MakeVolumes(preset, world, regions);

    ASSERT_TRUE(volumes->ApplyBlockSave(coord, payload));
    ASSERT_TRUE(volumes->RemeshBlock(coord));
    EXPECT_TRUE(volumes->IsBlockDirty(coord));

    VolumeDirtyPayload reExported;
    ASSERT_TRUE(volumes->ExportBlockSave(coord, reExported));
    EXPECT_EQ(reExported.density, payload.density) << "导出 → 读回 → 再导出必须相同";
    EXPECT_EQ(reExported.materialPresent, payload.materialPresent);
    EXPECT_EQ(MeshVertexBytes(*volumes, coord), sourceMesh) << "共享边界层一并恢复 ⇒ 网格逐顶点一致";
}

TEST(DigVolumeSave, RestoresAcrossAdjacentBlocksAndKeepsSeamConsistent) {
    const MapPreset  preset = FlatPreset();
    const BlockCoord left { 0, 3, 0 };
    const BlockCoord right { 1, 3, 0 };  // 世界 x ∈ [32, 64)：与 left 共享 x = 32 那一层采样
    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion("both", left, right) });

    VolumeDirtyPayload        leftPayload;
    VolumeDirtyPayload        rightPayload;
    std::vector<std::uint8_t> sourceLeft;
    std::vector<std::uint8_t> sourceRight;
    {
        TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
        const auto   volumes = MakeVolumes(preset, world, regions);

        // 球心落在 x = 32（两块交界）⇒ 一次挖除**同时**改两块，两侧的共享层都被写。
        std::vector<BlockCoord> dirty;
        ASSERT_TRUE(volumes->CarveSphere(glm::dvec3(32.0, 116.0, 8.0), 6.0F, dirty));
        ASSERT_GE(dirty.size(), 2U) << "应同时改动左右两块";
        (void)volumes->RemeshDirtyBlocks(dirty);

        ASSERT_TRUE(volumes->ExportBlockSave(left, leftPayload));
        ASSERT_TRUE(volumes->ExportBlockSave(right, rightPayload));
        sourceLeft  = MeshVertexBytes(*volumes, left);
        sourceRight = MeshVertexBytes(*volumes, right);
        ASSERT_FALSE(sourceLeft.empty());
        ASSERT_FALSE(sourceRight.empty());
    }

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    const auto   volumes = MakeVolumes(preset, world, regions);

    // **先恢复 right、再恢复 left**：结果必须与「谁先常驻」无关（由共享边界层重算保证）。
    ASSERT_TRUE(volumes->ApplyBlockSave(right, rightPayload));
    ASSERT_TRUE(volumes->ApplyBlockSave(left, leftPayload));
    ASSERT_TRUE(volumes->RemeshBlock(left));
    ASSERT_TRUE(volumes->RemeshBlock(right));

    EXPECT_EQ(MeshVertexBytes(*volumes, left), sourceLeft) << "交界层错一格的话，这里就会不等";
    EXPECT_EQ(MeshVertexBytes(*volumes, right), sourceRight);
}

TEST(DigVolumeSave, UnchangedPayloadReportsNoChange) {
    const MapPreset  preset = FlatPreset();
    const BlockCoord coord { 0, 3, 0 };
    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion("block", coord, coord) });

    VolumeDirtyPayload payload;
    {
        TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
        const auto   volumes = MakeVolumes(preset, world, regions);
        ASSERT_TRUE(volumes->ExportBlockSave(coord, payload));  // 未挖过 ⇒ 载荷 == 生成结果
    }

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    const auto   volumes = MakeVolumes(preset, world, regions);
    EXPECT_FALSE(volumes->ApplyBlockSave(coord, payload))
        << "载荷与生成结果一致 ⇒ 必须报告「无改动」（调用方由此省掉一次重网格）";
}

TEST(DigVolumeSave, RejectsMalformedPayloadAndNonResidentApply) {
    const MapPreset  preset = FlatPreset();
    const BlockCoord coord { 0, 3, 0 };
    const DigRegionTable regions = DigRegionTable::FromRegions({ MakeRegion("block", coord, coord) });

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    const auto   volumes = MakeVolumes(preset, world, regions);

    VolumeDirtyPayload shortPayload;
    shortPayload.density.assign(16U, 0);  // 长度不对
    EXPECT_THROW((void)volumes->ApplyBlockSave(coord, shortPayload), std::invalid_argument);

    VolumeDirtyPayload badMaterial;
    badMaterial.density.assign(vx::kVolumeSaveVoxelCount, 0);
    badMaterial.materialPresent = true;
    badMaterial.material.assign(16U, vx::kNoMaterialSlot);
    EXPECT_THROW((void)volumes->ApplyBlockSave(coord, badMaterial), std::invalid_argument);

    // 未常驻的块：叠加 false（等它被建出来时再叠加 —— ADR 0037 决策三）。
    VolumeDirtyPayload valid;
    valid.density.assign(vx::kVolumeSaveVoxelCount, 0);
    EXPECT_FALSE(volumes->ApplyBlockSave(BlockCoord { 5, 5, 5 }, valid));
}
