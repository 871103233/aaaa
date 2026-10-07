// V0.8 单测：**模块化建筑 kit 与成套建筑**（[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md)）。
//
// 覆盖两类**可判定不变量**：
//   ① 构件几何（代理体）：底面贴 `y = 0`、水平占地 = 整模数格、**门洞真的留空了**（可进入性的几何保证）、无退化三角形；
//   ② 配置校验（解析期，非法即抛）：模数必须整除占地、门洞墙必须留得下门楣与墙垛、成套建筑的引用必须有效。
// 端到端"人能走进去"是**人工验收项**（本环境无法注入输入），见 plans/v0.8.md 的验收表。

#include "object/object_layer.hpp"
#include "object/object_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using vx::BuildObjectMesh;
using vx::ComputeBuildingEnclosure;
using vx::kKitDoorClearanceBlocks;
using vx::kKitDoorWidthBlocks;
using vx::ObjectAssetKind;
using vx::ObjectEnclosure;
using vx::ObjectKitRole;
using vx::ObjectTable;
using vx::ObjectType;

/// 把 TOML 文本写到临时文件；析构时删除（与 `object_layer_test.cpp` 同口径）。
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

void ExpectLoadThrows(const std::filesystem::path& path) {
    EXPECT_THROW(static_cast<void>(ObjectTable::LoadFromFile(path)), std::runtime_error);
}

/// 造一个构件类型（不经过配置解析；供**几何**用例使用）。
[[nodiscard]] ObjectType MakeKitType(ObjectKitRole role, float halfX, float halfY, float halfZ, float module) {
    ObjectType type;
    type.id           = "kit_test";
    type.kind         = ObjectAssetKind::Kit;
    type.kitRole      = role;
    type.moduleBlocks = module;
    type.halfExtentX  = halfX;
    type.halfExtentY  = halfY;
    type.halfExtentZ  = halfZ;
    type.destructible = false;
    return type;
}

/// 断言：网格没有**退化三角形**（面积 > 0）—— 与 ADR 0007 的"退化三角形 = 0"口径一致。
void ExpectNoDegenerateTriangles(const vx::MeshData& mesh) {
    ASSERT_EQ(mesh.indices.size() % 3U, 0U);
    for (std::size_t t = 0; t + 2U < mesh.indices.size(); t += 3U) {
        const vx::MeshVertex& a = mesh.vertices[mesh.indices[t + 0U]];
        const vx::MeshVertex& b = mesh.vertices[mesh.indices[t + 1U]];
        const vx::MeshVertex& c = mesh.vertices[mesh.indices[t + 2U]];
        const float ab[3] = { b.position[0] - a.position[0], b.position[1] - a.position[1],
                              b.position[2] - a.position[2] };
        const float ac[3] = { c.position[0] - a.position[0], c.position[1] - a.position[1],
                              c.position[2] - a.position[2] };
        const float cross[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                                 ab[0] * ac[1] - ab[1] * ac[0] };
        const float areaTwice = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
        EXPECT_GT(areaTwice, 1.0e-6F) << "第 " << (t / 3U) << " 个三角形退化";
    }
}

/// 网格的包围盒（用于断言"占地 = 整模数格 / 底面贴地"）。
struct Bounds {
    float minX = 0.0F, maxX = 0.0F, minY = 0.0F, maxY = 0.0F, minZ = 0.0F, maxZ = 0.0F;
};

[[nodiscard]] Bounds ComputeBounds(const vx::MeshData& mesh) {
    Bounds bounds;
    bool   first = true;
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        if (first) {
            bounds.minX = bounds.maxX = vertex.position[0];
            bounds.minY = bounds.maxY = vertex.position[1];
            bounds.minZ = bounds.maxZ = vertex.position[2];
            first       = false;
            continue;
        }
        bounds.minX = std::min(bounds.minX, vertex.position[0]);
        bounds.maxX = std::max(bounds.maxX, vertex.position[0]);
        bounds.minY = std::min(bounds.minY, vertex.position[1]);
        bounds.maxY = std::max(bounds.maxY, vertex.position[1]);
        bounds.minZ = std::min(bounds.minZ, vertex.position[2]);
        bounds.maxZ = std::max(bounds.maxZ, vertex.position[2]);
    }
    return bounds;
}

// ---- ① 构件几何（代理体）----

/// 地板 / 屋顶：一块**底面贴地**的板，顶面 = 可站面；占地正好一个模数格。
TEST(ObjectKit, FloorAndRoofAreSlabsOnTheGround) {
    for (const ObjectKitRole role : { ObjectKitRole::Floor, ObjectKitRole::Roof }) {
        const vx::MeshData mesh = BuildObjectMesh(MakeKitType(role, 2.0F, 0.15F, 2.0F, 4.0F));
        ASSERT_FALSE(mesh.vertices.empty());
        const Bounds bounds = ComputeBounds(mesh);
        EXPECT_FLOAT_EQ(bounds.minY, 0.0F);            // 底面贴 `y = 0`（与其它形态同一约定）
        EXPECT_NEAR(bounds.maxY, 0.30F, 1.0e-5F);      // 板厚 = `kKitSlabThicknessBlocks`
        EXPECT_FLOAT_EQ(bounds.minX, -2.0F);
        EXPECT_FLOAT_EQ(bounds.maxX, 2.0F);
        EXPECT_FLOAT_EQ(bounds.minZ, -2.0F);
        EXPECT_FLOAT_EQ(bounds.maxZ, 2.0F);
        ExpectNoDegenerateTriangles(mesh);
    }
}

/// 墙：占满模数格的**地面投影**，但几何只占中间一层薄板（沿 Z 薄）⇒ 相邻件拼接不会互相插入。
TEST(ObjectKit, WallIsThinSlabCenteredInModuleCell) {
    const vx::MeshData mesh = BuildObjectMesh(MakeKitType(ObjectKitRole::Wall, 2.0F, 1.5F, 2.0F, 4.0F));
    const Bounds       bounds = ComputeBounds(mesh);
    EXPECT_FLOAT_EQ(bounds.minY, 0.0F);
    EXPECT_NEAR(bounds.maxY, 3.0F, 1.0e-5F);                     // 墙高 = 2*half_extent.y
    EXPECT_FLOAT_EQ(bounds.minX, -2.0F);                         // 占地 = 模数格
    EXPECT_FLOAT_EQ(bounds.maxX, 2.0F);
    EXPECT_NEAR(bounds.minZ, -0.15F, 1.0e-5F);                   // 厚度 = kKitSlabThicknessBlocks（居中）
    EXPECT_NEAR(bounds.maxZ, 0.15F, 1.0e-5F);
    ExpectNoDegenerateTriangles(mesh);
}

/// **可进入性的几何保证**：门洞墙在洞口区域**没有任何几何**（洞真的通了），且保留了门楣与两侧墙垛。
TEST(ObjectKit, DoorWallLeavesARealOpening) {
    const vx::MeshData mesh = BuildObjectMesh(MakeKitType(ObjectKitRole::WallDoor, 2.0F, 1.5F, 2.0F, 4.0F));
    ASSERT_FALSE(mesh.vertices.empty());

    const float halfDoor = kKitDoorWidthBlocks * 0.5F;
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        const bool insideOpening = std::fabs(vertex.position[0]) < halfDoor - 1.0e-4F &&
                                   vertex.position[1] < kKitDoorClearanceBlocks - 1.0e-4F;
        EXPECT_FALSE(insideOpening) << "洞口内出现了几何 ⇒ 洞不透（不可进入）";
    }

    const Bounds bounds = ComputeBounds(mesh);
    EXPECT_NEAR(bounds.maxY, 3.0F, 1.0e-5F);                     // 门楣存在（洞口上方仍有墙）
    EXPECT_NEAR(bounds.minX, -2.0F, 1.0e-5F);                    // 两侧墙垛存在
    EXPECT_NEAR(bounds.maxX, 2.0F, 1.0e-5F);
    ExpectNoDegenerateTriangles(mesh);
}

/// 四个角色都产出非空、底面贴地、法线单位、无退化三角形的网格。
TEST(ObjectKit, AllRolesProduceValidMeshes) {
    const ObjectKitRole roles[] = { ObjectKitRole::Floor, ObjectKitRole::Wall, ObjectKitRole::WallDoor,
                                    ObjectKitRole::Roof };
    for (const ObjectKitRole role : roles) {
        const vx::MeshData mesh = BuildObjectMesh(MakeKitType(role, 2.0F, 1.5F, 2.0F, 4.0F));
        ASSERT_FALSE(mesh.vertices.empty());
        EXPECT_FLOAT_EQ(ComputeBounds(mesh).minY, 0.0F);
        for (const vx::MeshVertex& vertex : mesh.vertices) {
            const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                                           vertex.normal[1] * vertex.normal[1] +
                                           vertex.normal[2] * vertex.normal[2]);
            EXPECT_NEAR(length, 1.0F, 1.0e-4F);
            EXPECT_GE(vertex.position[1], -1.0e-6F);
        }
        ExpectNoDegenerateTriangles(mesh);
    }
}

// ---- ② 配置校验（解析期）----

constexpr const char* kKitBase = R"(
schema_version = 1

[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[type]]
id = "kit_wall_door"
kind = "kit"
kit_role = "wall_door"
half_extent = [2.0, 1.5, 2.0]
module_blocks = 4.0
destructible = false
)";

TEST(ObjectKitParse, ParsesKitRoleAndModuleBlocks) {
    const TempToml file("vx_kit_ok.toml", kKitBase);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    const ObjectType* floor = table.Find("kit_floor");
    ASSERT_NE(floor, nullptr);
    EXPECT_EQ(floor->kind, ObjectAssetKind::Kit);
    EXPECT_EQ(floor->kitRole, ObjectKitRole::Floor);
    EXPECT_FLOAT_EQ(floor->moduleBlocks, 4.0F);
    const ObjectType* door = table.Find("kit_wall_door");
    ASSERT_NE(door, nullptr);
    EXPECT_EQ(door->kitRole, ObjectKitRole::WallDoor);
}

TEST(ObjectKitParse, KitWithoutRoleThrows) {
    const TempToml file("vx_kit_norole.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectKitParse, KitWithoutModuleThrows) {
    const TempToml file("vx_kit_nomodule.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
destructible = false
)");
    ExpectLoadThrows(file.path());
}

/// 模数必须**整除水平占地** —— 这是"拼起来没有错缝"的可判定不变量（不自洽即抛，不静默）。
TEST(ObjectKitParse, ModuleMustDivideFootprint) {
    const TempToml file("vx_kit_badmodule.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [1.5, 0.15, 2.0]
module_blocks = 4.0
destructible = false
)");
    ExpectLoadThrows(file.path());  // 3.0 / 4.0 不是整数 ⇒ 拒绝
}

/// `kit_role` / `module_blocks` **只允许出现在 `kind = "kit"`** 上（写了却不生效 = 静默配置，一律拒绝）。
TEST(ObjectKitParse, KitFieldsOnNonKitThrow) {
    const TempToml file("vx_kit_onstone.toml", R"(
schema_version = 1
[[type]]
id = "stone_a"
kind = "stone"
half_extent = [1.0, 1.0, 1.0]
module_blocks = 4.0
destructible = false
)");
    ExpectLoadThrows(file.path());
}

/// 门洞墙必须**留得下门楣**（`2*half_extent.y > 门洞净高`），否则门洞通到顶 —— 解析期拒绝。
TEST(ObjectKitParse, DoorWallMustBeTallerThanClearance) {
    const TempToml file("vx_kit_shortdoor.toml", R"(
schema_version = 1
[[type]]
id = "kit_wall_door"
kind = "kit"
kit_role = "wall_door"
half_extent = [2.0, 1.0, 2.0]
module_blocks = 4.0
destructible = false
)");
    ExpectLoadThrows(file.path());  // 2*half_extent.y = 2.0 ≤ 2.2
}

constexpr const char* kBuildingBase = R"(
schema_version = 1

[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[type]]
id = "kit_roof"
kind = "kit"
kit_role = "roof"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[type]]
id = "portal_gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
category = "portal"
destructible = false

[[building]]
id = "hut"
position = [-8.0, 0.0, -8.0]
yaw_deg = 0.0
pieces = [
    { type = "kit_floor", offset = [0.0, 0.0, 0.0] },
    { type = "kit_roof",  offset = [0.0, 3.3, 0.0], yaw_deg = 90.0 },
]
)";

TEST(ObjectKitParse, ParsesBuildingWithPieceOffsets) {
    const TempToml file("vx_kit_building_ok.toml", kBuildingBase);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.buildings.size(), 1U);
    EXPECT_EQ(table.buildings[0].id, "hut");
    EXPECT_FLOAT_EQ(table.buildings[0].x, -8.0F);
    EXPECT_FLOAT_EQ(table.buildings[0].z, -8.0F);
    ASSERT_EQ(table.buildings[0].pieces.size(), 2U);
    EXPECT_EQ(table.buildings[0].pieces[0].typeId, "kit_floor");
    EXPECT_FLOAT_EQ(table.buildings[0].pieces[0].offsetY, 0.0F);
    EXPECT_FLOAT_EQ(table.buildings[0].pieces[1].offsetY, 3.3F);   // **层高**来自相对偏移
    EXPECT_FLOAT_EQ(table.buildings[0].pieces[1].yawDegrees, 90.0F);
}

TEST(ObjectKitParse, BuildingWithUnknownPieceTypeThrows) {
    const TempToml file("vx_kit_building_unknown.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[building]]
id = "hut"
position = [0.0, 0.0, 0.0]
pieces = [ { type = "nope", offset = [0.0, 0.0, 0.0] } ]
)");
    ExpectLoadThrows(file.path());
}

/// 构件**不得引用传送门**（传送门需要 `target_world`，而构件项没有该字段 ⇒ 拒绝，避免静默失效）。
TEST(ObjectKitParse, BuildingWithPortalPieceThrows) {
    const TempToml file("vx_kit_building_portal.toml", R"(
schema_version = 1
[[type]]
id = "portal_gate"
kind = "portal"
half_extent = [1.0, 1.5, 0.2]
category = "portal"
destructible = false

[[building]]
id = "hut"
position = [0.0, 0.0, 0.0]
pieces = [ { type = "portal_gate", offset = [0.0, 0.0, 0.0] } ]
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectKitParse, BuildingWithEmptyPiecesThrows) {
    const TempToml file("vx_kit_building_empty.toml", R"(
schema_version = 1
[[building]]
id = "hut"
position = [0.0, 0.0, 0.0]
pieces = []
)");
    ExpectLoadThrows(file.path());
}

TEST(ObjectKitParse, DuplicateBuildingIdThrows) {
    const TempToml file("vx_kit_building_dup.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[building]]
id = "hut"
position = [0.0, 0.0, 0.0]
pieces = [ { type = "kit_floor", offset = [0.0, 0.0, 0.0] } ]

[[building]]
id = "hut"
position = [20.0, 0.0, 20.0]
pieces = [ { type = "kit_floor", offset = [0.0, 0.0, 0.0] } ]
)");
    ExpectLoadThrows(file.path());
}

/// 缺省（没有 `[[building]]` 段）⇒ 本表为空 ⇒ 与引入本形态之前逐位一致。
TEST(ObjectKitParse, BuildingsDefaultToEmpty) {
    const TempToml file("vx_kit_nobuilding.toml", kKitBase);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    EXPECT_TRUE(table.buildings.empty());
}

/// **发布配置**（A 世界物件清单）可加载，且带着 V0.8 的 kit 类型与那座可进入小屋。
TEST(ObjectKitParse, RepoWorldAConfigLoadsKitAndBuilding) {
#ifdef VOXEL_SOURCE_DIR
    const ObjectTable table = ObjectTable::LoadFromFile(
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps" / "world_a_objects.toml");
    const ObjectType* floor = table.Find("kit_floor");
    ASSERT_NE(floor, nullptr);
    EXPECT_EQ(floor->kind, ObjectAssetKind::Kit);
    EXPECT_FLOAT_EQ(floor->moduleBlocks, 4.0F);
    const ObjectType* door = table.Find("kit_wall_door");
    ASSERT_NE(door, nullptr);
    EXPECT_EQ(door->kitRole, ObjectKitRole::WallDoor);

    ASSERT_EQ(table.buildings.size(), 1U);
    EXPECT_EQ(table.buildings[0].id, "spawn_hut");
    ASSERT_EQ(table.buildings[0].pieces.size(), 6U);   // 地板 + 3 面墙 + 1 面门洞墙 + 屋顶
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

// ---- ③ 围合体代理（V0.8 室内变暗，ADR 0035 决策四）----

/// 有屋顶 ⇒ 围合体 = 屋顶并集的**包围盒**；`ceilingY` = 屋檐下沿（锚点地表 + 屋顶相对偏移）。
TEST(ObjectKitEnclosure, RoofDefinesEnclosureBox) {
    const TempToml      file("vx_kit_enclosure_ok.toml", kBuildingBase);
    const ObjectTable   table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.buildings.size(), 1U);
    const ObjectEnclosure enclosure = ComputeBuildingEnclosure(table.buildings[0], table, 10.0F);

    EXPECT_TRUE(enclosure.enabled);
    EXPECT_FLOAT_EQ(enclosure.centerX, -8.0F);
    EXPECT_FLOAT_EQ(enclosure.centerZ, -8.0F);
    EXPECT_FLOAT_EQ(enclosure.halfX, 2.0F);
    EXPECT_FLOAT_EQ(enclosure.halfZ, 2.0F);  // 屋顶绕 Y 转 90°：正方形footprint不变
    EXPECT_FLOAT_EQ(enclosure.ceilingY, 13.3F);
}

/// 无屋顶 ⇒ 不是"可进入空间" ⇒ `enabled = false`（片元据启用位整段跳过室内变暗）。
TEST(ObjectKitEnclosure, NoRoofIsNotEnclosed) {
    const TempToml file("vx_kit_enclosure_noroof.toml", R"(
schema_version = 1
[[type]]
id = "kit_floor"
kind = "kit"
kit_role = "floor"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[building]]
id = "patio"
position = [0.0, 0.0, 0.0]
pieces = [ { type = "kit_floor", offset = [0.0, 0.0, 0.0] } ]
)");
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.buildings.size(), 1U);
    EXPECT_FALSE(ComputeBuildingEnclosure(table.buildings[0], table, 0.0F).enabled);
}

/// 多块屋顶 ⇒ 取**并集**；`ceilingY` 取**最低**的下沿（最凹的屋檐决定天花板高度）。
TEST(ObjectKitEnclosure, MultipleRoofsUnionAndLowestEave) {
    const TempToml file("vx_kit_enclosure_union.toml", R"(
schema_version = 1
[[type]]
id = "kit_roof"
kind = "kit"
kit_role = "roof"
half_extent = [2.0, 0.15, 2.0]
module_blocks = 4.0
destructible = false

[[building]]
id = "hall"
position = [0.0, 0.0, 0.0]
pieces = [
    { type = "kit_roof", offset = [0.0, 3.0, 0.0] },
    { type = "kit_roof", offset = [4.0, 2.5, 0.0] },
]
)");
    const ObjectTable      table = ObjectTable::LoadFromFile(file.path());
    ASSERT_EQ(table.buildings.size(), 1U);
    const ObjectEnclosure enclosure = ComputeBuildingEnclosure(table.buildings[0], table, 5.0F);
    EXPECT_TRUE(enclosure.enabled);
    EXPECT_FLOAT_EQ(enclosure.centerX, 2.0F);    // 并集 x ∈ [-2, 6]
    EXPECT_FLOAT_EQ(enclosure.halfX, 4.0F);
    EXPECT_FLOAT_EQ(enclosure.centerZ, 0.0F);
    EXPECT_FLOAT_EQ(enclosure.halfZ, 2.0F);
    EXPECT_FLOAT_EQ(enclosure.ceilingY, 7.5F);   // min(5+3, 5+2.5)
}

/// **发布配置**的 `spawn_hut`：4×4 的围合体、屋檐下沿 = 锚点地表 + 3.3。
TEST(ObjectKitEnclosure, RepoSpawnHutEnclosure) {
#ifdef VOXEL_SOURCE_DIR
    const ObjectTable table = ObjectTable::LoadFromFile(
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps" / "world_a_objects.toml");
    ASSERT_EQ(table.buildings.size(), 1U);
    const ObjectEnclosure enclosure = ComputeBuildingEnclosure(table.buildings[0], table, 20.0F);
    EXPECT_TRUE(enclosure.enabled);
    EXPECT_FLOAT_EQ(enclosure.centerX, -8.0F);
    EXPECT_FLOAT_EQ(enclosure.centerZ, -8.0F);
    EXPECT_FLOAT_EQ(enclosure.halfX, 2.0F);
    EXPECT_FLOAT_EQ(enclosure.halfZ, 2.0F);
    EXPECT_FLOAT_EQ(enclosure.ceilingY, 23.3F);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

/// 确定性（红线 7）：同输入 ⇒ 逐位同输出。
TEST(ObjectKitEnclosure, IsDeterministic) {
    const TempToml    file("vx_kit_enclosure_det.toml", kBuildingBase);
    const ObjectTable table = ObjectTable::LoadFromFile(file.path());
    const ObjectEnclosure a = ComputeBuildingEnclosure(table.buildings[0], table, 10.0F);
    const ObjectEnclosure b = ComputeBuildingEnclosure(table.buildings[0], table, 10.0F);
    EXPECT_EQ(a.enabled, b.enabled);
    EXPECT_FLOAT_EQ(a.centerX, b.centerX);
    EXPECT_FLOAT_EQ(a.centerZ, b.centerZ);
    EXPECT_FLOAT_EQ(a.halfX, b.halfX);
    EXPECT_FLOAT_EQ(a.halfZ, b.halfZ);
    EXPECT_FLOAT_EQ(a.ceilingY, b.ceilingY);
}

}  // namespace
