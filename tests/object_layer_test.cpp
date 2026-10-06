// 物件层（ADR 0004 层③）单测：类型表 / 放置清单的 TOML 解析与校验，以及 ObjectLayer 的
// 放置 / 查询 / 移除 / 确定性遍历语义。见 docs/plans/v0.5.md §1.3。

#include "object/object_layer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::MergeObjectTables;
using vx::ObjectAssetKind;
using vx::ObjectInstance;
using vx::ObjectLayer;
using vx::ObjectPlacement;
using vx::ObjectScatter;
using vx::ObjectTable;
using vx::ObjectType;

/// 把 TOML 文本写到临时文件；析构时删除。文件名由调用方保证唯一（ctest 并行下不冲突）。
class TempToml {
public:
    TempToml(std::string name, const std::string& content)
        : path_(std::filesystem::temp_directory_path() / std::move(name)) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << content;
    }
    ~TempToml() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    TempToml(const TempToml&)            = delete;
    TempToml& operator=(const TempToml&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// `LoadFromFile` 带 `[[nodiscard]]`；期望抛异常时需显式丢弃返回值，故集中在这里。
void ExpectLoadThrows(const std::filesystem::path& path) {
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadFromFile(path)), std::runtime_error);
}

constexpr const char* kValid = R"(
schema_version = 1

[[type]]
id = "dirt_pile"
kind = "dirt_pile"
half_extent = [1.0, 0.75, 1.0]
destructible = true

[[type]]
id = "stone_boulder"
kind = "stone"
half_extent = [1.2, 0.9, 1.2]
destructible = false

[[placement]]
type = "dirt_pile"
position = [6.0, 0.0, 6.0]

[[placement]]
type = "stone_boulder"
position = [-8.0, 0.0, 4.0]
yaw_deg = 30.0
)";

// --------------------------- 类型表 / 放置清单解析 ---------------------------

TEST(ObjectTable, ParsesTypesAndPlacements) {
    const TempToml file("voxel_object_table_valid.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());

    ASSERT_EQ(table.schemaVersion, ObjectTable::kSchemaVersion);
    ASSERT_EQ(table.types.size(), 2U);
    ASSERT_EQ(table.placements.size(), 2U);

    const ObjectType* pile = table.Find("dirt_pile");
    ASSERT_NE(pile, nullptr);
    EXPECT_EQ(pile->kind, ObjectAssetKind::DirtPile);
    EXPECT_TRUE(pile->destructible);
    EXPECT_FLOAT_EQ(pile->halfExtentX, 1.0f);
    EXPECT_FLOAT_EQ(pile->halfExtentY, 0.75f);
    EXPECT_FLOAT_EQ(pile->halfExtentZ, 1.0f);

    const ObjectType* stone = table.Find("stone_boulder");
    ASSERT_NE(stone, nullptr);
    EXPECT_EQ(stone->kind, ObjectAssetKind::Stone);
    EXPECT_FALSE(stone->destructible);
    EXPECT_EQ(table.Find("no_such_type"), nullptr);

    // 放置按**文件顺序**（确定性）。
    EXPECT_EQ(table.placements[0].typeId, "dirt_pile");
    EXPECT_FLOAT_EQ(table.placements[0].x, 6.0f);
    EXPECT_FLOAT_EQ(table.placements[0].yawDegrees, 0.0f);
    EXPECT_EQ(table.placements[1].typeId, "stone_boulder");
    EXPECT_FLOAT_EQ(table.placements[1].z, 4.0f);
    EXPECT_FLOAT_EQ(table.placements[1].yawDegrees, 30.0f);
}

TEST(ObjectTable, RepoConfigLoads) {
#ifdef VOXEL_SOURCE_DIR
    const ObjectTable table = ObjectTable::LoadFromFile(
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "objects.toml");
    EXPECT_EQ(table.types.size(), 3U);
    EXPECT_EQ(table.placements.size(), 5U);
    EXPECT_TRUE(table.destructibleEnabled);  // 发布配置打开总开关（V0c）
    const ObjectType* pile = table.Find("dirt_pile");
    ASSERT_NE(pile, nullptr);
    EXPECT_TRUE(pile->destructible);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(ObjectTable, DestructibleEnabledDefaultsToTrue) {
    const TempToml file("voxel_object_table_switch_default.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    EXPECT_TRUE(table.destructibleEnabled);  // 缺省 = true（不写字段等价于打开）
}

TEST(ObjectTable, DestructibleEnabledParsesFalse) {
    const TempToml file("voxel_object_table_switch_off.toml", R"(
schema_version = 1
destructible_enabled = false

[[type]]
id = "pile"
kind = "dirt_pile"
half_extent = [1.0, 1.0, 1.0]
destructible = true
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    EXPECT_FALSE(table.destructibleEnabled);
}

TEST(ObjectTable, DestructibleEnabledNonBoolThrows) {
    const TempToml file("voxel_object_table_switch_bad.toml", R"(
schema_version = 1
destructible_enabled = "true"

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, DuplicateIdThrows) {
    const TempToml file("voxel_object_table_dup.toml", R"(
schema_version = 1

[[type]]
id = "same"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[type]]
id = "same"
kind = "crate"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, UnknownKindThrows) {
    const TempToml file("voxel_object_table_kind.toml", R"(
schema_version = 1

[[type]]
id = "mystery"
kind = "castle"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, NonPositiveExtentThrows) {
    const TempToml file("voxel_object_table_extent.toml", R"(
schema_version = 1

[[type]]
id = "flat"
kind = "stone"
half_extent = [1.0, 0.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, MissingFieldThrows) {
    const TempToml file("voxel_object_table_missing.toml", R"(
schema_version = 1

[[type]]
id = "no_kind"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, PlacementUnknownTypeThrows) {
    const TempToml file("voxel_object_table_badref.toml", R"(
schema_version = 1

[[type]]
id = "stone_boulder"
kind = "stone"
half_extent = [1.2, 0.9, 1.2]
destructible = false

[[placement]]
type = "ghost"
position = [0.0, 0.0, 0.0]
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, SchemaVersionMismatchThrows) {
    const TempToml file("voxel_object_table_schema.toml", R"(
schema_version = 2

[[type]]
id = "stone_boulder"
kind = "stone"
half_extent = [1.2, 0.9, 1.2]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

// --------------------------- 传送门（V3）---------------------------

TEST(ObjectTable, PortalWithoutTargetWorldThrows) {
    const TempToml file("voxel_object_table_portal_notarget.toml", R"(
schema_version = 1

[[type]]
id = "gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
destructible = false
category = "portal"

[[placement]]
type = "gate"
position = [0.0, 0.0, 0.0]
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, PortalWithEmptyTargetWorldThrows) {
    const TempToml file("voxel_object_table_portal_emptytarget.toml", R"(
schema_version = 1

[[type]]
id = "gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
destructible = false
category = "portal"

[[placement]]
type = "gate"
position = [0.0, 0.0, 0.0]
target_world = ""
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, PortalParsesTargetWorld) {
    const TempToml file("voxel_object_table_portal_ok.toml", R"(
schema_version = 1

[[type]]
id = "gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
destructible = false
category = "portal"

[[placement]]
type = "gate"
position = [4.0, 0.0, -2.0]
yaw_deg = 45.0
target_world = "world_b"
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.types.size(), 1U);
    EXPECT_EQ(table.types[0].kind, ObjectAssetKind::Portal);
    ASSERT_EQ(table.placements.size(), 1U);
    EXPECT_EQ(table.placements[0].targetWorldId, "world_b");
    EXPECT_FLOAT_EQ(table.placements[0].x, 4.0f);
    EXPECT_FLOAT_EQ(table.placements[0].z, -2.0f);
    EXPECT_FLOAT_EQ(table.placements[0].yawDegrees, 45.0f);
}

TEST(ObjectTable, NonPortalWithTargetWorldThrows) {
    const TempToml file("voxel_object_table_nontarget.toml", R"(
schema_version = 1

[[type]]
id = "stone_boulder"
kind = "stone"
half_extent = [1.2, 0.9, 1.2]
destructible = false

[[placement]]
type = "stone_boulder"
position = [0.0, 0.0, 0.0]
target_world = "world_b"
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, TargetWorldNonStringThrows) {
    const TempToml file("voxel_object_table_targettype.toml", R"(
schema_version = 1

[[type]]
id = "gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
destructible = false
category = "portal"

[[placement]]
type = "gate"
position = [0.0, 0.0, 0.0]
target_world = 3
)");
    ExpectLoadThrows(file.path());
}

// --------------------------- 外部模型 / 程序化散布（V8）---------------------------

TEST(ObjectTable, ModelWithoutModelFileThrows) {
    const TempToml file("voxel_object_table_model_nofile.toml", R"(
schema_version = 1

[[type]]
id = "tree"
kind = "model"
half_extent = [1.0, 3.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, ModelParsesModelFileAndMaterialSlot) {
    const TempToml file("voxel_object_table_model_ok.toml", R"(
schema_version = 1

[[type]]
id = "tree"
kind = "model"
model_file = "assets/models/nature/tree_default.glb"
half_extent = [1.33, 2.99, 1.14]
material_slot = 0
destructible = false

[[placement]]
type = "tree"
position = [4.0, 0.0, -4.0]
yaw_deg = 15.0

[[scatter]]
type = "tree"
center = [0.0, 0.0]
radius = 80.0
count = 18
seed = 1001
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.types.size(), 1U);
    EXPECT_EQ(table.types[0].kind, ObjectAssetKind::Model);
    EXPECT_EQ(table.types[0].modelFile, "assets/models/nature/tree_default.glb");
    EXPECT_EQ(table.types[0].materialSlot, 0);

    ASSERT_EQ(table.scatters.size(), 1U);
    EXPECT_EQ(table.scatters[0].typeId, "tree");
    EXPECT_FLOAT_EQ(table.scatters[0].centerX, 0.0f);
    EXPECT_FLOAT_EQ(table.scatters[0].centerZ, 0.0f);
    EXPECT_FLOAT_EQ(table.scatters[0].radius, 80.0f);
    EXPECT_EQ(table.scatters[0].count, 18);
    EXPECT_EQ(table.scatters[0].seed, 1001U);
}

TEST(ObjectTable, ModelMaterialSlotDefaultsToUnspecified) {
    const TempToml file("voxel_object_table_model_noslot.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "model"
model_file = "assets/models/nature/rock_smallB.glb"
half_extent = [0.54, 0.27, 0.54]
destructible = false
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.types.size(), 1U);
    EXPECT_EQ(table.types[0].materialSlot, -1);  // 未指定 ⇒ -1（调用方取形态默认 = 草槽）
}

TEST(ObjectTable, NonModelWithModelFileThrows) {
    const TempToml file("voxel_object_table_nomodel_file.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
model_file = "assets/models/nature/rock_smallB.glb"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, NonModelWithMaterialSlotThrows) {
    const TempToml file("voxel_object_table_nomodel_slot.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
material_slot = 2
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, ModelMaterialSlotOutOfRangeThrows) {
    const TempToml file("voxel_object_table_model_slot_range.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "model"
model_file = "assets/models/nature/rock_smallB.glb"
half_extent = [1.0, 1.0, 1.0]
material_slot = 4
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, ScatterUnknownTypeThrows) {
    const TempToml file("voxel_object_table_scatter_badtype.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[scatter]]
type = "ghost"
center = [0.0, 0.0]
radius = 10.0
count = 5
seed = 1
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, ScatterNonPositiveRadiusOrCountThrows) {
    const TempToml zeroRadius("voxel_object_table_scatter_r0.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[scatter]]
type = "rock"
center = [0.0, 0.0]
radius = 0.0
count = 5
seed = 1
)");
    ExpectLoadThrows(zeroRadius.path());

    const TempToml zeroCount("voxel_object_table_scatter_c0.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[scatter]]
type = "rock"
center = [0.0, 0.0]
radius = 10.0
count = 0
seed = 1
)");
    ExpectLoadThrows(zeroCount.path());
}

TEST(ObjectTable, ScatterCenterMustBeTwoNumbersAndSeedRequired) {
    const TempToml badCenter("voxel_object_table_scatter_center.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[scatter]]
type = "rock"
center = [0.0, 0.0, 0.0]
radius = 10.0
count = 5
seed = 1
)");
    ExpectLoadThrows(badCenter.path());

    const TempToml noSeed("voxel_object_table_scatter_noseed.toml", R"(
schema_version = 1

[[type]]
id = "rock"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false

[[scatter]]
type = "rock"
center = [0.0, 0.0]
radius = 10.0
count = 5
)");
    ExpectLoadThrows(noSeed.path());
}

// ------------------------------- ObjectLayer -------------------------------

TEST(ObjectLayer, PlaceGetRemoveAndCount) {
    const TempToml file("voxel_object_layer_basic.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());

    ObjectLayer layer;
    EXPECT_EQ(layer.Count(), 0U);

    const std::uint32_t first  = layer.Place(table, table.placements[0]);
    const std::uint32_t second = layer.Place(table, table.placements[1]);
    EXPECT_NE(first, second);
    EXPECT_EQ(layer.Count(), 2U);

    ObjectInstance instance;
    ASSERT_TRUE(layer.Get(first, instance));
    ASSERT_NE(instance.type, nullptr);
    EXPECT_EQ(instance.type->id, "dirt_pile");
    EXPECT_FLOAT_EQ(instance.x, 6.0f);
    EXPECT_FLOAT_EQ(instance.z, 6.0f);
    EXPECT_FLOAT_EQ(instance.yawDegrees, 0.0f);
    EXPECT_TRUE(instance.intact);

    EXPECT_TRUE(layer.Remove(first));
    EXPECT_FALSE(layer.Remove(first));
    EXPECT_EQ(layer.Count(), 1U);
    EXPECT_FALSE(layer.Get(first, instance));
    EXPECT_TRUE(layer.Get(second, instance));
}

TEST(ObjectLayer, ForEachFollowsPlacementOrder) {
    const TempToml file("voxel_object_layer_order.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());

    ObjectLayer layer;
    // 故意与文件顺序相反：应先放 stone_boulder（x = -8），再放 dirt_pile（x = 6）。
    layer.Place(table, table.placements[1]);
    layer.Place(table, table.placements[0]);

    std::vector<float> xs;
    layer.ForEach([&xs](const ObjectInstance& instance) { xs.push_back(instance.x); });

    ASSERT_EQ(xs.size(), 2U);
    EXPECT_FLOAT_EQ(xs[0], -8.0f);
    EXPECT_FLOAT_EQ(xs[1], 6.0f);
}

TEST(ObjectLayer, PlaceUnknownTypeThrows) {
    const TempToml file("voxel_object_layer_badplace.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());

    ObjectLayer layer;
    ObjectPlacement bad;
    bad.typeId = "missing";
    EXPECT_THROW(layer.Place(table, bad), std::invalid_argument);
    EXPECT_EQ(layer.Count(), 0U);
}

TEST(ObjectLayer, ClearResetsCountAndIds) {
    const TempToml file("voxel_object_layer_clear.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());

    ObjectLayer layer;
    layer.Place(table, table.placements[0]);
    layer.Place(table, table.placements[1]);
    ASSERT_EQ(layer.Count(), 2U);

    layer.Clear();
    EXPECT_EQ(layer.Count(), 0U);

    const std::uint32_t first = layer.Place(table, table.placements[0]);
    EXPECT_EQ(first, 1U);
}

// --------------------------- 可编辑层叠加（V0.5 E1）---------------------------

[[nodiscard]] ObjectTable MakeEmptyTable(bool destructibleEnabled) {
    ObjectTable table;
    table.schemaVersion       = ObjectTable::kSchemaVersion;
    table.destructibleEnabled = destructibleEnabled;
    return table;
}

TEST(MergeObjectTables, AppendsTypesPlacementsAndScattersInFileOrder) {
    ObjectTable base = MakeEmptyTable(true);
    base.types.push_back(ObjectType { "base_type", ObjectAssetKind::Stone, 1.0F, 1.0F, 1.0F, false });
    base.placements.push_back(ObjectPlacement { "base_type", 1.0F, 2.0F, 3.0F, 0.0F });

    ObjectTable overlay = MakeEmptyTable(true);
    overlay.types.push_back(ObjectType { "edit_type", ObjectAssetKind::Crate, 1.0F, 1.0F, 1.0F, false });
    overlay.placements.push_back(ObjectPlacement { "edit_type", 4.0F, 5.0F, 6.0F, 45.0F });
    overlay.scatters.push_back(ObjectScatter { "edit_type", 0.0F, 0.0F, 10.0F, 3, 7U });

    const ObjectTable merged = MergeObjectTables(base, overlay);
    ASSERT_EQ(merged.types.size(), 2U);
    EXPECT_EQ(merged.types[0].id, "base_type");
    EXPECT_EQ(merged.types[1].id, "edit_type");  // 编辑层**追加**在发布清单之后
    ASSERT_EQ(merged.placements.size(), 2U);
    EXPECT_EQ(merged.placements[0].typeId, "base_type");
    EXPECT_EQ(merged.placements[1].typeId, "edit_type");
    ASSERT_EQ(merged.scatters.size(), 1U);
    EXPECT_EQ(merged.scatters[0].typeId, "edit_type");
    EXPECT_TRUE(merged.destructibleEnabled);
}

TEST(MergeObjectTables, DuplicateTypeIdThrows) {
    ObjectTable base = MakeEmptyTable(true);
    base.types.push_back(ObjectType { "dup", ObjectAssetKind::Stone, 1.0F, 1.0F, 1.0F, false });

    ObjectTable overlay = MakeEmptyTable(true);
    overlay.types.push_back(ObjectType { "dup", ObjectAssetKind::Crate, 1.0F, 1.0F, 1.0F, false });

    EXPECT_THROW(static_cast<void>(MergeObjectTables(base, overlay)), std::runtime_error);
}

TEST(MergeObjectTables, DestructibleToggleMismatchThrows) {
    ObjectTable base    = MakeEmptyTable(true);
    ObjectTable overlay = MakeEmptyTable(false);
    EXPECT_THROW(static_cast<void>(MergeObjectTables(base, overlay)), std::runtime_error);
}

// --------------------------- 可编辑层加载（LoadOverlayFromFile，V0.5 E1）---------------------------

TEST(ObjectTable, OverlayWithoutTypeSectionParsesPlacementsOverBaseTypes) {
    const TempToml baseFile("voxel_object_overlay_base.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_overlay_placements.toml", R"(
schema_version = 1
destructible_enabled = true

[[placement]]
type = "dirt_pile"
position = [1.0, 0.0, 1.0]
yaw_deg = 15.0
)");
    const ObjectTable overlay = ObjectTable::LoadOverlayFromFile(overlayFile.path(), base);
    EXPECT_TRUE(overlay.types.empty());  // 省略 [[type]] 合法 = "只放落点"
    ASSERT_EQ(overlay.placements.size(), 1U);
    EXPECT_EQ(overlay.placements[0].typeId, "dirt_pile");
    EXPECT_TRUE(overlay.destructibleEnabled);
}

TEST(ObjectTable, OverlayAllowsNewTypeReferencedByItsOwnPlacement) {
    const TempToml baseFile("voxel_object_overlay_base2.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_overlay_newtype.toml", R"(
schema_version = 1

[[type]]
id = "edit_crate"
kind = "crate"
half_extent = [0.5, 0.5, 0.5]
destructible = false

[[placement]]
type = "edit_crate"
position = [0.0, 0.0, 4.0]
)");
    const ObjectTable overlay = ObjectTable::LoadOverlayFromFile(overlayFile.path(), base);
    ASSERT_EQ(overlay.types.size(), 1U);
    EXPECT_EQ(overlay.types[0].id, "edit_crate");
    ASSERT_EQ(overlay.placements.size(), 1U);
    EXPECT_EQ(overlay.placements[0].typeId, "edit_crate");
}

TEST(ObjectTable, OverlayDuplicateTypeIdWithBaseThrows) {
    const TempToml baseFile("voxel_object_overlay_base3.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_overlay_dup.toml", R"(
schema_version = 1

[[type]]
id = "dirt_pile"
kind = "crate"
half_extent = [1.0, 1.0, 1.0]
destructible = false
)");
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(overlayFile.path(), base)), std::runtime_error);
}

TEST(ObjectTable, OverlayPlacementOfUnknownTypeThrows) {
    const TempToml baseFile("voxel_object_overlay_base4.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_overlay_unknown.toml", R"(
schema_version = 1

[[placement]]
type = "does_not_exist"
position = [0.0, 0.0, 0.0]
)");
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(overlayFile.path(), base)), std::runtime_error);
}

TEST(ObjectTable, OverlayDestructibleToggleMismatchThrows) {
    const TempToml baseFile("voxel_object_overlay_base5.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_overlay_toggle.toml", R"(
schema_version = 1
destructible_enabled = false
)");
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(overlayFile.path(), base)), std::runtime_error);
}

// --------------------------- 仓库 / 类别（`category`，V0.5 E3）---------------------------

TEST(ObjectTable, CategoryDefaultsToMiscWhenOmitted) {
    const TempToml file("voxel_object_category_default.toml", kValid);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    const ObjectType* pile  = table.Find("dirt_pile");
    ASSERT_NE(pile, nullptr);
    EXPECT_EQ(pile->category, "misc");  // 缺省 = `misc`（不按形态 / 文件名推断）
}

TEST(ObjectTable, CategoryIsParsedWhenGiven) {
    const TempToml file("voxel_object_category_given.toml", R"(
schema_version = 1

[[type]]
id = "tree_default"
kind = "crate"
half_extent = [1.0, 2.0, 1.0]
destructible = false
category = "vegetation"
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    const ObjectType* type  = table.Find("tree_default");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->category, "vegetation");
    EXPECT_TRUE(vx::IsValidObjectCategory(type->category));
}

TEST(ObjectTable, InvalidCategoryThrows) {
    const TempToml file("voxel_object_category_invalid.toml", R"(
schema_version = 1

[[type]]
id = "weird"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false
category = "banana"
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, NonStringCategoryThrows) {
    const TempToml file("voxel_object_category_nonstring.toml", R"(
schema_version = 1

[[type]]
id = "weird"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
destructible = false
category = 3
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectTable, PortalCategoryMustBePortal) {
    const TempToml file("voxel_object_category_portal.toml", R"(
schema_version = 1

[[type]]
id = "gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
destructible = false
category = "prop"
)");
    ExpectLoadThrows(file.path());  // portal 形态必须归 `portal` 类别（否则会混进别的一级列表）
}

// --------------------------- 删除项（`[[remove]]`，V0.5 E3）---------------------------

TEST(RemovePlacementsByRemoval, FiltersByTypeAndPlanarToleranceKeepingOrder) {
    std::vector<ObjectPlacement> placements;
    placements.push_back(ObjectPlacement { "a", 1.0F, 0.0F, 1.0F, 0.0F });
    placements.push_back(ObjectPlacement { "b", 2.0F, 0.0F, 2.0F, 0.0F });
    placements.push_back(ObjectPlacement { "a", 5.0F, 0.0F, 5.0F, 0.0F });

    std::vector<vx::ObjectRemoval> removals;
    vx::ObjectRemoval              matched;
    matched.typeId    = "a";
    matched.x         = 1.2F;
    matched.z         = 0.9F;
    matched.tolerance = 0.5F;
    removals.push_back(matched);

    const std::vector<ObjectPlacement> kept = vx::RemovePlacementsByRemoval(placements, removals);
    ASSERT_EQ(kept.size(), 2U);
    EXPECT_EQ(kept[0].typeId, "b");  // 顺序保持（确定性；红线 7）
    EXPECT_EQ(kept[1].typeId, "a");
    EXPECT_FLOAT_EQ(kept[1].x, 5.0F);
}

TEST(RemovePlacementsByRemoval, TypeMismatchDoesNotRemove) {
    std::vector<ObjectPlacement> placements;
    placements.push_back(ObjectPlacement { "a", 1.0F, 0.0F, 1.0F, 0.0F });

    std::vector<vx::ObjectRemoval> removals;
    vx::ObjectRemoval              other;
    other.typeId    = "b";
    other.x         = 1.0F;
    other.z         = 1.0F;
    other.tolerance = 0.5F;
    removals.push_back(other);

    EXPECT_EQ(vx::RemovePlacementsByRemoval(placements, removals).size(), 1U);
}

TEST(ObjectTable, OverlayParsesRemovals) {
    const TempToml baseFile("voxel_object_remove_base.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_remove_overlay.toml", R"(
schema_version = 1

[[remove]]
type = "dirt_pile"
position = [6.0, 6.0]

[[remove]]
type = "stone_boulder"
position = [-8.0, 4.0]
tolerance = 1.25
)");
    const ObjectTable overlay = ObjectTable::LoadOverlayFromFile(overlayFile.path(), base);
    ASSERT_EQ(overlay.removals.size(), 2U);
    EXPECT_EQ(overlay.removals[0].typeId, "dirt_pile");
    EXPECT_FLOAT_EQ(overlay.removals[0].x, 6.0F);
    EXPECT_FLOAT_EQ(overlay.removals[0].z, 6.0F);
    EXPECT_FLOAT_EQ(overlay.removals[0].tolerance, 0.5F);  // 缺省 0.5 格
    EXPECT_FLOAT_EQ(overlay.removals[1].tolerance, 1.25F);
}

TEST(ObjectTable, RemoveOfUnknownTypeThrows) {
    const TempToml baseFile("voxel_object_remove_base2.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_remove_unknown.toml", R"(
schema_version = 1

[[remove]]
type = "does_not_exist"
position = [0.0, 0.0]
)");
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(overlayFile.path(), base)), std::runtime_error);
}

TEST(ObjectTable, NonPositiveRemoveToleranceThrows) {
    const TempToml baseFile("voxel_object_remove_base3.toml", kValid);
    const ObjectTable base = ObjectTable::LoadFromFile(baseFile.path());

    const TempToml overlayFile("voxel_object_remove_bad_tolerance.toml", R"(
schema_version = 1

[[remove]]
type = "dirt_pile"
position = [0.0, 0.0]
tolerance = 0.0
)");
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadOverlayFromFile(overlayFile.path(), base)), std::runtime_error);
}

TEST(MergeObjectTables, AppliesOverlayRemovalsToBaseBeforeAppending) {
    ObjectTable base = MakeEmptyTable(true);
    base.types.push_back(ObjectType { "base_type", ObjectAssetKind::Stone, 1.0F, 1.0F, 1.0F, false });
    base.placements.push_back(ObjectPlacement { "base_type", 1.0F, 2.0F, 3.0F, 0.0F });

    ObjectTable overlay = MakeEmptyTable(true);
    overlay.placements.push_back(ObjectPlacement { "base_type", 9.0F, 0.0F, 9.0F, 0.0F });
    vx::ObjectRemoval removal;
    removal.typeId    = "base_type";
    removal.x         = 1.0F;
    removal.z         = 3.0F;
    removal.tolerance = 0.5F;
    overlay.removals.push_back(removal);

    const ObjectTable merged = MergeObjectTables(base, overlay);
    // 发布清单里的 (1,3) 被删除项剔除 ⇒ 只剩本层新增的 (9,9)（顺序 = 发布清单 → 删除 → 追加）。
    ASSERT_EQ(merged.placements.size(), 1U);
    EXPECT_FLOAT_EQ(merged.placements[0].x, 9.0F);
    EXPECT_FLOAT_EQ(merged.placements[0].z, 9.0F);
}

}  // namespace
