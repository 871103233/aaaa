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

    const std::optional<std::int64_t> spread = ReadInt(document, "pile_spread_blocks");
    if (!spread.has_value()) {
        throw std::runtime_error(DescribeField(path, "pile_spread_blocks") + "缺失或不是整数");
    }
    if (*spread < 0 || *spread > 2) {
        throw std::runtime_error(DescribeField(path, "pile_spread_blocks") + "必须落在 [0, 2]");
    }
    table.m_spec.pileSpreadBlocks = static_cast<int>(*spread);

    const std::optional<std::int64_t> margin = ReadInt(document, "neighborhood_margin_blocks");
    if (!margin.has_value()) {
        throw std::runtime_error(DescribeField(path, "neighborhood_margin_blocks") + "缺失或不是整数");
    }
    if (*margin < 0 || *margin > 4) {
        throw std::runtime_error(DescribeField(path, "neighborhood_margin_blocks") + "必须落在 [0, 4]");
    }
    table.m_spec.neighborhoodMarginBlocks = static_cast<int>(*margin);

    return table;
}

CollapseTable CollapseTable::Default() {
    // 取值与 assets/config/collapse.toml 一致，保证测试与运行期行为可比。
    return CollapseTable {};
}

}  // namespace vx
