#include "generation/map_preset.hpp"

#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <toml++/toml.hpp>

namespace vx {
namespace {

/// tile 半径上限：8 个 tile = 每边 512 列，足以覆盖 V0.1 小场景。
constexpr int kMaxTileRadius = 8;

/// 出生点与编辑矩形允许的世界列范围由地图范围决定；这里给出统一的越界描述。
[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::int64_t ReadInt(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::int64_t> value = table[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是整数");
    }
    return *value;
}

/// 读取数值：既接受 TOML 浮点（`120.0`）也接受整数（`120`）。
[[nodiscard]] double ReadNumber(const toml::table& table, const std::filesystem::path& path, const char* field) {
    if (const std::optional<double> value = table[field].value<double>(); value.has_value()) {
        return *value;
    }
    if (const std::optional<std::int64_t> value = table[field].value<std::int64_t>(); value.has_value()) {
        return static_cast<double>(*value);
    }
    throw std::runtime_error(Describe(path, field) + "缺失或不是数值");
}

[[nodiscard]] std::string ReadString(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::string> value = table[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是字符串");
    }
    return *value;
}

/// 读取长度为 2 的整数数组（如 `tile_radius` / `min` / `max`）。
[[nodiscard]] std::pair<int, int> ReadIntPair(const toml::table& table, const std::filesystem::path& path,
                                              const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != static_cast<std::size_t>(2)) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是恰好含 2 个整数的数组");
    }
    const std::optional<std::int64_t> first  = (*array)[0].value<std::int64_t>();
    const std::optional<std::int64_t> second = (*array)[1].value<std::int64_t>();
    if (!first.has_value() || !second.has_value()) {
        throw std::runtime_error(Describe(path, field) + "的元素必须都是整数");
    }
    return { static_cast<int>(*first), static_cast<int>(*second) };
}

/// 读取长度为 2 的数值数组（如 `spawn`），接受整数或浮点。
[[nodiscard]] std::pair<double, double> ReadNumberPair(const toml::table& table, const std::filesystem::path& path,
                                                       const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != static_cast<std::size_t>(2)) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是恰好含 2 个数值的数组");
    }

    // `(*array)[i]` 是 node_view（不是 node&），故用泛型 lambda 直接取其数值。
    const auto readOne = [](const auto& node) -> std::optional<double> {
        if (const std::optional<double> value = node.value<double>(); value.has_value()) {
            return value;
        }
        if (const std::optional<std::int64_t> value = node.value<std::int64_t>(); value.has_value()) {
            return static_cast<double>(*value);
        }
        return std::nullopt;
    };
    const std::optional<double> first  = readOne((*array)[0]);
    const std::optional<double> second = readOne((*array)[1]);
    if (!first.has_value() || !second.has_value()) {
        throw std::runtime_error(Describe(path, field) + "的元素必须都是数值");
    }
    return { *first, *second };
}

[[nodiscard]] MapEditMode ParseMode(const std::string& text, const std::filesystem::path& path, std::size_t index) {
    if (text == "flatten") {
        return MapEditMode::Flatten;
    }
    if (text == "raise") {
        return MapEditMode::Raise;
    }
    if (text == "carve") {
        return MapEditMode::Carve;
    }
    throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) + " 的 mode 非法：\"" + text +
                             "\"（仅允许 flatten / raise / carve）");
}

constexpr double kMaxHeightBlocks = static_cast<double>(kMaxTerrainHeightBlocks);

}  // namespace

MapPreset MapPreset::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载地图预设 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    MapPreset preset;
    preset.schemaVersion = static_cast<int>(*schemaVersion);

    preset.name = ReadString(document, path, "name");
    if (preset.name.empty()) {
        throw std::runtime_error(path.string() + ": 字段 [name] 不能为空");
    }

    const std::int64_t seed = ReadInt(document, path, "seed");
    if (seed < 0) {
        throw std::runtime_error(path.string() + ": 字段 [seed] 不能为负");
    }
    preset.seed = static_cast<std::uint64_t>(seed);

    const std::pair<int, int> tileRadius = ReadIntPair(document, path, "tile_radius");
    if (tileRadius.first < 1 || tileRadius.first > kMaxTileRadius || tileRadius.second < 1 ||
        tileRadius.second > kMaxTileRadius) {
        throw std::runtime_error(path.string() + ": 字段 [tile_radius] 的每个分量必须落在 [1, " +
                                 std::to_string(kMaxTileRadius) + "]");
    }
    preset.tileRadiusX = tileRadius.first;
    preset.tileRadiusZ = tileRadius.second;

    // 地图覆盖的世界列范围（含共享边界列）。
    const int minColumnX = -preset.tileRadiusX * kTerrainTileSize;
    const int maxColumnX = preset.tileRadiusX * kTerrainTileSize;
    const int minColumnZ = -preset.tileRadiusZ * kTerrainTileSize;
    const int maxColumnZ = preset.tileRadiusZ * kTerrainTileSize;

    const std::pair<double, double> spawn = ReadNumberPair(document, path, "spawn");
    if (spawn.first < static_cast<double>(minColumnX) || spawn.first > static_cast<double>(maxColumnX) ||
        spawn.second < static_cast<double>(minColumnZ) || spawn.second > static_cast<double>(maxColumnZ)) {
        throw std::runtime_error(path.string() + ": 字段 [spawn] 必须落在地图范围内（列 X ∈ [" +
                                 std::to_string(minColumnX) + ", " + std::to_string(maxColumnX) + "]，列 Z ∈ [" +
                                 std::to_string(minColumnZ) + ", " + std::to_string(maxColumnZ) + "]）");
    }
    preset.spawnX = spawn.first;
    preset.spawnZ = spawn.second;

    if (const toml::array* edits = document["edit"].as_array(); edits != nullptr) {
        for (std::size_t index = 0; index < edits->size(); ++index) {
            const toml::table* entry = (*edits)[index].as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) + " 不是表");
            }

            MapEdit edit;
            edit.name = ReadString(*entry, path, "name");
            if (edit.name.empty()) {
                throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) +
                                         " 的 name 不能为空");
            }
            edit.mode = ParseMode(ReadString(*entry, path, "mode"), path, index);

            const std::pair<int, int> minCorner = ReadIntPair(*entry, path, "min");
            const std::pair<int, int> maxCorner = ReadIntPair(*entry, path, "max");
            edit.minX = minCorner.first;
            edit.minZ = minCorner.second;
            edit.maxX = maxCorner.first;
            edit.maxZ = maxCorner.second;

            if (edit.minX > edit.maxX || edit.minZ > edit.maxZ) {
                throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) + " (" + edit.name +
                                         ") 的 min 必须不大于 max");
            }
            if (edit.minX < minColumnX || edit.maxX > maxColumnX || edit.minZ < minColumnZ ||
                edit.maxZ > maxColumnZ) {
                throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) + " (" + edit.name +
                                         ") 的矩形超出地图范围");
            }

            const double height = ReadNumber(*entry, path, "height");
            if (height < 0.0 || height > kMaxHeightBlocks) {
                throw std::runtime_error(path.string() + ": [[edit]] #" + std::to_string(index) + " (" + edit.name +
                                         ") 的 height 必须落在 [0, " + std::to_string(kMaxTerrainHeightBlocks) + "]");
            }
            edit.heightUnits =
                static_cast<int>(std::lround(height * static_cast<double>(kHeightUnitsPerBlock)));

            preset.edits.push_back(std::move(edit));
        }
    }

    return preset;
}

void ApplyMapEditsToTile(const std::vector<MapEdit>& edits, TerrainTile& tile) noexcept {
    if (edits.empty()) {
        return;
    }

    for (int j = 0; j <= kTerrainTileSize; ++j) {
        const int worldZ = tile.WorldColumnZ(j);
        for (int i = 0; i <= kTerrainTileSize; ++i) {
            const int worldX = tile.WorldColumnX(i);

            int units = static_cast<int>(tile.At(i, j));
            for (const MapEdit& edit : edits) {
                if (worldX < edit.minX || worldX > edit.maxX || worldZ < edit.minZ || worldZ > edit.maxZ) {
                    continue;
                }
                switch (edit.mode) {
                    case MapEditMode::Flatten:
                        units = edit.heightUnits;
                        break;
                    case MapEditMode::Raise:
                        units += edit.heightUnits;
                        break;
                    case MapEditMode::Carve:
                        units -= edit.heightUnits;
                        break;
                }
            }

            units = std::clamp(units, kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
            tile.SetAt(i, j, static_cast<Height>(units));
        }
    }
}

}  // namespace vx
