// 世界切换管理器（WorldManager，V2a）单测：清单注册表 / 当前世界记账 / 切换请求状态机的**纯逻辑**语义。
// 见 docs/plans/v0.5.md §1.7。

#include "world_manager.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::WorldManager;
using vx::WorldSwitchPhase;

/// 临时目录（析构递归删除）；本文件只用于"非法清单即抛"这类用例。
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

#ifdef VOXEL_SOURCE_DIR
/// 三份发布清单的路径（按目录顺序 = 注册顺序）。
[[nodiscard]] std::vector<std::filesystem::path> RepoWorldManifests() {
    const std::filesystem::path maps = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps";
    return { maps / "world_a.toml", maps / "world_b.toml", maps / "world_c.toml" };
}
#endif

TEST(WorldManager, RegistersRepoWorldsAndLooksThemUp) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());

    EXPECT_EQ(manager.Count(), 3U);
    ASSERT_EQ(manager.Order().size(), 3U);
    EXPECT_EQ(manager.Order()[0], "world_a");
    EXPECT_EQ(manager.Order()[1], "world_b");
    EXPECT_EQ(manager.Order()[2], "world_c");

    ASSERT_TRUE(manager.Contains("world_a"));
    ASSERT_NE(manager.Find("world_a"), nullptr);
    EXPECT_EQ(manager.Find("world_a")->terrain.tileRadiusX, 78);  // 10 km
    ASSERT_NE(manager.Find("world_b"), nullptr);
    EXPECT_EQ(manager.Find("world_b")->source, vx::WorldSource::Premade);
    EXPECT_EQ(manager.Find("world_b")->terrain.tileRadiusX, 8);  // 1 km
    ASSERT_NE(manager.Find("world_c"), nullptr);
    EXPECT_EQ(manager.Find("world_c")->family, vx::WorldFamily::InstanceRoguelike);

    EXPECT_FALSE(manager.Contains("world_z"));
    EXPECT_EQ(manager.Find("world_z"), nullptr);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, RegisterReturnsIdAndRejectsDuplicate) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager                   manager;
    const std::vector<std::filesystem::path> paths = RepoWorldManifests();

    EXPECT_EQ(manager.Register(paths[0]), "world_a");
    EXPECT_THROW((void)manager.Register(paths[0]), std::runtime_error);  // 同 id 再注册 ⇒ 抛
    EXPECT_EQ(manager.Count(), 1U);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, ActiveIsEmptyUntilSetAndUnknownIdThrows) {
    WorldManager manager;
    EXPECT_EQ(manager.ActiveId(), "");
    EXPECT_THROW((void)manager.Active(), std::logic_error);        // 未设定当前世界
    EXPECT_THROW(manager.SetActive("nope"), std::invalid_argument);  // 未注册
}

TEST(WorldManager, SwitchRequestIsRejectedForUnknownOrSameOrDuplicate) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    manager.SetActive("world_a");

    std::string reason;
    EXPECT_FALSE(manager.RequestSwitch("world_z", reason));  // 未知 id
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(manager.HasPendingSwitch());

    EXPECT_FALSE(manager.RequestSwitch("world_a", reason));  // 目标即当前世界
    EXPECT_FALSE(reason.empty());

    EXPECT_TRUE(manager.RequestSwitch("world_b", reason));  // 合法
    EXPECT_TRUE(reason.empty());
    EXPECT_EQ(manager.Phase(), WorldSwitchPhase::Requested);
    EXPECT_EQ(manager.PendingId(), "world_b");

    EXPECT_FALSE(manager.RequestSwitch("world_c", reason));  // 已有待处理请求
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(manager.PendingId(), "world_b");  // 原请求未被覆盖
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, TakePendingSwitchDoesNotChangeActiveUntilCommit) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    manager.SetActive("world_a");

    std::string reason;
    ASSERT_TRUE(manager.RequestSwitch("world_b", reason));
    const std::optional<std::string> taken = manager.TakePendingSwitch();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, "world_b");
    EXPECT_EQ(manager.Phase(), WorldSwitchPhase::Idle);
    EXPECT_FALSE(manager.HasPendingSwitch());
    EXPECT_EQ(manager.ActiveId(), "world_a");      // 未确认 ⇒ 当前世界不变
    EXPECT_EQ(manager.Active().id, "world_a");
    EXPECT_FALSE(manager.TakePendingSwitch().has_value());  // 只能取出一次

    manager.CommitActive("world_b");               // 装载成功 ⇒ 确认
    EXPECT_EQ(manager.ActiveId(), "world_b");
    EXPECT_EQ(manager.Active().id, "world_b");
    EXPECT_THROW(manager.CommitActive("nope"), std::invalid_argument);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, CancelPendingSwitchClearsTheRequest) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    manager.SetActive("world_a");

    std::string reason;
    ASSERT_TRUE(manager.RequestSwitch("world_c", reason));
    manager.CancelPendingSwitch();
    EXPECT_EQ(manager.Phase(), WorldSwitchPhase::Idle);
    EXPECT_FALSE(manager.HasPendingSwitch());
    EXPECT_TRUE(manager.PendingId().empty());
    EXPECT_FALSE(manager.TakePendingSwitch().has_value());
    EXPECT_TRUE(manager.RequestSwitch("world_c", reason));  // 取消后可重新请求
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, RegisteringInvalidManifestThrows) {
    const TempDir dir("vx_world_manager_bad");
    dir.Write("world.toml", R"(
schema_version = 1
id = "broken"
name = "坏世界"
family = "overworld"
source = "procedural"
terrain_preset = "missing_terrain.toml"
destruction_enabled = true
persistent = true
randomize_seed_on_entry = false
)");
    WorldManager manager;
    EXPECT_THROW((void)manager.Register(dir.Path("world.toml")), std::runtime_error);
    EXPECT_EQ(manager.Count(), 0U);
}

// ---- V5：秘境实例账本（`EnsureInstance` / `ResetInstance`；见 docs/plans/v0.5.md §1.13）----
//
// 语义（所有者 2026-10-06）：**首次进入 roll 一次种子 ⇒ 反复进入复用 ⇒ 重置才换种子**。

TEST(WorldManager, EnsureInstanceCreatesThenReusesTheSameSeed) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    std::string reason;

    // 首次 ⇒ 建实例（generation = 0，采用传入种子）。
    ASSERT_TRUE(manager.EnsureInstance("world_c", 111ULL, reason)) << reason;
    const vx::WorldInstance* first = manager.FindInstance("world_c");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->seed, 111ULL);
    EXPECT_EQ(first->generation, 0U);
    EXPECT_EQ(first->worldId, "world_c");

    // 再次进入（即便传入另一个种子）⇒ **复用**：这正是"反复进入 ⇒ 同一个世界"。
    ASSERT_TRUE(manager.EnsureInstance("world_c", 999ULL, reason)) << reason;
    const vx::WorldInstance* again = manager.FindInstance("world_c");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->seed, 111ULL);
    EXPECT_EQ(again->generation, 0U);
    EXPECT_EQ(manager.InstanceCount(), 1U);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, ResetInstanceBumpsGenerationAndReplacesSeed) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    std::string reason;

    // 没有实例就重置 ⇒ 拒绝（不"凭空重置"）。
    EXPECT_FALSE(manager.ResetInstance("world_c", 222ULL, reason));
    EXPECT_FALSE(reason.empty());

    ASSERT_TRUE(manager.EnsureInstance("world_c", 111ULL, reason)) << reason;
    ASSERT_TRUE(manager.ResetInstance("world_c", 222ULL, reason)) << reason;
    const vx::WorldInstance* second = manager.FindInstance("world_c");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->seed, 222ULL);
    EXPECT_EQ(second->generation, 1U);

    ASSERT_TRUE(manager.ResetInstance("world_c", 333ULL, reason)) << reason;
    const vx::WorldInstance* third = manager.FindInstance("world_c");
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->seed, 333ULL);
    EXPECT_EQ(third->generation, 2U);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

TEST(WorldManager, InstanceLedgerRejectsUnknownAndNonRoguelikeWorlds) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    std::string reason;

    // 未知 id。
    EXPECT_FALSE(manager.EnsureInstance("nope", 1ULL, reason));
    EXPECT_FALSE(reason.empty());

    // A 是 overworld、B 是 instance_premade ⇒ **都不参与秘境实例化**（只有肉鸽才实例化）。
    EXPECT_FALSE(manager.EnsureInstance("world_a", 1ULL, reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(manager.EnsureInstance("world_b", 1ULL, reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(manager.ResetInstance("world_a", 1ULL, reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(manager.InstanceCount(), 0U);
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

// ---- V10：读档装回（`RestoreInstance`；见 docs/plans/v0.5.md §1.13.1）----
//
// 与 `EnsureInstance` 的区别：本函数**必须能覆盖**已有绑定（把上次存档装回），而 `EnsureInstance` 绝不覆盖。

TEST(WorldManager, RestoreInstanceWrittenBindingAndOverwritesExisting) {
#ifdef VOXEL_SOURCE_DIR
    WorldManager manager;
    manager.RegisterAll(RepoWorldManifests());
    std::string reason;

    // 无实例 ⇒ 直接装回（可带 `generation`，而 `EnsureInstance` 只能建 generation = 0）。
    ASSERT_TRUE(manager.RestoreInstance("world_c", 555ULL, 4U, reason)) << reason;
    const vx::WorldInstance* instance = manager.FindInstance("world_c");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->seed, 555ULL);
    EXPECT_EQ(instance->generation, 4U);

    // 已有 ⇒ **覆盖**（"读档装回"的语义；这也是它不能复用 `EnsureInstance` 的原因）。
    ASSERT_TRUE(manager.RestoreInstance("world_c", 777ULL, 9U, reason)) << reason;
    instance = manager.FindInstance("world_c");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->seed, 777ULL);
    EXPECT_EQ(instance->generation, 9U);
    EXPECT_EQ(manager.InstanceCount(), 1U);

    // 非 roguelike / 未知 id ⇒ 拒绝（返回 false + 原因，不静默）。
    EXPECT_FALSE(manager.RestoreInstance("world_b", 1ULL, 0U, reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(manager.RestoreInstance("nope", 1ULL, 0U, reason));
    EXPECT_FALSE(reason.empty());
#else
    GTEST_SKIP() << "VOXEL_SOURCE_DIR 未定义";
#endif
}

}  // namespace
