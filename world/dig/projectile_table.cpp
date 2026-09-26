#include "dig/projectile_table.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string DescribeField(const std::filesystem::path& path, const std::string& projectileId,
                                       const char* field) {
    return path.string() + ": 弹丸 [" + projectileId + "] 的字段 [" + field + "] ";
}

[[nodiscard]] float RequireFloat(const toml::table& table, const std::filesystem::path& path,
                                 const std::string& projectileId, const char* field) {
    const std::optional<double> value = table[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(DescribeField(path, projectileId, field) + "缺失或不是数值");
    }
    return static_cast<float>(*value);
}

/// 读一个三分量颜色（`emissive`）。
[[nodiscard]] std::array<float, 3> RequireFloat3(const toml::table& table, const std::filesystem::path& path,
                                                 const std::string& projectileId, const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != 3U) {
        throw std::runtime_error(DescribeField(path, projectileId, field) + "必须是 3 个数值的数组");
    }
    std::array<float, 3> values { 0.0F, 0.0F, 0.0F };
    for (std::size_t channel = 0; channel < values.size(); ++channel) {
        const std::optional<double> value = (*array)[channel].value<double>();
        if (!value.has_value()) {
            throw std::runtime_error(DescribeField(path, projectileId, field) + "的第 " + std::to_string(channel) +
                                     " 项不是数值");
        }
        values[channel] = static_cast<float>(*value);
    }
    return values;
}

}  // namespace

ProjectileTable ProjectileTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载弹丸表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    const std::optional<std::int64_t> maxActive = document["max_active"].value<std::int64_t>();
    if (!maxActive.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 max_active（同时存在的弹丸数上限）");
    }
    if (*maxActive < 1 || *maxActive > static_cast<std::int64_t>(kMaxActiveLimit)) {
        throw std::runtime_error(path.string() + ": 字段 [max_active] 必须落在 [1, " + std::to_string(kMaxActiveLimit) +
                                 "]（实际 " + std::to_string(*maxActive) + "）");
    }

    ProjectileTable table;
    table.m_schemaVersion = static_cast<int>(*schemaVersion);
    table.m_maxActive     = static_cast<int>(*maxActive);

    const toml::array* projectileArray = document["projectile"].as_array();
    if (projectileArray == nullptr || projectileArray->empty()) {
        throw std::runtime_error(path.string() + ": 至少要有一条 [[projectile]] 条目");
    }

    for (const toml::node& node : *projectileArray) {
        const toml::table* source = node.as_table();
        if (source == nullptr) {
            throw std::runtime_error(path.string() + ": [[projectile]] 数组项必须是表");
        }

        ProjectileSpec spec;
        const std::optional<std::string> id = (*source)["id"].value<std::string>();
        if (!id.has_value() || id->empty()) {
            throw std::runtime_error(path.string() + ": [[projectile]] 的字段 [id] 缺失或为空");
        }
        spec.id = *id;

        if (std::any_of(table.m_projectiles.begin(), table.m_projectiles.end(),
                        [&spec](const ProjectileSpec& other) { return other.id == spec.id; })) {
            throw std::runtime_error(path.string() + ": 弹丸 id 重复 [" + spec.id + "]");
        }

        spec.radius                = RequireFloat(*source, path, spec.id, "radius");
        spec.speed                 = RequireFloat(*source, path, spec.id, "speed");
        spec.gravityScale          = RequireFloat(*source, path, spec.id, "gravity_scale");
        spec.lifetimeSeconds       = RequireFloat(*source, path, spec.id, "lifetime");
        spec.fireIntervalSeconds   = RequireFloat(*source, path, spec.id, "fire_interval");
        spec.explosionRadiusBlocks = RequireFloat(*source, path, spec.id, "explosion_radius");
        spec.explosionDepthBlocks  = RequireFloat(*source, path, spec.id, "explosion_depth");
        spec.explosionRimBlocks    = RequireFloat(*source, path, spec.id, "explosion_rim");
        spec.explosionFalloff      = RequireFloat(*source, path, spec.id, "explosion_falloff");
        const std::array<float, 3> emissive = RequireFloat3(*source, path, spec.id, "emissive");
        for (std::size_t channel = 0; channel < emissive.size(); ++channel) {
            spec.emissiveRgb[channel] = emissive[channel];
        }

        if (!(spec.radius > 0.0F)) {
            throw std::runtime_error(DescribeField(path, spec.id, "radius") + "必须大于 0");
        }
        if (!(spec.speed > 0.0F)) {
            throw std::runtime_error(DescribeField(path, spec.id, "speed") + "必须大于 0");
        }
        if (spec.gravityScale < 0.0F) {
            throw std::runtime_error(DescribeField(path, spec.id, "gravity_scale") + "不能为负");
        }
        if (!(spec.lifetimeSeconds > 0.0F)) {
            throw std::runtime_error(DescribeField(path, spec.id, "lifetime") + "必须大于 0");
        }
        if (spec.fireIntervalSeconds < 0.0F) {
            throw std::runtime_error(DescribeField(path, spec.id, "fire_interval") + "不能为负");
        }
        if (!(spec.explosionRadiusBlocks > 0.0F)) {
            throw std::runtime_error(DescribeField(path, spec.id, "explosion_radius") + "必须大于 0");
        }
        if (!(spec.explosionDepthBlocks > 0.0F)) {
            throw std::runtime_error(DescribeField(path, spec.id, "explosion_depth") + "必须大于 0");
        }
        if (spec.explosionRimBlocks < 0.0F) {
            throw std::runtime_error(DescribeField(path, spec.id, "explosion_rim") + "不能为负");
        }
        if (!(spec.explosionFalloff > 0.0F) || spec.explosionFalloff > 1.0F) {
            throw std::runtime_error(DescribeField(path, spec.id, "explosion_falloff") + "必须落在 (0, 1]");
        }
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            if (spec.emissiveRgb[channel] < 0.0F) {
                throw std::runtime_error(DescribeField(path, spec.id, "emissive") + "的通道值不能为负");
            }
        }

        table.m_projectiles.push_back(spec);
    }

    return table;
}

ProjectileTable ProjectileTable::Default() {
    // 取值与 assets/config/projectiles.toml 一致，保证测试与运行期行为可比。
    ProjectileTable table;
    ProjectileSpec  spec;  // 其余字段取结构体默认值（与 projectiles.toml 一致）
    spec.id = "light_orb";
    table.m_projectiles.push_back(spec);
    return table;
}

}  // namespace vx
