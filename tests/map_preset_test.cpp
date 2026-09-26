#include "generation/map_preset.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using vx::Height;
using vx::kHeightUnitsPerBlock;
using vx::MapEdit;
using vx::MapEditMode;
using vx::MapPreset;
using vx::TerrainMaterialTable;
using vx::TerrainWorld;

constexpr std::uint64_t kExpectedSeed = 1592594996ULL;  // 0x5EED1234

[[nodiscard]] std::filesystem::path RepoMapPath() {
#ifdef VOXEL_SOURCE_DIR
    return std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps" / "test_range.toml";
#else
    return std::filesystem::path("assets/maps/test_range.toml");
#endif
}

/// 临时 TOML 文件守卫：写盘并在析构时删除（测试不留下垃圾文件）。
class TempToml {
public:
    explicit TempToml(const std::string& content) {
        static int counter = 0;
        m_path = std::filesystem::temp_directory_path() / ("voxel_map_preset_" + std::to_string(++counter) + ".toml");
        std::ofstream out(m_path, std::ios::binary);
        out << content;
    }
    ~TempToml() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }
    TempToml(const TempToml&) = delete;
    TempToml& operator=(const TempToml&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] const MapEdit* FindEdit(const MapPreset& preset, const std::string& name) {
    for (const MapEdit& edit : preset.edits) {
        if (edit.name == name) {
            return &edit;
        }
    }
    return nullptr;
}

}  // namespace

// T11 ①：合法文件解析出预期值。
TEST(MapPreset, LoadsValidFileWithExpectedValues) {
    const MapPreset preset = MapPreset::LoadFromFile(RepoMapPath());

    EXPECT_EQ(preset.schemaVersion, MapPreset::kSchemaVersion);
    EXPECT_FALSE(preset.name.empty());
    EXPECT_EQ(preset.seed, kExpectedSeed);
    EXPECT_EQ(preset.tileRadiusX, 1);
    EXPECT_EQ(preset.tileRadiusZ, 1);
    EXPECT_DOUBLE_EQ(preset.spawnX, 0.0);
    EXPECT_DOUBLE_EQ(preset.spawnZ, 0.0);

    ASSERT_FALSE(preset.edits.empty());

    const MapEdit* base = FindEdit(preset, "ground_base");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->mode, MapEditMode::Flatten);
    EXPECT_EQ(base->heightUnits, 120 * kHeightUnitsPerBlock);
    EXPECT_EQ(base->minX, -64);
    EXPECT_EQ(base->maxZ, 64);

    const MapEdit* pit = FindEdit(preset, "pit_hollow");
    ASSERT_NE(pit, nullptr);
    EXPECT_EQ(pit->mode, MapEditMode::Carve);
    EXPECT_EQ(pit->heightUnits, 24 * kHeightUnitsPerBlock);
    EXPECT_EQ(pit->minX, 34);
    EXPECT_EQ(pit->maxZ, 44);
}

// T11 ②：非法 / 缺字段的文件必须显式报错，不得静默回退。
TEST(MapPreset, InvalidFilesThrowExplicitErrors) {
    const char* kBadCases[] = {
        // 缺 schema_version
        "name = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n",
        // schema_version 不匹配
        "schema_version = 2\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n",
        // 缺 name
        "schema_version = 1\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n",
        // 缺 spawn
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\n",
        // 缺 seed
        "schema_version = 1\nname = \"x\"\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n",
        // mode 非法
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n"
        "[[edit]]\nname = \"e\"\nmode = \"dig\"\nmin = [0, 0]\nmax = [1, 1]\nheight = 1.0\n",
        // min > max
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n"
        "[[edit]]\nname = \"e\"\nmode = \"flatten\"\nmin = [5, 0]\nmax = [1, 1]\nheight = 1.0\n",
        // 缺 height
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n"
        "[[edit]]\nname = \"e\"\nmode = \"flatten\"\nmin = [0, 0]\nmax = [1, 1]\n",
        // 编辑矩形超出地图范围
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [0.0, 0.0]\n"
        "[[edit]]\nname = \"e\"\nmode = \"raise\"\nmin = [0, 0]\nmax = [999, 1]\nheight = 1.0\n",
        // spawn 超出地图范围
        "schema_version = 1\nname = \"x\"\nseed = 1\ntile_radius = [1, 1]\nspawn = [300.0, 0.0]\n",
    };

    for (const char* content : kBadCases) {
        const TempToml file(content);
        EXPECT_THROW((void)MapPreset::LoadFromFile(file.Path()), std::runtime_error) << content;
    }
}

// T11 ③：应用预设后——平整区恰为请求高度、carve 区低于周边、出生点在地表之上。
TEST(MapPreset, AppliesEditsToTerrainDeterministically) {
    const MapPreset preset = MapPreset::LoadFromFile(RepoMapPath());

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    for (int tileZ = -preset.tileRadiusZ; tileZ <= preset.tileRadiusZ; ++tileZ) {
        for (int tileX = -preset.tileRadiusX; tileX <= preset.tileRadiusX; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }

    // 平整区：基底与出生平台都必须恰为 120 格。
    Height height = 0;
    ASSERT_TRUE(world.ReadColumnHeight(0, 0, height));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(height), 120.0F);
    ASSERT_TRUE(world.ReadColumnHeight(50, -30, height));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(height), 120.0F);

    // 1 格台阶平台 = 121 格。
    ASSERT_TRUE(world.ReadColumnHeight(-38, -38, height));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(height), 121.0F);

    // 阶梯坡道顶端 = 130 格（与高台齐平）。
    ASSERT_TRUE(world.ReadColumnHeight(34, 0, height));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(height), 130.0F);

    // carve 区（坑底 = 96 格）必须低于周边（120 格）。
    Height pit = 0;
    Height rim = 0;
    ASSERT_TRUE(world.ReadColumnHeight(40, 38, pit));
    ASSERT_TRUE(world.ReadColumnHeight(30, 38, rim));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(pit), 96.0F);
    EXPECT_LT(pit, rim);

    // 地标塔远高于高台。
    ASSERT_TRUE(world.ReadColumnHeight(53, 0, height));
    EXPECT_FLOAT_EQ(vx::HeightToBlocks(height), 260.0F);

    // 出生点：列上有地形数据（在地图范围内），且脚底位置（地表 + 余量）严格高于地表。
    float surface = 0.0F;
    ASSERT_TRUE(world.QueryHeight(static_cast<float>(preset.spawnX), static_cast<float>(preset.spawnZ), surface));
    EXPECT_FLOAT_EQ(surface, 120.0F);
    constexpr float kClearance = 0.5F;
    EXPECT_GT(surface + kClearance, surface);
}
