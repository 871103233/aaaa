#pragma once

#include "generation/level_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vx {

/// 世界切换请求的状态（V2；[ADR 0028](../../docs/adr/0028-world-families-and-static-asset-first.md) 决策四"进程内真切世界"）。
enum class WorldSwitchPhase : std::uint8_t {
    Idle,       ///< 无待处理请求
    Requested,  ///< 已请求切换，等待装载器**分帧**卸载当前世界后开始装载目标
};

/// 一个**秘境实例**（V5 / [`plans/v0.5.md`](../../docs/plans/v0.5.md) §1.13）。
///
/// 生命周期（所有者 2026-10-06 口径："**C 地图生成后与玩家绑定，可以反复进入，同一个玩家反复进入后是一样的，直到被销毁**"）：
///   1. **首次进入** ⇒ `EnsureInstance` 由 game 层 roll 一个**实例种子**并记下（`generation = 0`）；
///   2. **之后的进入** ⇒ **复用**同一实例（种子不变）⇒ **同一个世界**（世界指纹逐位相同，红线 7）；
///   3. **销毁 / 重置** ⇒ `ResetInstance` 丢弃旧实例、roll 新种子、`generation + 1`。
///
/// **注意 `randomize_seed_on_entry` 的语义收紧**（本需求）：它 = "**首次进入时随机生成实例种子**"，
/// **不是**"每次进入都换种子" —— 后者会让"反复进入一样"不成立。
struct WorldInstance {
    std::string   worldId;            ///< 所属世界 id
    std::uint64_t seed       = 0;     ///< 实例种子（决定该秘境长什么样）
    std::uint32_t generation = 0;     ///< 第几次生成（0 = 首次；每次重置 +1）
};

/// **世界切换管理器**（V2a；[`plans/v0.5.md`](../../docs/plans/v0.5.md) §1.7）。
///
/// 本类**只做纯逻辑**：**清单注册表**（`id → LevelManifest`）+ **当前世界记账** + **切换请求状态机**；
/// **不碰 GPU / 物理 / 世界数据** ⇒ 可单测。真正的"卸载 → 装载 → 重建玩家与相机"由 `game/main.cpp`
/// 的装载器（V2b）在取到本类的请求后执行。
///
/// 拒绝规则（**不静默**：一律返回 false 并写出原因）：
///   ① 未知 id；② 目标即当前世界；③ 已有待处理请求（避免请求被悄悄覆盖）。
///
/// 为什么"取出"与"确认"分开（`TakePendingSwitch` / `CommitActive`）：装载可能失败 —— 失败时**当前世界必须
/// 仍是原世界**（不得出现"半个新世界"），故只有装载成功后才把当前世界改名。
class WorldManager {
public:
    /// 注册一份清单（按 `id`）。**id 重复**或清单非法（文件缺失 / 语法错 / 字段非法 / 引用地形预设非法）
    /// ⇒ 抛 `std::runtime_error`（ADR 0005）。返回注册到的 `id`（按值，避免引用随容器扩容失效）。
    [[nodiscard]] std::string Register(const std::filesystem::path& manifestPath) {
        LevelManifest manifest = LevelManifest::LoadFromFile(manifestPath);
        if (Find(manifest.id) != nullptr) {
            throw std::runtime_error("世界清单 id 重复 [" + manifest.id + "]：" + manifestPath.string());
        }
        const std::string id = manifest.id;
        m_order.push_back(id);
        m_manifests.emplace(id, std::move(manifest));
        return id;
    }

    /// 依次注册多份清单（注册顺序即 `Order()` 的顺序，确定性）。
    void RegisterAll(const std::vector<std::filesystem::path>& manifestPaths) {
        for (const std::filesystem::path& path : manifestPaths) {
            (void)Register(path);
        }
    }

    [[nodiscard]] std::size_t Count() const noexcept { return m_manifests.size(); }

    [[nodiscard]] bool Contains(const std::string& id) const noexcept {
        return m_manifests.find(id) != m_manifests.end();
    }

    [[nodiscard]] const LevelManifest* Find(const std::string& id) const noexcept {
        const auto found = m_manifests.find(id);
        return (found == m_manifests.end()) ? nullptr : &found->second;
    }

    /// 注册顺序（供日志"列出所有世界"用）。
    [[nodiscard]] const std::vector<std::string>& Order() const noexcept { return m_order; }

    /// 设定**当前世界**（启动时调用一次）。未知 id ⇒ 抛 `std::invalid_argument`。
    void SetActive(const std::string& id) {
        if (!Contains(id)) {
            throw std::invalid_argument("未知世界 id [" + id + "]（未注册）");
        }
        m_activeId = id;
    }

    [[nodiscard]] const std::string& ActiveId() const noexcept { return m_activeId; }

    /// 当前世界清单。**未设定当前世界** ⇒ 抛 `std::logic_error`。
    [[nodiscard]] const LevelManifest& Active() const {
        const LevelManifest* manifest = Find(m_activeId);
        if (manifest == nullptr) {
            throw std::logic_error("尚未设定当前世界（WorldManager::SetActive 未被调用）");
        }
        return *manifest;
    }

    /// 请求切到 `id`：成功返回 true；被拒绝返回 false 并写出 `outReason`（未知 id / 即当前世界 / 已有请求）。
    [[nodiscard]] bool RequestSwitch(const std::string& id, std::string& outReason) {
        if (!Contains(id)) {
            outReason = "未知世界 id [" + id + "]";
            return false;
        }
        if (id == m_activeId) {
            outReason = "目标即当前世界 [" + id + "]";
            return false;
        }
        if (m_phase == WorldSwitchPhase::Requested) {
            outReason = "已有待处理的世界切换请求 [" + m_pendingId + "]";
            return false;
        }
        m_pendingId = id;
        m_phase     = WorldSwitchPhase::Requested;
        outReason.clear();
        return true;
    }

    [[nodiscard]] WorldSwitchPhase Phase() const noexcept { return m_phase; }
    [[nodiscard]] bool             HasPendingSwitch() const noexcept {
        return m_phase == WorldSwitchPhase::Requested;
    }
    [[nodiscard]] const std::string& PendingId() const noexcept { return m_pendingId; }

    /// **取出**待处理请求（只取出一次）：返回目标 id、状态回到 `Idle`，**当前世界不变**。
    /// 装载成功后由装载器调 `CommitActive`；失败（或用户取消）则什么都不做 —— 当前世界仍是原世界。
    [[nodiscard]] std::optional<std::string> TakePendingSwitch() {
        if (m_phase != WorldSwitchPhase::Requested) {
            return std::nullopt;
        }
        std::string target = m_pendingId;
        m_pendingId.clear();
        m_phase = WorldSwitchPhase::Idle;
        return target;
    }

    /// 取消待处理请求（用户取消 / 装载被中止）。
    void CancelPendingSwitch() noexcept {
        m_pendingId.clear();
        m_phase = WorldSwitchPhase::Idle;
    }

    /// 装载**成功**后确认新世界（未知 id ⇒ 抛 `std::invalid_argument`）。
    void CommitActive(const std::string& id) { SetActive(id); }

    // ---- V5：秘境实例账本（纯逻辑；种子由 game 层提供 ⇒ 本类保持可单测、不碰随机数）----

    /// **确保秘境实例存在**：没有 ⇒ 用 `seed` 建一个（`generation = 0`）；**已有 ⇒ 复用**（不动种子）。
    ///
    /// 拒绝（返回 false + `outReason`，不静默）：**未知 id** / 该世界**不是 `instance_roguelike`**。
    /// 复用语义正是"反复进入 ⇒ 同一个世界"的实现。
    [[nodiscard]] bool EnsureInstance(const std::string& id, std::uint64_t seed, std::string& outReason) {
        const LevelManifest* manifest = Find(id);
        if (manifest == nullptr) {
            outReason = "未知世界 id [" + id + "]";
            return false;
        }
        if (manifest->family != WorldFamily::InstanceRoguelike) {
            outReason = "世界 [" + id + "] 不是肉鸽秘境（family=" + std::string(ToString(manifest->family)) +
                        "）⇒ 不参与秘境实例化";
            return false;
        }
        if (m_instances.find(id) == m_instances.end()) {
            m_instances.emplace(id, WorldInstance { id, seed, 0U });
        }
        outReason.clear();
        return true;
    }

    /// **销毁并重生**：丢弃该秘境的实例、以 `seed` 新建、`generation + 1`。
    ///
    /// 拒绝：**未知 id** / 非 `instance_roguelike` / **当前没有实例**（须先 `EnsureInstance`，避免"凭空重置"）。
    [[nodiscard]] bool ResetInstance(const std::string& id, std::uint64_t seed, std::string& outReason) {
        const LevelManifest* manifest = Find(id);
        if (manifest == nullptr) {
            outReason = "未知世界 id [" + id + "]";
            return false;
        }
        if (manifest->family != WorldFamily::InstanceRoguelike) {
            outReason = "世界 [" + id + "] 不是肉鸽秘境（family=" + std::string(ToString(manifest->family)) +
                        "）⇒ 不可重置";
            return false;
        }
        const auto found = m_instances.find(id);
        if (found == m_instances.end()) {
            outReason = "世界 [" + id + "] 当前没有秘境实例 ⇒ 先进入一次（EnsureInstance）再重置";
            return false;
        }
        found->second = WorldInstance { id, seed, found->second.generation + 1U };
        outReason.clear();
        return true;
    }

    /// 查秘境实例（不存在 ⇒ `nullptr`）。
    [[nodiscard]] const WorldInstance* FindInstance(const std::string& id) const noexcept {
        const auto found = m_instances.find(id);
        return (found == m_instances.end()) ? nullptr : &found->second;
    }

    /// **读档恢复**（V10）：把存档里的绑定（种子 + `generation`）**原样装回账本** —— 有则覆盖、无则新建。
    ///
    /// 与 `EnsureInstance` 的区别（**为什么单独一个入口**）：后者是"运行期首次进入"（无则建、**有则复用**、
    /// 绝不覆盖已有种子）；本函数只由**启动读档**调用，必须能**覆盖**（把上次的绑定装回）—— 若复用它，
    /// 就得给 `EnsureInstance` 开一个"覆盖已有种子"的口子，反而让运行期也可能误改种子。
    ///
    /// 拒绝（返回 false + `outReason`，不静默）：**未知 id** / 非 `instance_roguelike`。
    [[nodiscard]] bool RestoreInstance(const std::string& id, std::uint64_t seed, std::uint32_t generation,
                                       std::string& outReason) {
        const LevelManifest* manifest = Find(id);
        if (manifest == nullptr) {
            outReason = "未知世界 id [" + id + "]";
            return false;
        }
        if (manifest->family != WorldFamily::InstanceRoguelike) {
            outReason = "世界 [" + id + "] 不是肉鸽秘境（family=" + std::string(ToString(manifest->family)) +
                        "）⇒ 不参与秘境实例化";
            return false;
        }
        m_instances[id] = WorldInstance { id, seed, generation };
        outReason.clear();
        return true;
    }

    [[nodiscard]] std::size_t InstanceCount() const noexcept { return m_instances.size(); }

private:
    std::vector<std::string>                       m_order;      ///< 注册顺序（确定性）
    std::unordered_map<std::string, LevelManifest> m_manifests;  ///< `id → 清单`
    std::string                                    m_activeId;   ///< 当前世界 id（空 = 未设定）
    std::string                                    m_pendingId;  ///< 待处理的目标 id
    WorldSwitchPhase                               m_phase = WorldSwitchPhase::Idle;

    std::unordered_map<std::string, WorldInstance> m_instances;  ///< 秘境实例账本（V5；`id → 实例`）
};

}  // namespace vx
