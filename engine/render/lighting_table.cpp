#include "render/lighting_table.hpp"

#include "render/shadow_cascade.hpp"  // 仅为 kMaxShadowCascades（级联数上限，解析时校验）

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* section, const char* field) {
    return path.string() + ": 段 [" + section + "] 字段 [" + field + "] ";
}

[[nodiscard]] const toml::table& RequireTable(const toml::table& document, const std::filesystem::path& path,
                                              const char* section) {
    const toml::table* table = document[section].as_table();
    if (table == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [" + section + "] 段");
    }
    return *table;
}

[[nodiscard]] float ReadFloat(const toml::table& section, const std::filesystem::path& path, const char* sectionName,
                              const char* field) {
    const std::optional<double> value = section[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是数值");
    }
    return static_cast<float>(*value);
}

[[nodiscard]] bool ReadBool(const toml::table& section, const std::filesystem::path& path, const char* sectionName,
                            const char* field) {
    const std::optional<bool> value = section[field].value<bool>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是布尔值");
    }
    return *value;
}

/// 读取整数（级数 / 分辨率等计数与尺寸字段用）。
[[nodiscard]] int ReadInt(const toml::table& section, const std::filesystem::path& path, const char* sectionName,
                          const char* field) {
    const std::optional<std::int64_t> value = section[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是整数");
    }
    return static_cast<int>(*value);
}

/// 读取长度必须为 3 的数值数组（方向 / 颜色都用它）。
[[nodiscard]] std::array<float, 3> ReadVec3(const toml::table& section, const std::filesystem::path& path,
                                            const char* sectionName, const char* field) {
    const toml::array* array = section[field].as_array();
    if (array == nullptr || array->size() != 3) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是长度 3 的数组");
    }
    std::array<float, 3> out {};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::optional<double> value = (*array)[i].value<double>();
        if (!value.has_value()) {
            throw std::runtime_error(Describe(path, sectionName, field) + "的元素必须都是数值");
        }
        out[i] = static_cast<float>(*value);
    }
    return out;
}

/// 读取颜色：三个分量都必须落在 [0, 1]，越界即报错（禁止静默钳制）。
[[nodiscard]] ColorRgb ReadColor(const toml::table& section, const std::filesystem::path& path, const char* sectionName,
                                 const char* field) {
    const ColorRgb color = ReadVec3(section, path, sectionName, field);
    for (const float component : color) {
        if (component < 0.0F || component > 1.0F) {
            throw std::runtime_error(Describe(path, sectionName, field) + "的分量必须落在 [0, 1]（sRGB 作者色）");
        }
    }
    return color;
}

[[nodiscard]] float ReadNonNegative(const toml::table& section, const std::filesystem::path& path,
                                    const char* sectionName, const char* field) {
    const float value = ReadFloat(section, path, sectionName, field);
    if (value < 0.0F) {
        throw std::runtime_error(Describe(path, sectionName, field) + "不能为负");
    }
    return value;
}

}  // namespace

ColorRgb SrgbToLinear(const ColorRgb& srgb) noexcept {
    constexpr float kGammaExponent = 2.2F;
    return ColorRgb { std::pow(srgb[0], kGammaExponent), std::pow(srgb[1], kGammaExponent),
                      std::pow(srgb[2], kGammaExponent) };
}

LightingTable LightingTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载光照表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    const toml::table& sun = RequireTable(document, path, "sun");
    const toml::table& sky = RequireTable(document, path, "sky");
    const toml::table& fog = RequireTable(document, path, "fog");

    LightingTable table;
    table.m_schemaVersion = static_cast<int>(*schemaVersion);

    table.m_sun.direction = ReadVec3(sun, path, "sun", "direction");
    const float directionLengthSq = table.m_sun.direction[0] * table.m_sun.direction[0] +
                                    table.m_sun.direction[1] * table.m_sun.direction[1] +
                                    table.m_sun.direction[2] * table.m_sun.direction[2];
    if (!(directionLengthSq > 0.0F)) {
        throw std::runtime_error(Describe(path, "sun", "direction") + "长度不能为零（无法确定光照方向）");
    }
    table.m_sun.color     = ReadColor(sun, path, "sun", "color");
    table.m_sun.intensity = ReadNonNegative(sun, path, "sun", "intensity");

    table.m_sky.zenithColor  = ReadColor(sky, path, "sky", "zenith_color");
    table.m_sky.horizonColor = ReadColor(sky, path, "sky", "horizon_color");
    table.m_sky.groundColor  = ReadColor(sky, path, "sky", "ground_color");
    table.m_sky.intensity    = ReadNonNegative(sky, path, "sky", "intensity");

    table.m_fog.enabled       = ReadBool(fog, path, "fog", "enabled");
    table.m_fog.density       = ReadNonNegative(fog, path, "fog", "density");
    table.m_fog.heightFalloff = ReadNonNegative(fog, path, "fog", "height_falloff");
    // fog.color 是**可选**字段：缺失时默认取 sky.horizon_color（远景与天空地平色一致）。
    table.m_fog.color = fog.contains("color") ? ReadColor(fog, path, "fog", "color") : table.m_sky.horizonColor;

    // T21b：级联阴影配置（[shadow] 段，必填；缺失 / 越界一律中止启动）。
    const toml::table& shadow = RequireTable(document, path, "shadow");

    table.m_shadow.enabled      = ReadBool(shadow, path, "shadow", "enabled");
    table.m_shadow.cascadeCount = ReadInt(shadow, path, "shadow", "cascade_count");
    if (table.m_shadow.cascadeCount < 1 || table.m_shadow.cascadeCount > kMaxShadowCascades) {
        throw std::runtime_error(Describe(path, "shadow", "cascade_count") + "必须落在 [1, " +
                                 std::to_string(kMaxShadowCascades) + "]");
    }

    table.m_shadow.resolution = ReadInt(shadow, path, "shadow", "resolution");
    if (table.m_shadow.resolution < 256 || (table.m_shadow.resolution & (table.m_shadow.resolution - 1)) != 0) {
        throw std::runtime_error(Describe(path, "shadow", "resolution") + "必须是 2 的幂且 ≥ 256");
    }

    table.m_shadow.maxDistance = ReadFloat(shadow, path, "shadow", "max_distance");
    if (!(table.m_shadow.maxDistance > 0.0F)) {
        throw std::runtime_error(Describe(path, "shadow", "max_distance") + "必须 > 0");
    }

    table.m_shadow.splitLambda = ReadFloat(shadow, path, "shadow", "split_lambda");
    if (table.m_shadow.splitLambda < 0.0F || table.m_shadow.splitLambda > 1.0F) {
        throw std::runtime_error(Describe(path, "shadow", "split_lambda") + "必须落在 [0, 1]");
    }

    table.m_shadow.depthBias    = ReadNonNegative(shadow, path, "shadow", "depth_bias");
    table.m_shadow.normalOffset = ReadNonNegative(shadow, path, "shadow", "normal_offset");

    return table;
}

LightingTable LightingTable::Default() {
    // 取值与 assets/config/lighting.toml 一致，保证测试与运行期行为可比。
    LightingTable table;
    table.m_sun = SunLight { std::array<float, 3> { 0.45F, 0.80F, 0.30F },
                             ColorRgb { 1.00F, 0.96F, 0.88F }, 1.30F };
    table.m_sky = SkyLight { ColorRgb { 0.42F, 0.62F, 0.92F }, ColorRgb { 0.70F, 0.80F, 0.92F },
                             ColorRgb { 0.34F, 0.29F, 0.23F }, 1.00F };
    table.m_fog = FogLayer { true, 0.0030F, 0.02F, table.m_sky.horizonColor };
    // T21b：默认值即本结构体的成员初值（与 assets/config/lighting.toml 的 [shadow] 一致）。
    table.m_shadow = ShadowSettings {};
    return table;
}

LightingUniform BuildLightingUniform(const LightingTable& table, double cameraX, double cameraY,
                                     double cameraZ) noexcept {
    const SunLight& sun = table.Sun();
    const SkyLight& sky = table.Sky();
    const FogLayer& fog = table.Fog();

    LightingUniform uniform;

    // 太阳方向归一化：配置只需给出非零方向，uniform 里恒为单位向量（口径见 lighting.toml 注释）。
    const float directionLengthSq = sun.direction[0] * sun.direction[0] + sun.direction[1] * sun.direction[1] +
                                    sun.direction[2] * sun.direction[2];
    const float inverseLength = 1.0F / std::sqrt(directionLengthSq);
    uniform.sunDirectionX = sun.direction[0] * inverseLength;
    uniform.sunDirectionY = sun.direction[1] * inverseLength;
    uniform.sunDirectionZ = sun.direction[2] * inverseLength;
    uniform.sunIntensity  = sun.intensity;

    // 颜色（常量）在 CPU 侧一次性转线性，见头文件说明。
    const ColorRgb sunLinear     = SrgbToLinear(sun.color);
    const ColorRgb zenithLinear  = SrgbToLinear(sky.zenithColor);
    const ColorRgb horizonLinear = SrgbToLinear(sky.horizonColor);
    const ColorRgb groundLinear  = SrgbToLinear(sky.groundColor);
    const ColorRgb fogLinear     = SrgbToLinear(fog.color);

    uniform.sunColorR = sunLinear[0];
    uniform.sunColorG = sunLinear[1];
    uniform.sunColorB = sunLinear[2];

    uniform.skyZenithR   = zenithLinear[0];
    uniform.skyZenithG   = zenithLinear[1];
    uniform.skyZenithB   = zenithLinear[2];
    uniform.skyIntensity = sky.intensity;

    uniform.skyHorizonR = horizonLinear[0];
    uniform.skyHorizonG = horizonLinear[1];
    uniform.skyHorizonB = horizonLinear[2];

    uniform.skyGroundR = groundLinear[0];
    uniform.skyGroundG = groundLinear[1];
    uniform.skyGroundB = groundLinear[2];

    uniform.cameraWorldX = static_cast<float>(cameraX);
    uniform.cameraWorldY = static_cast<float>(cameraY);
    uniform.cameraWorldZ = static_cast<float>(cameraZ);

    uniform.fogColorR        = fogLinear[0];
    uniform.fogColorG        = fogLinear[1];
    uniform.fogColorB        = fogLinear[2];
    uniform.fogDensity       = fog.density;
    uniform.fogEnabled       = fog.enabled ? 1.0F : 0.0F;
    uniform.fogHeightFalloff = fog.heightFalloff;

    return uniform;
}

}  // namespace vx
