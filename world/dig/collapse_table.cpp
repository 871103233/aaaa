#include "dig/collapse_table.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string DescribeField(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::optional<std::int64_t> ReadInt(const toml::table& table, const char* field) {
    return table[field].value<std::int64_t>();
}

[[nodiscard]] std::optional<double> ReadDouble(const toml::table& table, const char* field) {
    return table[field].value<double>();
}

/// 读一个"必须是数值且落在 [low, high]"的字段（缺失 / 类型错 / 越界都抛，口径同其余配置表）。
[[nodiscard]] double ReadDoubleInRange(const toml::table& table, const std::filesystem::path& path,
                                       const char* field, double low, double high) {
    const std::optional<double> value = ReadDouble(table, field);
    if (!value.has_value()) {
        throw std::runtime_error(DescribeField(path, field) + "缺失或不是数值");
    }
    if (!(*value >= low) || !(*value <= high)) {
        throw std::runtime_error(DescribeField(path, field) + "必须落在 [" + std::to_string(low) + ", " +
                                 std::to_string(high) + "]");
    }
    return *value;
}

/// 读一个"必须是整数且落在 [low, high]"的字段。
[[nodiscard]] std::int64_t ReadIntInRange(const toml::table& table, const std::filesystem::path& path,
                                          const char* field, std::int64_t low, std::int64_t high) {
    const std::optional<std::int64_t> value = ReadInt(table, field);
    if (!value.has_value()) {
        throw std::runtime_error(DescribeField(path, field) + "缺失或不是整数");
    }
    if (*value < low || *value > high) {
        throw std::runtime_error(DescribeField(path, field) + "必须落在 [" + std::to_string(low) + ", " +
                                 std::to_string(high) + "]");
    }
    return *value;
}

}  // namespace

CollapseTable CollapseTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载塌落规则表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = ReadInt(document, "schema_version");
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    CollapseTable table;
    table.m_schemaVersion = static_cast<int>(*schemaVersion);

    const std::optional<bool> enabled = document["enabled"].value<bool>();
    if (!enabled.has_value()) {
        throw std::runtime_error(DescribeField(path, "enabled") + "缺失或不是布尔值");
    }
    table.m_spec.enabled = *enabled;

    const std::optional<double> cantilever = document["max_cantilever_blocks"].value<double>();
    if (!cantilever.has_value()) {
        throw std::runtime_error(DescribeField(path, "max_cantilever_blocks") + "缺失或不是数值");
    }
    if (!(*cantilever >= 0.0) || *cantilever > static_cast<double>(kMaxCantileverLimit)) {
        throw std::runtime_error(DescribeField(path, "max_cantilever_blocks") + "必须落在 [0, " +
                                 std::to_string(static_cast<int>(kMaxCantileverLimit)) + "]");
    }
    table.m_spec.maxCantileverBlocks = static_cast<float>(*cantilever);

    const std::optional<std::int64_t> margin = ReadInt(document, "neighborhood_margin_blocks");
    if (!margin.has_value()) {
        throw std::runtime_error(DescribeField(path, "neighborhood_margin_blocks") + "缺失或不是整数");
    }
    if (*margin < 0 || *margin > 4) {
        throw std::runtime_error(DescribeField(path, "neighborhood_margin_blocks") + "必须落在 [0, 4]");
    }
    table.m_spec.neighborhoodMarginBlocks = static_cast<int>(*margin);

    // ---- T33 / T43：刚体化倒塌参数（质量 / 摩擦 / 弹性由材质表提供，见 ADR 0016）----
    table.m_spec.settleLinearSpeed =
        static_cast<float>(ReadDoubleInRange(document, path, "settle_linear_speed", 0.001, 1000.0));
    table.m_spec.settleAngularSpeed =
        static_cast<float>(ReadDoubleInRange(document, path, "settle_angular_speed", 0.001, 1000.0));
    table.m_spec.settleSteps = static_cast<int>(ReadIntInRange(document, path, "settle_steps", 1, 600));
    table.m_spec.initialTiltSpeed =
        static_cast<float>(ReadDoubleInRange(document, path, "initial_tilt_speed", 0.0, 20.0));
    table.m_spec.impulseSpeed = static_cast<float>(ReadDoubleInRange(document, path, "impulse_speed", 0.0, 100.0));
    table.m_spec.debrisDeleteMaxVoxels =
        static_cast<int>(ReadIntInRange(document, path, "debris_delete_max_voxels", 0, 4096));
    table.m_spec.maxActiveUnits = static_cast<int>(ReadIntInRange(document, path, "max_active_units", 1, 16));

    return table;
}

CollapseTable CollapseTable::Default() {
    // 取值与 assets/config/collapse.toml 一致，保证测试与运行期行为可比。
    return CollapseTable {};
}

}  // namespace vx
