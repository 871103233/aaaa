// 秘境存档槽（V10）单测：**往返逐字段一致** + **未知版本被拒**（红线 8 的迁移口径）+ 非法字段即抛。
// 见 docs/plans/v0.5.md §1.13.1。

#include "save/world_instance_save.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::LoadWorldInstanceSave;
using vx::SavedWorldInstance;
using vx::SaveWorldInstanceSave;
using vx::WorldInstanceSave;

/// 临时目录（析构递归删除）。
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

TEST(WorldInstanceSave, RoundTripsEveryFieldIncludingSeedAboveInt64Max) {
    const TempDir dir("vx_instance_save_roundtrip");
    const std::filesystem::path file = dir.Path("instances.toml");

    WorldInstanceSave save;
    // 含一个 **> INT64_MAX** 的 u64 种子（`RollInstanceSeed` 会产生这种值）⇒ 必须无损往返。
    save.instances.push_back(SavedWorldInstance { "world_c", 18415848916547283674ULL, 0U });
    save.instances.push_back(SavedWorldInstance { "world_d", 42ULL, 7U });

    SaveWorldInstanceSave(file, save);

    // 原子替换：临时文件不得残留。
    EXPECT_FALSE(std::filesystem::exists(dir.Path("instances.toml.tmp")));

    const WorldInstanceSave loaded = LoadWorldInstanceSave(file);
    ASSERT_EQ(loaded.instances.size(), 2U);
    EXPECT_EQ(loaded.instances[0].worldId, "world_c");
    EXPECT_EQ(loaded.instances[0].seed, 18415848916547283674ULL);
    EXPECT_EQ(loaded.instances[0].generation, 0U);
    EXPECT_EQ(loaded.instances[1].worldId, "world_d");
    EXPECT_EQ(loaded.instances[1].seed, 42ULL);
    EXPECT_EQ(loaded.instances[1].generation, 7U);
}

TEST(WorldInstanceSave, MissingFileIsAnEmptySaveAndDoesNotThrow) {
    const TempDir dir("vx_instance_save_missing");
    const WorldInstanceSave loaded = LoadWorldInstanceSave(dir.Path("does_not_exist.toml"));
    EXPECT_TRUE(loaded.instances.empty());
}

TEST(WorldInstanceSave, EmptySaveRoundTripsToNoInstances) {
    const TempDir dir("vx_instance_save_empty");
    const std::filesystem::path file = dir.Path("instances.toml");

    SaveWorldInstanceSave(file, WorldInstanceSave {});
    const WorldInstanceSave loaded = LoadWorldInstanceSave(file);
    EXPECT_TRUE(loaded.instances.empty());
}

TEST(WorldInstanceSave, RejectsUnknownSchemaVersionInsteadOfMisreading) {
    const TempDir dir("vx_instance_save_version");
    dir.Write("instances.toml", R"(
schema_version = 99

[[instance]]
world_id = "world_c"
seed = "1"
generation = 0
)");
    // 红线 8：未知版本**不静默误读** ⇒ 抛（未来版本须在此加迁移函数 + 迁移测试）。
    EXPECT_THROW((void)LoadWorldInstanceSave(dir.Path("instances.toml")), std::runtime_error);
}

TEST(WorldInstanceSave, MissingSchemaVersionThrows) {
    const TempDir dir("vx_instance_save_noschema");
    dir.Write("instances.toml", "[[instance]]\nworld_id = \"world_c\"\nseed = \"1\"\ngeneration = 0\n");
    EXPECT_THROW((void)LoadWorldInstanceSave(dir.Path("instances.toml")), std::runtime_error);
}

TEST(WorldInstanceSave, RejectsInvalidFields) {
    const TempDir dir("vx_instance_save_invalid");

    const auto expectThrow = [&dir](const char* file, const char* content) {
        dir.Write(file, content);
        EXPECT_THROW((void)LoadWorldInstanceSave(dir.Path(file)), std::runtime_error) << file;
    };

    // 空 world_id。
    expectThrow("empty_id.toml", "schema_version = 1\n[[instance]]\nworld_id = \"\"\nseed = \"1\"\ngeneration = 0\n");
    // seed 不是纯十进制数字（拒绝静默截断）。
    expectThrow("bad_seed.toml", "schema_version = 1\n[[instance]]\nworld_id = \"world_c\"\nseed = \"12ab\"\ngeneration = 0\n");
    // generation 越界（> u32 上限）。
    expectThrow("bad_generation.toml",
                "schema_version = 1\n[[instance]]\nworld_id = \"world_c\"\nseed = \"1\"\ngeneration = 4294967296\n");
    // generation 缺失。
    expectThrow("missing_generation.toml", "schema_version = 1\n[[instance]]\nworld_id = \"world_c\"\nseed = \"1\"\n");
    // instance 不是数组。
    expectThrow("not_array.toml", "schema_version = 1\ninstance = 3\n");
}

TEST(WorldInstanceSave, AcceptsIntegerSeedFormForHandEditedFile) {
    const TempDir dir("vx_instance_save_int_seed");
    dir.Write("instances.toml", "schema_version = 1\n[[instance]]\nworld_id = \"world_c\"\nseed = 12345\ngeneration = 2\n");

    const WorldInstanceSave loaded = LoadWorldInstanceSave(dir.Path("instances.toml"));
    ASSERT_EQ(loaded.instances.size(), 1U);
    EXPECT_EQ(loaded.instances[0].seed, 12345ULL);
    EXPECT_EQ(loaded.instances[0].generation, 2U);
}

}  // namespace
