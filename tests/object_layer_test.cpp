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

using vx::ObjectAssetKind;
using vx::ObjectInstance;
using vx::ObjectLayer;
using vx::ObjectPlacement;
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

}  // namespace
