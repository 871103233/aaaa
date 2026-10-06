#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 一个**已持久化的秘境实例绑定**（V10；见 [`plans/v0.5.md`](../../docs/plans/v0.5.md) §1.13.1）。
///
/// 口径（所有者 2026-10-06）：存档槽 = **单槽自动**；只存**秘境绑定**（世界 id + 实例种子 + 已重置次数），
/// **不含**玩家状态与世界改动。这样"同一存档反复启动 ⇒ 进入同一个 C；重置后新种子被持久化"。
struct SavedWorldInstance {
    std::string   worldId;           ///< 世界 id（纯 ASCII，如 `world_c`）
    std::uint64_t seed       = 0;    ///< 实例种子（决定该秘境长什么样）
    std::uint32_t generation = 0;    ///< **已重置次数**（0 = 尚未重置；与 `WorldInstance::generation` 同口径）
};

/// 存档槽内容（V10 **单槽**）：秘境实例绑定的集合。
///
/// 写入顺序由调用方给定（game 层按 `WorldManager::Order()` ⇒ **文件内容确定**，便于比对与测试）。
struct WorldInstanceSave {
    std::vector<SavedWorldInstance> instances;
};

/// 存档 **schema 版本**（红线 8：格式带版本 + 变更须配迁移函数与迁移测试）。
inline constexpr std::int64_t kWorldInstanceSaveSchemaVersion = 1;

/// 存档槽文件名（放在 `SDL_GetPrefPath` 给出的每用户目录下的 `saves/`）。
///
/// 为什么是 TOML 而非 `.voxr`：本档只存**元数据**（世界种子 / 生成次数），不含任何世界状态 ——
/// `save-and-serialization.md` §1 允许"文本格式用于 `level.dat` 这类人可读元数据"；
/// 未修改的世界靠种子**确定性重建**、本就不落盘 ⇒ **不触发 `.voxr` v2 字节布局的冻结**。
inline constexpr const char* kWorldInstanceSaveFileName = "instances.toml";

/// 读存档（TOML / UTF-8）：
///   - **文件不存在** ⇒ 返回空存档（首次运行，**不报错**）；
///   - 语法错 / 字段缺失或类型错 / `schema_version` 非整数 / **未知版本** ⇒ 抛 `std::runtime_error`（**不静默误读**）。
///
/// 版本迁移（红线 8）：读取按 `schema_version` **分派**；当前只有 v1，未知版本一律**拒绝**（见 `.cpp` 的 `ParseByVersion`）。
[[nodiscard]] WorldInstanceSave LoadWorldInstanceSave(const std::filesystem::path& path);

/// 写存档（TOML / UTF-8 / LF）：**临时文件 + `rename` 原子替换**（崩溃不会留下半个文件）。
///
/// 前置条件：父目录已存在，否则抛。属**极低频非热路径**写入（首次进入 / 重置 / 退出），与 `settings.toml` 同口径。
void SaveWorldInstanceSave(const std::filesystem::path& path, const WorldInstanceSave& save);

}  // namespace vx
