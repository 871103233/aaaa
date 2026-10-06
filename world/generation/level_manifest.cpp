#include "generation/level_manifest.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::string ReadString(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::string> value = table[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是字符串");
    }
    return *value;
}

/// 策略位一律**必填**：显式写出可避免"漏写 ⇒ 悄悄取默认"这类静默回退（ADR 0005 口径）。
[[nodiscard]] bool ReadRequiredBool(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<bool> value = table[field].value<bool>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是布尔值");
    }
    return *value;
}

[[nodiscard]] WorldFamily ParseFamily(const std::string& text, const std::filesystem::path& path) {
    if (text == "overworld") {
        return WorldFamily::Overworld;
    }
    if (text == "instance_premade") {
        return WorldFamily::InstancePremade;
    }
    if (text == "instance_roguelike") {
        return WorldFamily::InstanceRoguelike;
    }
    throw std::runtime_error(path.string() + ": 字段 [family] 非法：\"" + text +
                             "\"（仅允许 overworld / instance_premade / instance_roguelike）");
}

[[nodiscard]] WorldSource ParseSource(const std::string& text, const std::filesystem::path& path) {
    if (text == "procedural") {
        return WorldSource::Procedural;
    }
    if (text == "premade") {
        return WorldSource::Premade;
    }
    throw std::runtime_error(path.string() + ": 字段 [source] 非法：\"" + text +
                             "\"（仅允许 procedural / premade）");
}

/// 解析一个**相对清单文件所在目录**的引用路径（绝对路径原样返回）。
/// 让"一个世界"自成一个目录（清单 + 地形预设 + 预制文件相邻），与 C `#include` / Unity Addressables 同直觉。
[[nodiscard]] std::filesystem::path ResolveReference(const std::filesystem::path& manifestPath,
                                                     const std::string&     reference) {
    const std::filesystem::path referenced(reference);
    if (referenced.is_absolute()) {
        return referenced;
    }
    return manifestPath.parent_path() / referenced;
}

}  // namespace

const char* ToString(WorldFamily family) noexcept {
    switch (family) {
        case WorldFamily::Overworld:
            return "overworld";
        case WorldFamily::InstancePremade:
            return "instance_premade";
        case WorldFamily::InstanceRoguelike:
            return "instance_roguelike";
    }
    return "overworld";
}

const char* ToString(WorldSource source) noexcept {
    switch (source) {
        case WorldSource::Procedural:
            return "procedural";
        case WorldSource::Premade:
            return "premade";
    }
    return "procedural";
}

LevelManifest LevelManifest::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载世界清单 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    LevelManifest manifest;
    manifest.schemaVersion = static_cast<int>(*schemaVersion);

    manifest.id = ReadString(document, path, "id");
    if (manifest.id.empty()) {
        throw std::runtime_error(path.string() + ": 字段 [id] 不能为空");
    }
    manifest.name = ReadString(document, path, "name");
    if (manifest.name.empty()) {
        throw std::runtime_error(path.string() + ": 字段 [name] 不能为空");
    }

    manifest.family = ParseFamily(ReadString(document, path, "family"), path);
    manifest.source = ParseSource(ReadString(document, path, "source"), path);

    // `premade_file`：可选字段，但**是否允许出现**由 `source` 决定（见下）。
    if (const toml::node* node = document.get("premade_file"); node != nullptr) {
        const std::optional<std::string> file = node->value<std::string>();
        if (!file.has_value()) {
            throw std::runtime_error(Describe(path, "premade_file") + "不是字符串");
        }
        manifest.premadeFile = *file;
    }
    if (manifest.source == WorldSource::Premade && manifest.premadeFile.empty()) {
        throw std::runtime_error(path.string() + ": source = premade 时必须给出 [premade_file]");
    }
    if (manifest.source == WorldSource::Procedural && !manifest.premadeFile.empty()) {
        throw std::runtime_error(path.string() + ": source = procedural 时不得给出 [premade_file]（会指向不被读取的文件）");
    }
    // V4：按**清单所在目录**解析出预制文件的**实际路径**（与 terrain_preset 同口径）。
    // 这里只解析、**不打开**：预制文件是离线烘焙产物、可能尚未生成；打开与校验在 game 层加载世界时做。
    if (manifest.source == WorldSource::Premade) {
        manifest.premadeFilePath = ResolveReference(path, manifest.premadeFile);
    }

    manifest.destructionEnabled   = ReadRequiredBool(document, path, "destruction_enabled");
    manifest.persistent           = ReadRequiredBool(document, path, "persistent");
    manifest.randomizeSeedOnEntry = ReadRequiredBool(document, path, "randomize_seed_on_entry");

    // 族 × 策略的一致性（可判定，且不会误伤当前内容）。
    if (manifest.randomizeSeedOnEntry && manifest.family != WorldFamily::InstanceRoguelike) {
        throw std::runtime_error(path.string() + ": [randomize_seed_on_entry] 只对 instance_roguelike 有意义");
    }
    if (manifest.family == WorldFamily::InstanceRoguelike && manifest.persistent) {
        throw std::runtime_error(path.string() + ": instance_roguelike 必须 persistent = false（退出即丢）");
    }

    manifest.terrainPresetPath = ReadString(document, path, "terrain_preset");
    if (manifest.terrainPresetPath.empty()) {
        throw std::runtime_error(path.string() + ": 字段 [terrain_preset] 不能为空");
    }
    const std::filesystem::path terrainPath = ResolveReference(path, manifest.terrainPresetPath);
    try {
        manifest.terrain = MapPreset::LoadFromFile(terrainPath);
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": 引用的地形预设 [" + manifest.terrainPresetPath +
                                 "] 加载失败：" + error.what());
    }

    // `objects_file`（可选，V3）：**相对清单所在目录**解析（与 terrain_preset 同口径）。
    // 未给出 ⇒ 保留头里的**全局默认** `assets/config/objects.toml`（仓库相对，由 game 层拼接）。
    if (const toml::node* node = document.get("objects_file"); node != nullptr) {
        const std::optional<std::string> file = node->value<std::string>();
        if (!file.has_value()) {
            throw std::runtime_error(Describe(path, "objects_file") + "不是字符串");
        }
        if (file->empty()) {
            throw std::runtime_error(path.string() + ": 字段 [objects_file] 不能为空");
        }
        manifest.objectsFile = ResolveReference(path, *file);
    }

    return manifest;
}

}  // namespace vx
