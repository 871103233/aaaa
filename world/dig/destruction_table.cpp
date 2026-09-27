#include "dig/destruction_table.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

/// 读一个"必须是数值且 > 0"的字段（缺失 / 类型错 / 越界都抛，口径同其余配置表）。
[[nodiscard]] float RequirePositive(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<double> value = table[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是数值");
    }
    if (!(*value > 0.0)) {
        throw std::runtime_error(Describe(path, field) + "必须大于 0");
    }
    return static_cast<float>(*value);
}

/// 读一个"必须是数值且 ≥ 0"的字段。
[[nodiscard]] float RequireNonNegative(const toml::table& table, const std::filesystem::path& path,
                                       const char* field) {
    const std::optional<double> value = table[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是数值");
    }
    if (*value < 0.0) {
        throw std::runtime_error(Describe(path, field) + "不能为负");
    }
    return static_cast<float>(*value);
}

/// 读一个"必须是三个 [0,1] 数值"的颜色字段。
[[nodiscard]] std::array<float, 3> RequireTint3(const toml::table& table, const std::filesystem::path& path,
                                                const char* field) {
    const toml::array* values = table[field].as_array();
    if (values == nullptr || values->size() != 3U) {
        throw std::runtime_error(Describe(path, field) + "必须是有 3 个元素的数组");
    }
    std::array<float, 3> out { 0.0F, 0.0F, 0.0F };
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        const std::optional<double> value = (*values)[channel].value<double>();
        if (!value.has_value()) {
            throw std::runtime_error(Describe(path, field) + "的第 " + std::to_string(channel) + " 个元素不是数值");
        }
        if (*value < 0.0 || *value > 1.0) {
            throw std::runtime_error(Describe(path, field) + "的每个通道必须落在 [0, 1]");
        }
        out[channel] = static_cast<float>(*value);
    }
    return out;
}

}  // namespace

DestructionTable DestructionTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载破坏表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    DestructionTable table;
    table.m_schemaVersion             = static_cast<int>(*schemaVersion);
    table.m_spec.pointsPerCubicBlock  = RequirePositive(document, path, "points_per_cubic_block");
    table.m_spec.propDamageThreshold  = RequireNonNegative(document, path, "prop_damage_threshold");
    const std::array<float, 3> tint   = RequireTint3(document, path, "prop_broken_tint");
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        table.m_spec.propBrokenTint[channel] = tint[channel];
    }
    return table;
}

DestructionTable DestructionTable::Default() {
    // 取值与 assets/config/destruction.toml 一致，保证测试与运行期行为可比。
    DestructionTable table;
    return table;
}

}  // namespace vx
