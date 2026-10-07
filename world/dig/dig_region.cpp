#include "dig/dig_region.hpp"

#include "save/world_save.hpp"  // Fnv1a64Builder（世界定义内容哈希）
#include "terrain/terrain_types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

namespace vx {
namespace {

/// 向下取整的整数除法（负数也正确）：`floor(-33 / 32) = -2`。
[[nodiscard]] int FloorDiv(int value, int divisor) noexcept {
    const int quotient  = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? (quotient - 1) : quotient;
}

/// 世界坐标（格）→ 该坐标所在的块索引（**向外吸附**：向负方向取整）。
[[nodiscard]] int SnapToBlock(int coordinate) noexcept {
    return FloorDiv(coordinate, kVolumeBlockSize);
}

[[nodiscard]] std::string DescribeField(const std::filesystem::path& path, const std::string& regionName,
                                       const char* field) {
    return path.string() + ": 区域 [" + regionName + "] 的字段 [" + field + "] ";
}

[[nodiscard]] std::string DescribeRegion(const std::string& regionName, const char* field) {
    return "可挖区域 [" + regionName + "] 的字段 [" + field + "] ";
}

[[nodiscard]] int RequireInt(const toml::table& table, const std::filesystem::path& path, const std::string& regionName,
                             const char* field) {
    const std::optional<std::int64_t> value = table[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(DescribeField(path, regionName, field) + "缺失或不是整数");
    }
    if (*value < static_cast<std::int64_t>(INT32_MIN) || *value > static_cast<std::int64_t>(INT32_MAX)) {
        throw std::runtime_error(DescribeField(path, regionName, field) + "超出 int32 范围");
    }
    return static_cast<int>(*value);
}

/// 读一个三元素整数数组（`min` / `max`）。
[[nodiscard]] std::array<int, 3> RequireInt3(const toml::table& table, const std::filesystem::path& path,
                                             const std::string& regionName, const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != 3U) {
        throw std::runtime_error(DescribeField(path, regionName, field) + "必须是 3 个整数的数组");
    }
    std::array<int, 3> values { 0, 0, 0 };
    for (std::size_t axis = 0; axis < values.size(); ++axis) {
        const std::optional<std::int64_t> value = (*array)[axis].value<std::int64_t>();
        if (!value.has_value()) {
            throw std::runtime_error(DescribeField(path, regionName, field) + "的第 " + std::to_string(axis) +
                                     " 项不是整数");
        }
        if (*value < static_cast<std::int64_t>(INT32_MIN) || *value > static_cast<std::int64_t>(INT32_MAX)) {
            throw std::runtime_error(DescribeField(path, regionName, field) + "的第 " + std::to_string(axis) +
                                     " 项超出 int32 范围");
        }
        values[axis] = static_cast<int>(*value);
    }
    return values;
}

/// 世界垂直方向可容纳的块数（`0 ~ kMaxTerrainHeightBlocks` 格）。
[[nodiscard]] constexpr int VerticalBlockCount() noexcept {
    return kMaxTerrainHeightBlocks / kVolumeBlockSize;
}

/// 读**可选**的非负整数字段（缺失 ⇒ 0）；存在但非法即抛。
///
/// 用于 T59 / [ADR 0020](../../docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 的
/// 竖向带宽：**缺失 = 0 = 不裁剪**，与引入本字段之前的旧口径**逐位一致** ⇒ `schema_version` 无需提升。
[[nodiscard]] int OptionalNonNegativeInt(const toml::table& table, const std::filesystem::path& path,
                                        const char* field) {
    if (!table.contains(field)) {
        return 0;
    }
    const std::optional<std::int64_t> value = table[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(path.string() + ": 字段 [" + field + "] 存在但不是整数");
    }
    if (*value < 0) {
        throw std::runtime_error(path.string() + ": 字段 [" + field + "] 必须是非负整数（实际 " +
                                 std::to_string(*value) + "）");
    }
    return static_cast<int>(*value);
}

}  // namespace

void DigRegionTable::RebuildBlocks() {
    m_blocks.clear();
    for (const DigRegion& region : m_regions) {
        if (!region.diggable) {
            continue;  // sealed 只用于屏蔽低优先级区域，本身不产生块
        }
        for (int blockY = region.blockMin.y; blockY <= region.blockMax.y; ++blockY) {
            for (int blockZ = region.blockMin.z; blockZ <= region.blockMax.z; ++blockZ) {
                for (int blockX = region.blockMin.x; blockX <= region.blockMax.x; ++blockX) {
                    m_blocks.push_back(BlockCoord { blockX, blockY, blockZ });
                }
            }
        }
    }
    std::sort(m_blocks.begin(), m_blocks.end());
    m_blocks.erase(std::unique(m_blocks.begin(), m_blocks.end()), m_blocks.end());

    // 逐块再按**合并后的可挖判定**过滤：高优先级的 `sealed` 区域会把块从集合里剔掉
    // （以块中心点为代表点；块只有 32³ 体素，二者不会出现"半块可挖"的情形）。
    m_blocks.erase(std::remove_if(m_blocks.begin(), m_blocks.end(),
                                  [this](const BlockCoord& coord) {
                                      const double center = 0.5 * static_cast<double>(kVolumeBlockSize);
                                      return !IsDiggable(static_cast<double>(BlockOriginBlocks(coord.x)) + center,
                                                         static_cast<double>(BlockOriginBlocks(coord.y)) + center,
                                                         static_cast<double>(BlockOriginBlocks(coord.z)) + center);
                                  }),
                   m_blocks.end());

    if (m_blocks.size() > kMaxTotalBlocks) {
        throw std::runtime_error("可挖区域表的体积块数 " + std::to_string(m_blocks.size()) + " 超过上限 " +
                                 std::to_string(kMaxTotalBlocks) + "（每块 32³ 体素，见 ADR 0008）");
    }
}

DigRegionTable DigRegionTable::FromRegions(std::vector<DigRegion> regions, int bandDownBlocks, int bandUpBlocks) {
    DigRegionTable table;
    if (bandDownBlocks < 0 || bandDownBlocks > kMaxTerrainHeightBlocks) {
        throw std::runtime_error("可挖区域表的 band_down 必须落在 [0, " +
                                 std::to_string(kMaxTerrainHeightBlocks) + "] 格内（0 = 不裁剪）");
    }
    if (bandUpBlocks < 0 || bandUpBlocks > kMaxTerrainHeightBlocks) {
        throw std::runtime_error("可挖区域表的 band_up 必须落在 [0, " + std::to_string(kMaxTerrainHeightBlocks) +
                                 "] 格内（0 = 不裁剪）");
    }
    table.m_bandDown = bandDownBlocks;
    table.m_bandUp   = bandUpBlocks;
    // priority 升序；同优先级保持文件顺序（`stable_sort`）⇒ 解析时"后出现的覆盖先出现的"。
    std::stable_sort(regions.begin(), regions.end(), [](const DigRegion& left, const DigRegion& right) {
        return left.priority < right.priority;
    });
    table.m_regions = std::move(regions);

    for (const DigRegion& region : table.m_regions) {
        if (region.blockMin.y < 0 || region.blockMax.y >= VerticalBlockCount()) {
            throw std::runtime_error(DescribeRegion(region.name, "min/max") + "垂直范围必须落在世界高度范围 0 ~ " +
                                     std::to_string(kMaxTerrainHeightBlocks) + " 格内（按 32 格块对齐后可覆盖该范围）");
        }
    }

    table.RebuildBlocks();
    return table;
}

DigRegionTable DigRegionTable::Default() {
    return DigRegionTable {};
}

DigRegionTable DigRegionTable::LoadFromFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        // ADR 0006：数据文件缺失时不报错（只用程序化规则）。本轮程序化规则未实现 ⇒ 空表 = 无可挖区域。
        return Default();
    }

    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载可挖区域表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    std::vector<DigRegion> regions;
    const toml::array*     regionArray = document["region"].as_array();
    if (regionArray != nullptr) {
        for (const toml::node& node : *regionArray) {
            const toml::table* table = node.as_table();
            if (table == nullptr) {
                throw std::runtime_error(path.string() + ": [[region]] 数组项必须是表");
            }

            DigRegion region;
            const std::optional<std::string> name = (*table)["name"].value<std::string>();
            if (!name.has_value() || name->empty()) {
                throw std::runtime_error(path.string() + ": [[region]] 的字段 [name] 缺失或为空");
            }
            region.name = *name;

            const std::optional<std::string> mode = (*table)["mode"].value<std::string>();
            if (!mode.has_value()) {
                throw std::runtime_error(DescribeField(path, region.name, "mode") + "缺失");
            }
            if (*mode == "diggable") {
                region.diggable = true;
            } else if (*mode == "sealed") {
                region.diggable = false;
            } else {
                throw std::runtime_error(DescribeField(path, region.name, "mode") + "必须是 diggable 或 sealed（实际 \"" +
                                         *mode + "\"）");
            }

            region.priority = RequireInt(*table, path, region.name, "priority");

            const std::array<int, 3> minimum = RequireInt3(*table, path, region.name, "min");
            const std::array<int, 3> maximum = RequireInt3(*table, path, region.name, "max");
            for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
                if (minimum[axis] > maximum[axis]) {
                    throw std::runtime_error(DescribeField(path, region.name, "min") +
                                             "必须是闭区间下界（min 不得大于 max）");
                }
            }

            // 垂直范围必须在世界高度范围内（块对齐后的校验在 `FromRegions` 里统一做）。
            if (minimum[1] < 0 || maximum[1] > kMaxTerrainHeightBlocks) {
                throw std::runtime_error(DescribeField(path, region.name, "min/max") +
                                         "垂直范围必须落在世界高度范围 0 ~ " + std::to_string(kMaxTerrainHeightBlocks) +
                                         " 格内");
            }

            region.blockMin = BlockCoord { SnapToBlock(minimum[0]), SnapToBlock(minimum[1]), SnapToBlock(minimum[2]) };
            region.blockMax = BlockCoord { SnapToBlock(maximum[0]), SnapToBlock(maximum[1]), SnapToBlock(maximum[2]) };
            regions.push_back(region);
        }
    }

    // T59 / ADR 0020 决策一：**竖向带宽**（可选字段；缺失 = 0 = 不裁剪 ⇒ 旧文件照旧可用）。
    const int bandDown = OptionalNonNegativeInt(document, path, "band_down");
    const int bandUp   = OptionalNonNegativeInt(document, path, "band_up");

    return FromRegions(std::move(regions), bandDown, bandUp);
}

bool DigRegionTable::IsDiggable(double x, double y, double z) const noexcept {
    // 从最高优先级往低走，取第一个命中的区域；同优先级时文件顺序在后者优先（`stable_sort` + 反向遍历）。
    for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it) {
        if (it->Contains(x, y, z)) {
            return it->diggable;
        }
    }
    return false;
}

bool DigRegionTable::SkipQuad(const TerrainQuad& quad) const {
    for (std::size_t corner = 0; corner < 4; ++corner) {
        if (!IsDiggable(static_cast<double>(quad.columnX[corner]), static_cast<double>(quad.height[corner]),
                        static_cast<double>(quad.columnZ[corner]))) {
            return false;  // 只要有一角不在可挖区域内，该四边形仍由地表网格绘制
        }
    }
    return true;
}

std::uint64_t DigRegionContentHash(const DigRegionTable& table) noexcept {
    Fnv1a64Builder builder;
    builder.Bytes("DGRG", 4);  // 域标签（与地形参数内容哈希区分，避免跨类型碰撞）

    builder.I32(table.BandDownBlocks());
    builder.I32(table.BandUpBlocks());
    builder.U32(static_cast<std::uint32_t>(table.Regions().size()));
    for (const DigRegion& region : table.Regions()) {
        // 名称带**长度前缀**（否则 ("ab","c") 与 ("a","bc") 编码后无法区分）。
        builder.U32(static_cast<std::uint32_t>(region.name.size()));
        builder.Bytes(region.name.data(), region.name.size());
        builder.Bool(region.diggable);
        builder.I32(region.priority);
        builder.I32(region.blockMin.x);
        builder.I32(region.blockMin.y);
        builder.I32(region.blockMin.z);
        builder.I32(region.blockMax.x);
        builder.I32(region.blockMax.y);
        builder.I32(region.blockMax.z);
    }
    // 注意：**不含 `schema_version`** —— 它是存档头部的独立字段（`digRegionSchemaVersion`）。
    return builder.Value();
}

}  // namespace vx
