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
    /// **本阶段只校验"非空"**：预制文件的存在性与格式校验发生在**实际加载世界时**（V4）——
    /// 因为预制文件是**离线烘焙产物**（不入库），此刻可能尚未生成。
    std::string premadeFile;

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

    /// 该世界的**破坏能力**（ADR 0028 §一 的"破坏策略"）。
    bool destructionEnabled = true;

    /// 该世界是否**持久化**（"存档策略"；存档尚未开始 ⇒ 本阶段只解析 / 校验 / 登记）。
    bool persistent = false;

    /// 每次进入是否**更换种子**（仅 `instance_roguelike` 可开；本阶段只解析 / 校验，V5 消费）。
    bool randomizeSeedOnEntry = false;

    /// 由 `terrainPresetPath` 加载的**地形预设**（spawn / seed / 半径 / 编辑的唯一事实来源）。
    MapPreset terrain;

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`。
    [[nodiscard]] static LevelManifest LoadFromFile(const std::filesystem::path& path);
};

}  // namespace vx
