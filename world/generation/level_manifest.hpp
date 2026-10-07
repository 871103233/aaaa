#pragma once

#include "generation/map_preset.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace vx {

/// **世界族**（[ADR 0028](../../docs/adr/0028-world-families-and-static-asset-first.md) §一）：
/// 三型世界的**唯一差异来源 = `LevelManifest`**（常驻策略 / 破坏策略 / 存档策略 / 数据来源）。
enum class WorldFamily : std::uint8_t {
    Overworld,          ///< 大世界（窗口流式 + LOD；标记区域可破坏；持久化）
    InstancePremade,    ///< 预制小世界（≈1 km，全量常驻；按用途持久化）
    InstanceRoguelike,  ///< 随机小世界（≈1 km，种子程序化生成；退出即丢）
};

/// 世界的数据来源。
enum class WorldSource : std::uint8_t {
    Procedural,  ///< 按种子运行期生成（地形预设的 `seed` / `edits`）
    Premade,     ///< 读**离线烘焙**的预制文件（`premade_file`，[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md) 的容器）
};

/// 枚举 → 配置串（日志与错误信息用）。
[[nodiscard]] const char* ToString(WorldFamily family) noexcept;
[[nodiscard]] const char* ToString(WorldSource source) noexcept;

/// **世界清单**（[ADR 0028](../../docs/adr/0028-world-families-and-static-asset-first.md) §一）：
/// "加载 / 切换一个世界"所需的**全部差异**。
///
/// 设计口径：
///   - **清单只"声明"，不"复制"地形**：`terrain` 由 `terrainPresetPath` 指向的 `MapPreset` **加载**而来
///     （`spawn` / `seed` / `tile_radius` / `edits` 的**单一事实来源仍是地形预设**）；
///     因此"目标世界的出生点由目标清单声明"（ADR 0028 决策四）由**传递引用**满足，不会出现两处口径。
///   - 加载失败（文件缺失 / 语法错 / 字段缺失 / 取值非法 / 枚举串非法 / **引用的地形预设非法**）
///     一律**抛 `std::runtime_error`** 并中止启动，**禁止**静默回退（与 `MapPreset` 同一口径，ADR 0005）。
struct LevelManifest {
    /// 当前文件格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    int         schemaVersion = kSchemaVersion;
    std::string id;    ///< 唯一标识（如 `world_a`；`--world=<id>` 用它选世界）
    std::string name;  ///< 显示名（日志 / 加载界面）

    WorldFamily family = WorldFamily::Overworld;
    WorldSource source = WorldSource::Procedural;

    /// `source = premade` 时**必填**（仓库相对路径）；`procedural` 时**必须为空**。
    ///
    /// **清单加载时只校验"非空"**：预制文件的存在性与格式校验发生在**实际加载世界时**——
    /// 因为预制文件是**离线烘焙产物**（不入库），此刻可能尚未生成（由 `tools/bake_premade_maps.ps1` 生成）。
    std::string premadeFile;

    /// `premade_file` 按**清单所在目录**解析出的路径（仅 `source = premade` 时非空；与 `terrain_preset` 同口径）。
    ///
    /// V4 起由 `game/main.cpp` 据此**打开并校验**预制容器（存在 / 魔数 / 版本 / 半径与种子与清单一致）；
    /// 校验失败**即抛**（不静默回退到程序化生成）。
    std::filesystem::path premadeFilePath;

    /// 引用的**地形预设**（`MapPreset`）路径（仓库相对）；必填且必须可加载。
    std::string terrainPresetPath;

    /// 该世界的**物件放置清单**（`objects_file`，可选；[ADR 0028](../../docs/adr/0028-world-families-and-static-asset-first.md) §一
    /// "差异全部落在清单" ⇒ **每个世界有自己的放置清单**）。
    ///
    /// 解析口径：
    ///   - **给出**（非空字符串）⇒ 相对**清单所在目录**解析（与 `terrain_preset` 同口径，V3）；
    ///   - **未给出** ⇒ 回退到**全局默认** `assets/config/objects.toml`（**仓库相对**字面量）。
    /// 因此本字段要么是**绝对路径**（给出且清单为绝对路径时），要么是**仓库相对的待拼接路径**
    /// （缺省时）—— 由 game 层按 `is_absolute()` 分流（见 `plans/v0.5.md` §1.8）。
    std::filesystem::path objectsFile = "assets/config/objects.toml";

    /// 该世界的**可编辑层**（阶段 V0.5 的 E1：手工摆放静态资产的落点；可选）。
    ///
    /// 解析口径（`plans/v0.5.md` §1.18）：
    ///   - **给出** `objects_edit_file`（非空）⇒ 相对**清单所在目录**解析；
    ///   - **未给出** ⇒ **派生默认** = 与 `objects_file` 同目录、同主名 + `.edit.toml`
    ///     （例：`world_a_objects.toml` ⇒ `world_a_objects.edit.toml`）。
    ///
    /// **缺失语义**（由 game 层执行）：**显式给出却不存在 ⇒ 抛**（不静默）；**派生默认却不存在 ⇒ 跳过**
    /// （编辑层按需生成，不改变既有行为）。加载顺序 = **发布清单（只读）→ 编辑层（叠加）**。
    std::filesystem::path objectsEditFilePath;

    /// `objectsEditFilePath` 来自**显式配置**（true）还是**派生默认**（false）—— 决定"文件不存在"是抛还是跳过。
    bool objectsEditFileExplicit = false;

    /// 该世界的**破坏能力**（ADR 0028 §一 的"破坏策略"）。
    bool destructionEnabled = true;

    /// 该世界是否**持久化**（"存档策略"）。
    ///
    /// **V0.10（2026-10-07）现状：仍未被消费**。存档能力已落地（`.voxr` v2，见 ADR 0037），但 `game/main.cpp`
    /// **没有**用本字段决定"该世界是否落盘"（当前只在启动日志里打印是 / 否）⇒ 声明 `persistent = false` 的世界
    /// （C / 肉鸽秘境）**也会**被落盘。**已登记为未决缺口**（切换条件 = 所有者裁定"是否按清单门控落盘"），
    /// 见 `docs/plans/v0.10.md` §3「阻塞 / 未决」与 `docs/adr/README.md` §五.3。
    bool persistent = false;

    /// **首次进入时随机生成实例种子**（仅 `instance_roguelike` 可开；V5 起消费）。
    ///
    /// **语义收紧（2026-10-06）**：**不是**"每次进入都换种子" —— 那会让"反复进入一样"不成立。
    /// 正确口径见 `game/world_manager.hpp` 的 `WorldInstance`：首次 roll 一次 ⇒ 反复进入**复用** ⇒ **重置**才换。
    bool randomizeSeedOnEntry = false;

    /// 由 `terrainPresetPath` 加载的**地形预设**（spawn / seed / 半径 / 编辑的唯一事实来源）。
    MapPreset terrain;

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`。
    [[nodiscard]] static LevelManifest LoadFromFile(const std::filesystem::path& path);
};

}  // namespace vx
