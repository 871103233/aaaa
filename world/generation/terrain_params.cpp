// 地表生成参数（含地貌分区）：加载与校验 + 两个纯函数（W3）。
//
// 口径与其它配置表一致（ADR 0005）：TOML + toml++，**启动期一次性加载**；
// 文件缺失 / 语法错 / 字段缺失 / 取值非法**一律抛异常**，禁止静默回退。

#include "generation/terrain_params.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace vx {
namespace {

constexpr int kSchemaVersion = 1;

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] double ReadNumber(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const toml::node* node = table.get(field);
    if (node == nullptr) {
        throw std::runtime_error(Describe(path, field) + "缺失");
    }
    if (const std::optional<double> value = node->value<double>(); value.has_value()) {
        return *value;
    }
    if (const std::optional<std::int64_t> value = node->value<std::int64_t>(); value.has_value()) {
        return static_cast<double>(*value);
    }
    throw std::runtime_error(Describe(path, field) + "必须是数值");
}

[[nodiscard]] std::int64_t ReadInt(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const toml::node* node = table.get(field);
    if (node == nullptr) {
        throw std::runtime_error(Describe(path, field) + "缺失");
    }
    if (const std::optional<std::int64_t> value = node->value<std::int64_t>(); value.has_value()) {
        return *value;
    }
    throw std::runtime_error(Describe(path, field) + "必须是整数");
}

[[nodiscard]] bool ReadBool(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const toml::node* node = table.get(field);
    if (node == nullptr) {
        throw std::runtime_error(Describe(path, field) + "缺失");
    }
    if (const std::optional<bool> value = node->value<bool>(); value.has_value()) {
        return *value;
    }
    throw std::runtime_error(Describe(path, field) + "必须是布尔值");
}

[[nodiscard]] float ReadPositiveNumber(const toml::table& table, const std::filesystem::path& path,
                                       const char* field) {
    const double value = ReadNumber(table, path, field);
    if (!(value > 0.0)) {
        throw std::runtime_error(Describe(path, field) + "必须为正数（当前 " + std::to_string(value) + "）");
    }
    return static_cast<float>(value);
}

[[nodiscard]] float ReadNonNegativeNumber(const toml::table& table, const std::filesystem::path& path,
                                          const char* field) {
    const double value = ReadNumber(table, path, field);
    if (!(value >= 0.0)) {
        throw std::runtime_error(Describe(path, field) + "必须非负（当前 " + std::to_string(value) + "）");
    }
    return static_cast<float>(value);
}

/// 平滑插值权重：`t` 钳到 `[0,1]` 后做 smoothstep。
[[nodiscard]] float SmoothStep01(float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/// 在"平原值 → 丘陵值 → 山川值"之间按掩罩取值做分段 smoothstep 插值（连续、单调）。
[[nodiscard]] float PiecewiseLerp(float mask, const TerrainLandformParams& params, float plainsValue,
                                  float hillsValue, float mountainsValue) noexcept {
    const float hillsStart     = params.hillsStart;
    const float mountainsStart = params.mountainsStart;
    const float blend          = params.blend;

    if (mask <= hillsStart) {
        return plainsValue;
    }
    if (mask <= hillsStart + blend) {
        const float t = SmoothStep01((mask - hillsStart) / blend);
        return plainsValue + (hillsValue - plainsValue) * t;
    }
    if (mask <= mountainsStart) {
        return hillsValue;
    }
    const float t = SmoothStep01((mask - mountainsStart) / blend);
    return hillsValue + (mountainsValue - hillsValue) * t;
}

}  // namespace

LandformKind ClassifyLandform(float mask, const TerrainLandformParams& params) noexcept {
    if (!params.enabled) {
        return LandformKind::Hills;  // 未启用时无分区语义（仅供统计 / 内容）
    }
    const float halfBlend = params.blend * 0.5F;
    if (mask < params.hillsStart + halfBlend) {
        return LandformKind::Plains;
    }
    if (mask < params.mountainsStart + halfBlend) {
        return LandformKind::Hills;
    }
    return LandformKind::Mountains;
}

LandformModulation EvaluateLandformModulation(float mask, const TerrainLandformParams& params) noexcept {
    if (!params.enabled) {
        return LandformModulation { 1.0F, 0.0F };  // ⇒ 与引入本层之前逐位一致
    }
    LandformModulation modulation;
    modulation.amplitudeScale = PiecewiseLerp(mask, params, params.plainsAmplitudeScale, params.hillsAmplitudeScale,
                                              params.mountainsAmplitudeScale);
    modulation.offsetBlocks   = PiecewiseLerp(mask, params, params.plainsOffsetBlocks, params.hillsOffsetBlocks,
                                              params.mountainsOffsetBlocks);
    return modulation;
}

TerrainGenerationParams TerrainGenerationParams::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载地表生成参数 " + path.string() + ": " + error.what());
    }

    const std::int64_t schemaVersion = ReadInt(document, path, "schema_version");
    if (schemaVersion != kSchemaVersion) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(schemaVersion) + "）");
    }

    TerrainGenerationParams params;
    params.baseFrequency      = ReadPositiveNumber(document, path, "base_frequency");
    params.detailFrequency    = ReadPositiveNumber(document, path, "detail_frequency");
    params.roughFrequency     = ReadPositiveNumber(document, path, "rough_frequency");
    params.baseAmplitude      = ReadNonNegativeNumber(document, path, "base_amplitude");
    params.detailAmplitude    = ReadNonNegativeNumber(document, path, "detail_amplitude");
    params.roughAmplitude     = ReadNonNegativeNumber(document, path, "rough_amplitude");
    params.heightOffsetBlocks = ReadNonNegativeNumber(document, path, "height_offset_blocks");
    if (params.heightOffsetBlocks > 512.0F) {
        throw std::runtime_error(Describe(path, "height_offset_blocks") + "超出世界垂直范围 0~512 格");
    }
    params.variationFrequency = ReadPositiveNumber(document, path, "variation_frequency");

    // [landform] 段（本阶段 W3 的新特性；本文件**必填**）。
    const toml::table* landform = document["landform"].as_table();
    if (landform == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [landform] 段");
    }
    const toml::table& lf = *landform;

    TerrainLandformParams& landformParams       = params.landform;
    landformParams.enabled                      = ReadBool(lf, path, "enabled");
    landformParams.frequency                    = ReadPositiveNumber(lf, path, "frequency");
    landformParams.hillsStart                   = static_cast<float>(ReadNumber(lf, path, "hills_start"));
    landformParams.mountainsStart               = static_cast<float>(ReadNumber(lf, path, "mountains_start"));
    landformParams.blend                        = static_cast<float>(ReadNumber(lf, path, "blend"));
    landformParams.plainsAmplitudeScale         = ReadPositiveNumber(lf, path, "plains_amplitude_scale");
    landformParams.hillsAmplitudeScale          = ReadPositiveNumber(lf, path, "hills_amplitude_scale");
    landformParams.mountainsAmplitudeScale      = ReadPositiveNumber(lf, path, "mountains_amplitude_scale");
    landformParams.plainsOffsetBlocks           = static_cast<float>(ReadNumber(lf, path, "plains_offset_blocks"));
    landformParams.hillsOffsetBlocks            = static_cast<float>(ReadNumber(lf, path, "hills_offset_blocks"));
    landformParams.mountainsOffsetBlocks        = static_cast<float>(ReadNumber(lf, path, "mountains_offset_blocks"));

    const std::int64_t seedChannel = ReadInt(lf, path, "seed_channel");
    if (seedChannel <= 0) {
        throw std::runtime_error(Describe(path, "seed_channel") + "必须为正整数");
    }
    landformParams.seedChannel = static_cast<std::uint64_t>(seedChannel);

    // 分档与过渡的合法性（见头文件的前置条件）。
    if (!(landformParams.hillsStart >= 0.0F) || !(landformParams.mountainsStart <= 1.0F) ||
        landformParams.hillsStart > landformParams.mountainsStart) {
        throw std::runtime_error(Describe(path, "hills_start/mountains_start") +
                                 "必须满足 0 ≤ hills_start ≤ mountains_start ≤ 1");
    }
    if (!(landformParams.blend >= 0.0F) ||
        landformParams.hillsStart + landformParams.blend > landformParams.mountainsStart) {
        throw std::runtime_error(Describe(path, "blend") +
                                 "必须满足 blend ≥ 0 且 hills_start + blend ≤ mountains_start");
    }

    // [overhang] 段（W4：地表体积壳使用的 3D 噪声；本文件**必填**）。
    const toml::table* overhang = document["overhang"].as_table();
    if (overhang == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [overhang] 段");
    }
    const toml::table& oh = *overhang;
    params.overhang.frequency       = ReadPositiveNumber(oh, path, "frequency");
    params.overhang.amplitudeBlocks = ReadNonNegativeNumber(oh, path, "amplitude_blocks");
    const std::int64_t overhangChannel = ReadInt(oh, path, "seed_channel");
    if (overhangChannel <= 0) {
        throw std::runtime_error(Describe(path, "seed_channel") + "（[overhang]）必须为正整数");
    }
    params.overhang.seedChannel = static_cast<std::uint64_t>(overhangChannel);

    // [caves] 段（W5：地表壳的洞穴隧道网络；本文件**必填**）。
    const toml::table* caves = document["caves"].as_table();
    if (caves == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [caves] 段");
    }
    const toml::table& cv = *caves;
    params.caves.enabled             = ReadBool(cv, path, "enabled");
    params.caves.frequency           = ReadPositiveNumber(cv, path, "frequency");
    params.caves.tunnelRadius        = ReadPositiveNumber(cv, path, "tunnel_radius");
    params.caves.carveStrengthBlocks = ReadPositiveNumber(cv, path, "carve_strength_blocks");
    params.caves.depthFadeBlocks     = ReadNonNegativeNumber(cv, path, "depth_fade_blocks");
    if (params.caves.tunnelRadius > 1.0F) {
        throw std::runtime_error(Describe(path, "tunnel_radius") + "必须落在 (0, 1]（噪声值域口径）");
    }
    const std::int64_t caveChannel = ReadInt(cv, path, "seed_channel");
    if (caveChannel <= 0) {
        throw std::runtime_error(Describe(path, "seed_channel") + "（[caves]）必须为正整数");
    }
    params.caves.seedChannel = static_cast<std::uint64_t>(caveChannel);

    // [river] 段（W6：地表壳的河道下切与水面；本文件**必填**）。
    const toml::table* river = document["river"].as_table();
    if (river == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [river] 段");
    }
    const toml::table& rv = *river;
    params.river.enabled                = ReadBool(rv, path, "enabled");
    params.river.stepBlocks             = ReadPositiveNumber(rv, path, "step_blocks");
    params.river.channelDepthBlocks     = ReadPositiveNumber(rv, path, "channel_depth_blocks");
    params.river.channelHalfWidthBlocks = ReadPositiveNumber(rv, path, "channel_half_width_blocks");
    params.river.bankHalfWidthBlocks    = ReadNonNegativeNumber(rv, path, "bank_half_width_blocks");
    params.river.waterDepthBlocks       = ReadPositiveNumber(rv, path, "water_depth_blocks");
    params.river.jitterRadians          = ReadNonNegativeNumber(rv, path, "jitter_radians");
    params.river.jitterFrequency        = ReadPositiveNumber(rv, path, "jitter_frequency");
    const std::int64_t maxNodes         = ReadInt(rv, path, "max_nodes");
    if (maxNodes <= 0) {
        throw std::runtime_error(Describe(path, "max_nodes") + "（[river]）必须为正整数");
    }
    params.river.maxNodes = static_cast<int>(maxNodes);
    // 水必须在河床之内（否则"水漫过河岸"⇒ 与地形不自洽，见 ADR 0027「水在低处」）。
    if (params.river.waterDepthBlocks > params.river.channelDepthBlocks) {
        throw std::runtime_error(Describe(path, "water_depth_blocks") +
                                 "（[river]）不得大于 channel_depth_blocks（水必须在河床之内）");
    }
    const std::int64_t riverChannel = ReadInt(rv, path, "seed_channel");
    if (riverChannel <= 0) {
        throw std::runtime_error(Describe(path, "seed_channel") + "（[river]）必须为正整数");
    }
    params.river.seedChannel = static_cast<std::uint64_t>(riverChannel);

    return params;
}

}  // namespace vx
