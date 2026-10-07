// 可编辑层保存单测（V0.5 E3；ADR 0032 决策六）：写 TOML → `LoadOverlayFromFile` 读回**逐字段一致**、
// 保留 `[[scatter]]`、`schema_version` 版本纪律（未知版本拒绝）。见 docs/plans/v0.5.md §1.19。

#include "object/object_edit_save.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using vx::ObjectAssetKind;
using vx::ObjectPlacement;
using vx::ObjectRemoval;
using vx::ObjectScatter;
using vx::ObjectTable;
using vx::ObjectType;
using vx::SaveObjectEditLayer;

/// 唯一临时文件路径（析构删除；同名文件由调用方保证不冲突）。
class TempPath {
public:
    explicit TempPath(std::string name) : path_(std::filesystem::temp_directory_path() / std::move(name)) {}
    ~TempPath() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + ".tmp", error);
    }
    TempPath(const TempPath&)            = delete;
    TempPath& operator=(const TempPath&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// 发布清单（只读）：一个程序化类型 + 一个模型类型，供编辑层引用 / 删除。
ObjectTable MakeBase() {
    ObjectTable base;
    base.destructibleEnabled = true;

    ObjectType dirt;
    dirt.id           = "dirt_pile";
    dirt.kind         = ObjectAssetKind::DirtPile;
    dirt.halfExtentX  = 1.0F;
    dirt.halfExtentY  = 0.75F;
    dirt.halfExtentZ  = 1.0F;
    dirt.destructible = true;
    dirt.category     = "prop";
    base.types.push_back(dirt);

    ObjectPlacement published;
    published.typeId = "dirt_pile";
    published.x      = 1.0F;
    published.z      = 1.0F;
    base.placements.push_back(published);
    return base;
}

/// 编辑层增量：新增类型 + 新增落点 + 删除项 + 保留的散布。
ObjectTable MakeEditLayer() {
    ObjectTable layer;
    layer.destructibleEnabled = true;

    ObjectType bush;
    bush.id           = "bush_x";
    bush.kind         = ObjectAssetKind::Stone;
    bush.halfExtentX  = 0.5F;
    bush.halfExtentY  = 0.5F;
    bush.halfExtentZ  = 0.5F;
    bush.destructible = false;
    bush.category     = "vegetation";
    layer.types.push_back(bush);

    ObjectPlacement added;
    added.typeId     = "bush_x";
    added.x          = 5.0F;
    added.y          = 0.0F;
    added.z          = 6.0F;
    added.yawDegrees = 30.0F;
    layer.placements.push_back(added);

    ObjectRemoval removal;
    removal.typeId    = "dirt_pile";
    removal.x         = 1.0F;
    removal.z         = 1.0F;
    removal.tolerance = 0.75F;
    layer.removals.push_back(removal);

    ObjectScatter scatter;
    scatter.typeId  = "bush_x";
    scatter.centerX = 0.0F;
    scatter.centerZ = 0.0F;
    scatter.radius  = 10.0F;
    scatter.count   = 3;
    scatter.seed    = 7U;
    layer.scatters.push_back(scatter);

    return layer;
}

TEST(ObjectEditSave, RoundTripsTypePlacementRemovalAndScatter) {
    const TempPath  file("voxel_edit_save_roundtrip.toml");
    const ObjectTable base  = MakeBase();
    const ObjectTable layer = MakeEditLayer();

    SaveObjectEditLayer(file.path(), layer);
    ASSERT_TRUE(std::filesystem::exists(file.path()));

    const ObjectTable readBack = ObjectTable::LoadOverlayFromFile(file.path(), base);

    // 类型（本层新增）
    ASSERT_EQ(readBack.types.size(), 1U);
    EXPECT_EQ(readBack.types[0].id, "bush_x");
    EXPECT_EQ(readBack.types[0].kind, ObjectAssetKind::Stone);
    EXPECT_EQ(readBack.types[0].category, "vegetation");
    EXPECT_FALSE(readBack.types[0].destructible);

    // 落点
    ASSERT_EQ(readBack.placements.size(), 1U);
    EXPECT_EQ(readBack.placements[0].typeId, "bush_x");
    EXPECT_FLOAT_EQ(readBack.placements[0].x, 5.0F);
    EXPECT_FLOAT_EQ(readBack.placements[0].z, 6.0F);
    EXPECT_FLOAT_EQ(readBack.placements[0].yawDegrees, 30.0F);

    // 删除项
    ASSERT_EQ(readBack.removals.size(), 1U);
    EXPECT_EQ(readBack.removals[0].typeId, "dirt_pile");
    EXPECT_FLOAT_EQ(readBack.removals[0].x, 1.0F);
    EXPECT_FLOAT_EQ(readBack.removals[0].z, 1.0F);
    EXPECT_FLOAT_EQ(readBack.removals[0].tolerance, 0.75F);

    // 散布（保留原样）
    ASSERT_EQ(readBack.scatters.size(), 1U);
    EXPECT_EQ(readBack.scatters[0].typeId, "bush_x");
    EXPECT_FLOAT_EQ(readBack.scatters[0].radius, 10.0F);
    EXPECT_EQ(readBack.scatters[0].count, 3);
    EXPECT_EQ(readBack.scatters[0].seed, 7U);
}

TEST(ObjectEditSave, AppliesRemovalToBasePlacementsOnMerge) {
    const TempPath  file("voxel_edit_save_merge_removal.toml");
    const ObjectTable base  = MakeBase();
    const ObjectTable layer = MakeEditLayer();

    SaveObjectEditLayer(file.path(), layer);
    const ObjectTable overlay = ObjectTable::LoadOverlayFromFile(file.path(), base);
    const ObjectTable merged  = vx::MergeObjectTables(base, overlay);

    // 发布清单里的 `dirt_pile`（1,1）被 `[[remove]]` 剔除 ⇒ 只剩本层新增的 `bush_x`。
    ASSERT_EQ(merged.placements.size(), 1U);
    EXPECT_EQ(merged.placements[0].typeId, "bush_x");
}

TEST(ObjectEditSave, OverwritesExistingFileAtomically) {
    const TempPath  file("voxel_edit_save_overwrite.toml");
    const ObjectTable base = MakeBase();

    ObjectTable first = MakeEditLayer();
    SaveObjectEditLayer(file.path(), first);

    ObjectTable second;
    second.destructibleEnabled = true;  // 空增量：只写开关（无类型 / 落点 / 删除 / 散布）
    SaveObjectEditLayer(file.path(), second);

    const ObjectTable readBack = ObjectTable::LoadOverlayFromFile(file.path(), base);
    EXPECT_TRUE(readBack.types.empty());
    EXPECT_TRUE(readBack.placements.empty());
    EXPECT_TRUE(readBack.removals.empty());
    EXPECT_FALSE(std::filesystem::exists(file.path().string() + ".tmp"));  // 原子替换后不留临时文件
}

TEST(ObjectEditSave, RejectsUnknownSchemaVersion) {
    const TempPath file("voxel_edit_save_bad_version.toml");
    {
        std::ofstream out(file.path(), std::ios::binary | std::ios::trunc);
        out << "schema_version = 999\ndestructible_enabled = true\n";
    }
    const ObjectTable base = MakeBase();
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(file.path(), base)), std::runtime_error);
}

// --------------------------- V0.9：成套建筑 / 按 id 删除 / 变暗覆盖（ADR 0036 决策三~四）--------------------------

using vx::ComputeBuildingEnclosure;
using vx::MergeObjectTables;
using vx::ObjectBuilding;
using vx::ObjectBuildingDarkening;
using vx::ObjectBuildingLandingMode;
using vx::ObjectBuildingPiece;
using vx::ObjectBuildingRemoval;
using vx::ObjectKitRole;

/// 发布清单：两个 kit 类型 + 两座建筑（`hut` / `hall`）—— 供编辑层引用 / 删除 / 覆盖。
ObjectTable MakeKitBase() {
    ObjectTable base;
    base.destructibleEnabled = true;

    ObjectType floor;
    floor.id           = "kit_floor";
    floor.kind         = ObjectAssetKind::Kit;
    floor.kitRole      = ObjectKitRole::Floor;
    floor.moduleBlocks = 4.0F;
    floor.halfExtentX  = 2.0F;
    floor.halfExtentY  = 0.15F;
    floor.halfExtentZ  = 2.0F;
    floor.destructible = false;
    base.types.push_back(floor);

    ObjectType roof = floor;
    roof.id             = "kit_roof";
    roof.kitRole        = ObjectKitRole::Roof;
    base.types.push_back(roof);

    for (const char* id : { "hut", "hall" }) {
        ObjectBuilding building;
        building.id = id;
        building.x  = 3.0F;
        building.z  = 4.0F;
        ObjectBuildingPiece piece;
        piece.typeId = "kit_floor";
        building.pieces.push_back(piece);
        ObjectBuildingPiece roofPiece;
        roofPiece.typeId  = "kit_roof";
        roofPiece.offsetY = 3.3F;
        building.pieces.push_back(roofPiece);
        base.buildings.push_back(std::move(building));
    }
    return base;
}

/// 编辑层增量：新增一座建筑（`tower`）+ 删除 `hut` + 覆盖 `hall` 的变暗。
ObjectTable MakeBuildingEditLayer() {
    ObjectTable layer;
    layer.destructibleEnabled = true;

    ObjectBuilding tower;
    tower.id                 = "tower";
    tower.x                  = 10.0F;
    tower.z                  = 20.0F;
    tower.yawDegrees         = 45.0F;
    tower.interiorDarkening  = 0.30F;
    tower.landingMode        = ObjectBuildingLandingMode::Sink;
    ObjectBuildingPiece floor;
    floor.typeId = "kit_floor";
    tower.pieces.push_back(floor);
    ObjectBuildingPiece roof;
    roof.typeId     = "kit_roof";
    roof.offsetY    = 3.3F;
    roof.yawDegrees = 90.0F;
    tower.pieces.push_back(roof);
    layer.buildings.push_back(std::move(tower));

    ObjectBuildingRemoval removal;
    removal.buildingId = "hut";
    layer.buildingRemovals.push_back(std::move(removal));

    ObjectBuildingDarkening overrideEntry;
    overrideEntry.buildingId = "hall";
    overrideEntry.darkening  = 0.20F;
    layer.buildingDarkenings.push_back(std::move(overrideEntry));

    return layer;
}

TEST(ObjectEditSave, RoundTripsBuildingsRemovalsAndDarkening) {
    const TempPath file("voxel_edit_save_buildings.toml");
    const ObjectTable base  = MakeKitBase();
    const ObjectTable layer = MakeBuildingEditLayer();

    SaveObjectEditLayer(file.path(), layer);
    const ObjectTable readBack = ObjectTable::LoadOverlayFromFile(file.path(), base);

    // 建筑（本层新增）：逐字段一致。
    ASSERT_EQ(readBack.buildings.size(), 1U);
    EXPECT_EQ(readBack.buildings[0].id, "tower");
    EXPECT_FLOAT_EQ(readBack.buildings[0].x, 10.0F);
    EXPECT_FLOAT_EQ(readBack.buildings[0].z, 20.0F);
    EXPECT_FLOAT_EQ(readBack.buildings[0].yawDegrees, 45.0F);
    EXPECT_FLOAT_EQ(readBack.buildings[0].interiorDarkening, 0.30F);
    EXPECT_EQ(readBack.buildings[0].landingMode, ObjectBuildingLandingMode::Sink);
    ASSERT_EQ(readBack.buildings[0].pieces.size(), 2U);
    EXPECT_EQ(readBack.buildings[0].pieces[0].typeId, "kit_floor");
    EXPECT_FLOAT_EQ(readBack.buildings[0].pieces[1].offsetY, 3.3F);
    EXPECT_FLOAT_EQ(readBack.buildings[0].pieces[1].yawDegrees, 90.0F);

    // 删除项 / 变暗覆盖。
    ASSERT_EQ(readBack.buildingRemovals.size(), 1U);
    EXPECT_EQ(readBack.buildingRemovals[0].buildingId, "hut");
    ASSERT_EQ(readBack.buildingDarkenings.size(), 1U);
    EXPECT_EQ(readBack.buildingDarkenings[0].buildingId, "hall");
    EXPECT_FLOAT_EQ(readBack.buildingDarkenings[0].darkening, 0.20F);
}

TEST(ObjectEditSave, MergeAppliesBuildingRemovalAndDarkening) {
    const TempPath file("voxel_edit_save_buildings_merge.toml");
    const ObjectTable base  = MakeKitBase();
    const ObjectTable layer = MakeBuildingEditLayer();

    SaveObjectEditLayer(file.path(), layer);
    const ObjectTable overlay = ObjectTable::LoadOverlayFromFile(file.path(), base);
    const ObjectTable merged  = MergeObjectTables(base, overlay);

    // `hut` 被按 id 删除；`hall` 保留且被覆盖为 0.20；本层新增 `tower` 追加在后。
    ASSERT_EQ(merged.buildings.size(), 2U);
    EXPECT_EQ(merged.buildings[0].id, "hall");
    EXPECT_FLOAT_EQ(merged.buildings[0].interiorDarkening, 0.20F);
    EXPECT_EQ(merged.buildings[1].id, "tower");
    // 围合体把覆盖后的值带出（渲染链路：Merge → ComputeBuildingEnclosure → 实例缓冲）。
    EXPECT_FLOAT_EQ(ComputeBuildingEnclosure(merged.buildings[0], merged, 0.0F).darkening, 0.20F);
}

TEST(ObjectEditSave, MergeRejectsBuildingRemovalForUnknownId) {
    ObjectTable base  = MakeKitBase();
    ObjectTable layer = MakeBuildingEditLayer();
    layer.buildingRemovals[0].buildingId = "does_not_exist";
    EXPECT_THROW(static_cast<void>(MergeObjectTables(base, layer)), std::runtime_error);
}

TEST(ObjectEditSave, MergeRejectsBuildingDarkeningForUnknownId) {
    ObjectTable base  = MakeKitBase();
    ObjectTable layer = MakeBuildingEditLayer();
    layer.buildingDarkenings[0].buildingId = "does_not_exist";
    EXPECT_THROW(static_cast<void>(MergeObjectTables(base, layer)), std::runtime_error);
}

}  // namespace
