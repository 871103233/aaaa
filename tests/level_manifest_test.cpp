// 世界清单（LevelManifest，ADR 0028 §一）单测：三份发布清单可加载、字段正确；
// 非法配置（缺字段 / 枚举串非法 / 族×策略不一致 / 引用地形预设缺失或非法）一律抛。
// 见 docs/plans/v0.5.md §1.6。

#include "generation/level_manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using vx::LevelManifest;
using vx::WorldFamily;
using vx::WorldSource;

/// 一个临时目录：析构时递归删除。用于"清单 + 它引用的地形预设"这类**多文件**用例。
class TempDir {
public:
    explicit TempDir(std::string name) : path_(std::filesystem::temp_directory_path() / std::move(name)) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    void Write(const std::string& file, const std::string& content) const {
        std::ofstream out(path_ / file, std::ios::binary | std::ios::trunc);
        out << content;
    }

    [[nodiscard]] std::filesystem::path Path(const std::string& file) const { return path_ / file; }

private:
    std::filesystem::path path_;
};

/// 一份**合法**的地形预设（`MapPreset`），供清单 `terrain_preset` 引用。
constexpr const char* kTerrainOk = R"(
schema_version = 1
name           = "test terrain"
seed           = 7
tile_radius    = [8, 8]
spawn          = [0.0, 0.0]
)";

/// `LoadFromFile` 带 `[[nodiscard]]` ⇒ 期望抛异常时需显式丢弃返回值。
void ExpectThrows(const std::filesystem::path& path) {
    EXPECT_THROW(static_cast<void>(LevelManifest::LoadFromFile(path)), std::runtime_error);
}

/// 一份**最小合法**的清单（`family = overworld`，引用 `terrain_ok.toml`）。
[[nodiscard]] std::string ValidManifest(const std::string& extraLines = std::string {}) {
    return std::string(R"(
schema_version = 1
id   = "unit_world"
name = "单元测试世界"
family = "overworld"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled     = true
persistent              = true
randomize_seed_on_entry = false
)") + extraLines;
}

TEST(LevelManifest, RepoManifestsLoadAndDescribeWorlds) {
#ifdef VOXEL_SOURCE_DIR
    const std::filesystem::path maps = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps";

    // ---- A：大世界（10×10 km；procedural；持久化）----
    const LevelManifest a = LevelManifest::LoadFromFile(maps / "world_a.toml");
    EXPECT_EQ(a.id, "world_a");
    EXPECT_FALSE(a.name.empty());
    EXPECT_EQ(a.family, WorldFamily::Overworld);
    EXPECT_EQ(a.source, WorldSource::Procedural);
    EXPECT_TRUE(a.premadeFile.empty());
    EXPECT_TRUE(a.destructionEnabled);
    EXPECT_TRUE(a.persistent);
    EXPECT_FALSE(a.randomizeSeedOnEntry);
    EXPECT_EQ(a.terrainPresetPath, "world_10km.toml");
    EXPECT_EQ(a.terrain.tileRadiusX, 78);  // 10 km
    EXPECT_EQ(a.terrain.tileRadiusZ, 78);
    EXPECT_EQ(a.objectsFile, maps / "world_a_objects.toml");  // V3：每世界自己的放置清单

    // ---- B：预制小世界（1×1 km；premade；有预制文件声明）----
    const LevelManifest b = LevelManifest::LoadFromFile(maps / "world_b.toml");
    EXPECT_EQ(b.id, "world_b");
    EXPECT_EQ(b.family, WorldFamily::InstancePremade);
    EXPECT_EQ(b.source, WorldSource::Premade);
    EXPECT_FALSE(b.premadeFile.empty());
    // 2026-10-06 所有者裁定「B 要写在程序里、何时访问都一样」⇒ **共享只读**（改不留）⇒ `persistent = false`。
    EXPECT_FALSE(b.persistent);
    EXPECT_EQ(b.terrain.tileRadiusX, 8);  // 1 km
    EXPECT_EQ(b.terrain.tileRadiusZ, 8);
    EXPECT_EQ(b.objectsFile, maps / "world_b_objects.toml");

    // ---- C：随机小世界（1×1 km；roguelike；每次换种子；不持久化）----
    const LevelManifest c = LevelManifest::LoadFromFile(maps / "world_c.toml");
    EXPECT_EQ(c.id, "world_c");
    EXPECT_EQ(c.family, WorldFamily::InstanceRoguelike);
    EXPECT_EQ(c.source, WorldSource::Procedural);
    EXPECT_TRUE(c.randomizeSeedOnEntry);
    EXPECT_FALSE(c.persistent);
    EXPECT_EQ(c.terrain.tileRadiusX, 8);  // 1 km
    EXPECT_EQ(c.terrain.tileRadiusZ, 8);
    EXPECT_EQ(c.objectsFile, maps / "world_c_objects.toml");
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(LevelManifest, LoadsAndResolvesTerrainPresetFromManifestDirectory) {
    const TempDir dir("vx_level_manifest_ok");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest());

    const LevelManifest manifest = LevelManifest::LoadFromFile(dir.Path("world.toml"));
    EXPECT_EQ(manifest.id, "unit_world");
    EXPECT_EQ(manifest.terrain.seed, 7U);  // 地形预设被真正加载（种子来自它）
    EXPECT_EQ(manifest.terrain.tileRadiusX, 8);
}

// --------------------------- 物件清单（objects_file，V3）---------------------------

TEST(LevelManifest, ObjectsFileDefaultsToGlobalConfig) {
    const TempDir dir("vx_level_manifest_objects_default");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest());  // 未给 objects_file

    const LevelManifest manifest = LevelManifest::LoadFromFile(dir.Path("world.toml"));
    EXPECT_EQ(manifest.objectsFile, std::filesystem::path("assets/config/objects.toml"));
}

TEST(LevelManifest, ObjectsFileResolvesRelativeToManifestDirectory) {
    const TempDir dir("vx_level_manifest_objects_ref");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest("objects_file = \"world_x_objects.toml\"\n"));

    const LevelManifest manifest = LevelManifest::LoadFromFile(dir.Path("world.toml"));
    EXPECT_EQ(manifest.objectsFile, dir.Path("world_x_objects.toml"));
}

TEST(LevelManifest, EmptyObjectsFileThrows) {
    const TempDir dir("vx_level_manifest_objects_empty");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest("objects_file = \"\"\n"));
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, ObjectsFileNonStringThrows) {
    const TempDir dir("vx_level_manifest_objects_type");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest("objects_file = 3\n"));
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, MissingIdThrows) {
    const TempDir dir("vx_level_manifest_no_id");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
name = "n"
family = "overworld"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, UnknownFamilyThrows) {
    const TempDir dir("vx_level_manifest_bad_family");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "dungeon"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, UnknownSourceThrows) {
    const TempDir dir("vx_level_manifest_bad_source");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "overworld"
source = "streamed_from_network"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, PremadeWithoutFileThrows) {
    const TempDir dir("vx_level_manifest_premade_no_file");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "instance_premade"
source = "premade"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, ProceduralWithPremadeFileThrows) {
    const TempDir dir("vx_level_manifest_proc_with_file");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", ValidManifest("premade_file = \"leftover.vxmap\"\n"));
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, RandomizeSeedOnNonRoguelikeThrows) {
    const TempDir dir("vx_level_manifest_rand_non_rogue");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "overworld"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = true
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, RoguelikePersistentThrows) {
    const TempDir dir("vx_level_manifest_rogue_persistent");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "instance_roguelike"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = true
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, MissingPolicyBoolThrows) {
    const TempDir dir("vx_level_manifest_no_policy");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "overworld"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, SchemaVersionMismatchThrows) {
    const TempDir dir("vx_level_manifest_schema");
    dir.Write("terrain_ok.toml", kTerrainOk);
    dir.Write("world.toml", R"(
schema_version = 99
id = "w"
name = "n"
family = "overworld"
source = "procedural"
terrain_preset = "terrain_ok.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, MissingTerrainPresetFieldThrows) {
    const TempDir dir("vx_level_manifest_no_terrain_field");
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "overworld"
source = "procedural"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, TerrainPresetNotFoundThrows) {
    const TempDir dir("vx_level_manifest_terrain_missing");
    dir.Write("world.toml", R"(
schema_version = 1
id = "w"
name = "n"
family = "overworld"
source = "procedural"
terrain_preset = "does_not_exist.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    ExpectThrows(dir.Path("world.toml"));
}

TEST(LevelManifest, TerrainPresetInvalidThrows) {
    const TempDir dir("vx_level_manifest_terrain_invalid");
    // 引用的地形预设本身非法（缺 seed）⇒ 清单加载必须失败，而不是带着半个地形往下走。
    dir.Write("terrain_ok.toml", R"(
schema_version = 1
name = "broken"
tile_radius = [8, 8]
spawn = [0.0, 0.0]
)");
    dir.Write("world.toml", ValidManifest());
    ExpectThrows(dir.Path("world.toml"));
}

}  // namespace
