#include "dig/terrain_brush.hpp"

#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace vx {
namespace {

/// 平滑阶跃：`value <= edge0` → 0、`value >= edge1` → 1，中间三次曲线过渡。
/// 与 `world/terrain/material_blender.cpp` 的 `SmoothStep` 同公式（同风格，非同一处复用，避免跨模块耦合）。
[[nodiscard]] float SmoothStep(float edge0, float edge1, float value) noexcept {
    if (edge1 <= edge0) {
        return (value >= edge1) ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/// 爆破坑**外环隆起**剖面：参数 `t = d / craterRadius`。
///
/// 中心（t = 0）与边界（t = 1）均为 0，且两端一阶导为 0（smoothstep 的两端导数归零），
/// 故与"坑体收口"和"半径外不变"都平滑衔接；峰值位于 `t ∈ [0.6, 0.72]`（即 d ≈ 0.6~0.7·R，
/// 靠近坑沿），模拟爆炸抛出的土堆。过渡带刻意取宽（升 0.3R、降 0.28R），
/// 使剖面二阶差分很小（无硬台阶），而不是在坑沿堆一条窄脊。
[[nodiscard]] float CraterRimProfile(float t) noexcept {
    return SmoothStep(0.30F, 0.60F, t) * (1.0F - SmoothStep(0.72F, 1.0F, t));
}

/// 圆盘的整数包围盒（世界列，闭区间），已按半径外扩。
struct DiscBounds {
    int minX = 0;
    int maxX = -1;
    int minZ = 0;
    int maxZ = -1;
};

[[nodiscard]] DiscBounds MakeDiscBounds(double centerX, double centerZ, double radius) noexcept {
    DiscBounds bounds;
    bounds.minX = static_cast<int>(std::floor(centerX - radius));
    bounds.maxX = static_cast<int>(std::ceil(centerX + radius));
    bounds.minZ = static_cast<int>(std::floor(centerZ - radius));
    bounds.maxZ = static_cast<int>(std::ceil(centerZ + radius));
    return bounds;
}

/// 去重并升序排列脏 tile（各笔刷共用）。
void FinalizeDirtyTiles(BrushResult& result) {
    std::sort(result.dirtyTiles.begin(), result.dirtyTiles.end());
    result.dirtyTiles.erase(std::unique(result.dirtyTiles.begin(), result.dirtyTiles.end()), result.dirtyTiles.end());
}

}  // namespace

BrushResult ApplyTerrainBrush(TerrainWorld& world, const BrushPose& brush, int deltaHeightUnits) {
    BrushResult result;
    if (brush.radius <= 0.0F) {
        return result;
    }

    const double centerX     = static_cast<double>(brush.centerX);
    const double centerZ     = static_cast<double>(brush.centerZ);
    const double radius      = static_cast<double>(brush.radius);
    const double radiusSq    = radius * radius;

    const int minX = static_cast<int>(std::floor(centerX - radius));
    const int maxX = static_cast<int>(std::ceil(centerX + radius));
    const int minZ = static_cast<int>(std::floor(centerZ - radius));
    const int maxZ = static_cast<int>(std::ceil(centerZ + radius));

    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            const double dx = static_cast<double>(x) - centerX;
            const double dz = static_cast<double>(z) - centerZ;
            if (dx * dx + dz * dz > radiusSq) {
                continue;  // 圆盘外：一律不动
            }

            Height current = 0;
            if (!world.ReadColumnHeight(x, z, current)) {
                continue;  // 该列未加载：按不存在处理
            }

            const int target = std::clamp(static_cast<int>(current) + deltaHeightUnits, kMinTerrainHeightUnits,
                                          kMaxTerrainHeightUnits);
            if (target == static_cast<int>(current)) {
                continue;  // 钳制后无变化：不计数，也不弄脏 tile
            }

            world.WriteColumnHeight(x, z, static_cast<Height>(target), result.dirtyTiles);
            ++result.changedColumns;
        }
    }

    FinalizeDirtyTiles(result);
    return result;
}

float BrushFalloff(float distance, float radius, float band) noexcept {
    if (!(radius > 0.0F)) {
        return 0.0F;
    }
    if (band <= 0.0F) {
        return (distance < radius) ? 1.0F : 0.0F;
    }
    // 内沿 r·(1−band) 处为满强度，到 r 用 smoothstep 平滑归零（两端一阶导为 0 ⇒ 边界一阶连续）。
    const float inner = radius * (1.0F - std::clamp(band, 0.0F, 1.0F));
    return 1.0F - SmoothStep(inner, radius, distance);
}

BrushResult ApplyTerrainLevel(TerrainWorld& world, const BrushPose& brush, float targetHeightBlocks,
                              float maxStepBlocks, float falloffBand, LevelMode mode) {
    BrushResult result;
    if (brush.radius <= 0.0F) {
        return result;
    }

    const double centerX  = static_cast<double>(brush.centerX);
    const double centerZ  = static_cast<double>(brush.centerZ);
    const double radius   = static_cast<double>(brush.radius);
    const double radiusSq = radius * radius;
    const DiscBounds bounds = MakeDiscBounds(centerX, centerZ, radius);

    // 目标高度换算为定点单位并钳制到世界垂直范围（与写入口径一致）。
    const int targetUnits = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(targetHeightBlocks) * kHeightUnitsPerBlock)),
        kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
    // 本次最大改动（格 → 单位）；负值按 0 处理。
    const float maxStepUnits = std::max(0.0F, maxStepBlocks) * static_cast<float>(kHeightUnitsPerBlock);

    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int x = bounds.minX; x <= bounds.maxX; ++x) {
            const double dx = static_cast<double>(x) - centerX;
            const double dz = static_cast<double>(z) - centerZ;
            const double distanceSq = dx * dx + dz * dz;
            if (distanceSq > radiusSq) {
                continue;  // 圆盘外：一律不动
            }

            Height current = 0;
            if (!world.ReadColumnHeight(x, z, current)) {
                continue;
            }

            const int difference = targetUnits - static_cast<int>(current);
            // 单向：Fill 只抬升低于目标处、Shave 只削低高于目标处（另一侧不动）。
            if ((mode == LevelMode::Fill && difference <= 0) || (mode == LevelMode::Shave && difference >= 0)) {
                continue;
            }

            const float falloff = BrushFalloff(static_cast<float>(std::sqrt(distanceSq)), brush.radius, falloffBand);
            float magnitude = static_cast<float>(std::abs(difference)) * falloff;
            magnitude = std::min(magnitude, maxStepUnits);
            int stepUnits = static_cast<int>(std::lround(magnitude));
            if (stepUnits == 0) {
                stepUnits = 1;  // 保证收敛：衰减后不足 1 单位时仍推进 1 单位
            }
            stepUnits = std::min(stepUnits, std::abs(difference));  // 不过冲目标

            const int newUnits =
                std::clamp(static_cast<int>(current) + ((difference > 0) ? stepUnits : -stepUnits),
                           kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
            if (newUnits == static_cast<int>(current)) {
                continue;
            }
            world.WriteColumnHeight(x, z, static_cast<Height>(newUnits), result.dirtyTiles);
            ++result.changedColumns;
        }
    }

    FinalizeDirtyTiles(result);
    return result;
}

BrushResult ApplyTerrainCrater(TerrainWorld& world, const BrushPose& brush, float depthBlocks, float rimBlocks,
                               float craterRadiusBlocks, float falloffBand) {
    BrushResult result;
    if (!(craterRadiusBlocks > 0.0F)) {
        return result;
    }

    const double centerX  = static_cast<double>(brush.centerX);
    const double centerZ  = static_cast<double>(brush.centerZ);
    const double radius   = static_cast<double>(craterRadiusBlocks);
    const double radiusSq = radius * radius;
    const DiscBounds bounds = MakeDiscBounds(centerX, centerZ, radius);

    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int x = bounds.minX; x <= bounds.maxX; ++x) {
            const double dx = static_cast<double>(x) - centerX;
            const double dz = static_cast<double>(z) - centerZ;
            const double distanceSq = dx * dx + dz * dz;
            if (distanceSq > radiusSq) {
                continue;  // 圆盘外：一律不动（边界外侧逐列不变）
            }

            Height current = 0;
            if (!world.ReadColumnHeight(x, z, current)) {
                continue;
            }

            const float distance = static_cast<float>(std::sqrt(distanceSq));
            const float t        = distance / craterRadiusBlocks;
            // 坑体下挖（中心最深、边界归零）+ 外环隆起（中心/边界均归零，峰值靠近坑沿）。
            const float deltaBlocks =
                rimBlocks * CraterRimProfile(t) - depthBlocks * BrushFalloff(distance, craterRadiusBlocks, falloffBand);
            const int deltaUnits = static_cast<int>(std::lround(static_cast<double>(deltaBlocks) *
                                                                static_cast<double>(kHeightUnitsPerBlock)));
            if (deltaUnits == 0) {
                continue;
            }

            const int newUnits =
                std::clamp(static_cast<int>(current) + deltaUnits, kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
            if (newUnits == static_cast<int>(current)) {
                continue;
            }
            world.WriteColumnHeight(x, z, static_cast<Height>(newUnits), result.dirtyTiles);
            ++result.changedColumns;
        }
    }

    FinalizeDirtyTiles(result);
    return result;
}

namespace {

[[nodiscard]] std::string DescribeBrushField(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] float ReadBrushFloat(const toml::table& document, const std::filesystem::path& path, const char* field) {
    const std::optional<double> value = document[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(DescribeBrushField(path, field) + "缺失或不是数值");
    }
    return static_cast<float>(*value);
}

}  // namespace

BrushTable BrushTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载笔刷表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    BrushTable table;
    table.m_schemaVersion = static_cast<int>(*schemaVersion);

    // 全部字段必填（此表没有可选字段）。数值口径：半径 / 深度 / 隆起 / 坑半径 = 格；strength = 格/秒。
    table.m_settings.radius       = ReadBrushFloat(document, path, "radius");
    table.m_settings.strength     = ReadBrushFloat(document, path, "strength");
    table.m_settings.falloff      = ReadBrushFloat(document, path, "falloff");
    table.m_settings.craterDepth  = ReadBrushFloat(document, path, "crater_depth");
    table.m_settings.craterRim    = ReadBrushFloat(document, path, "crater_rim");
    table.m_settings.craterRadius = ReadBrushFloat(document, path, "crater_radius");

    if (!(table.m_settings.radius > 0.0F)) {
        throw std::runtime_error(DescribeBrushField(path, "radius") + "必须大于 0");
    }
    if (!(table.m_settings.strength > 0.0F)) {
        throw std::runtime_error(DescribeBrushField(path, "strength") + "必须大于 0");
    }
    if (!(table.m_settings.falloff > 0.0F) || table.m_settings.falloff > 1.0F) {
        throw std::runtime_error(DescribeBrushField(path, "falloff") + "必须落在 (0, 1]");
    }
    if (!(table.m_settings.craterDepth > 0.0F)) {
        throw std::runtime_error(DescribeBrushField(path, "crater_depth") + "必须大于 0");
    }
    if (table.m_settings.craterRim < 0.0F) {
        throw std::runtime_error(DescribeBrushField(path, "crater_rim") + "不能为负");
    }
    if (!(table.m_settings.craterRadius > 0.0F)) {
        throw std::runtime_error(DescribeBrushField(path, "crater_radius") + "必须大于 0");
    }

    return table;
}

BrushTable BrushTable::Default() {
    // 取值与 assets/config/brush.toml 一致，保证测试与运行期行为可比。
    return BrushTable {};
}

}  // namespace vx
