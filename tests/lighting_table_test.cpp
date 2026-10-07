// T21a / T21b / T21c：光照、阴影与雾配置表的解析、校验与 CPU→GPU 单入口投影（BuildLightingUniform）单测。
//
// 口径依据：docs/adr/0010-render-quality-pipeline.md（参数进配置，禁止在着色器里写死第二份）、
//           docs/adr/0005-config-parsing.md（toml++，启动期一次性加载，非法即抛异常，不静默回退）。
//
// 覆盖：合法解析 / 非法配置（颜色越界、零方向、缺字段、schema 不符、负标量、阴影级数 / 分辨率 /
//       分割系数 / 偏移越界）抛异常 / 颜色在 CPU 侧线性化（sRGB 0.5 → 0.5^2.2 ≈ 0.2176）/
//       fog.color 缺失时默认取 sky.horizon_color / 太阳方向归一化 / uniform 与 std140 布局的字节数一致。

#include "render/lighting_table.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using vx::BuildLightingUniform;
using vx::ColorRgb;
using vx::LightingTable;
using vx::LightingUniform;
using vx::ShadowSettings;
using vx::SrgbToLinear;

/// 一份合法的临时光照表：太阳方向 = +Y（已单位化）、天空色便于手算线性值。
/// fog.color **刻意缺失**，用于同时覆盖"默认取 sky.horizon_color"这条路径；
/// [shadow] 段位于最末，便于用字符串替换逐项制造非法值（T21b）。
constexpr const char* kValidConfig =
    "schema_version = 5\n"
    "[sun]\n"
    "direction = [0.0, 1.0, 0.0]\n"
    "color = [1.0, 1.0, 1.0]\n"
    "intensity = 1.5\n"
    "[sky]\n"
    "zenith_color = [0.5, 0.5, 0.5]\n"
    "horizon_color = [0.25, 0.5, 0.75]\n"
    "ground_color = [0.1, 0.1, 0.1]\n"
    "intensity = 0.8\n"
    "[fog]\n"
    "enabled = true\n"
    "density = 0.01\n"
    "height_falloff = 0.05\n"
    "[shadow]\n"
    "enabled = true\n"
    "cascade_count = 3\n"
    "resolution = 2048\n"
    "max_distance = 180.0\n"
    "split_lambda = 0.75\n"
    "depth_bias = 0.0015\n"
    "normal_offset = 0.05\n"
    "caster_height_min = 80.0\n"
    "cascade_blend = 0.3\n";

[[nodiscard]] std::filesystem::path WriteTempConfig(const char* name, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream               out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

void RemoveTempConfig(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

/// `0.5^2.2` 的参考值：CPU 侧线性化必须与着色器的 `pow(c, 2.2)` 口径一致。
constexpr float kHalfLinear = 0.21763764F;

}  // namespace

// 仓库中已提交的配置必须能加载并通过校验；字段值与文件内容一致。
TEST(LightingTable, LoadsCommittedConfig) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "lighting.toml";

    const LightingTable table = LightingTable::LoadFromFile(path);

    EXPECT_EQ(table.SchemaVersion(), LightingTable::kSchemaVersion);
    EXPECT_FLOAT_EQ(table.Sun().intensity, 1.30F);
    EXPECT_FLOAT_EQ(table.Sun().direction[1], 0.80F) << "direction.y（由地表指向太阳）";
    EXPECT_FLOAT_EQ(table.Sky().intensity, 1.00F);
    EXPECT_FLOAT_EQ(table.Sky().horizonColor[2], 0.92F);
    EXPECT_TRUE(table.Fog().enabled);
    EXPECT_FLOAT_EQ(table.Fog().density, 0.0030F);
    EXPECT_FLOAT_EQ(table.Fog().heightFalloff, 0.02F);
    // 提交的 lighting.toml 里 fog.color 被注释掉（可选字段）→ 必须回落到 sky.horizon_color。
    EXPECT_FLOAT_EQ(table.Fog().color[0], table.Sky().horizonColor[0]);
    EXPECT_FLOAT_EQ(table.Fog().color[1], table.Sky().horizonColor[1]);
    EXPECT_FLOAT_EQ(table.Fog().color[2], table.Sky().horizonColor[2]);
    // T21b：提交的 [shadow] 段与配置一致。
    EXPECT_TRUE(table.Shadow().enabled);
    EXPECT_EQ(table.Shadow().cascadeCount, 3);
    EXPECT_EQ(table.Shadow().resolution, 2048);
    EXPECT_FLOAT_EQ(table.Shadow().maxDistance, 180.0F);
    EXPECT_FLOAT_EQ(table.Shadow().splitLambda, 0.75F);
    EXPECT_FLOAT_EQ(table.Shadow().depthBias, 0.0015F);
    EXPECT_FLOAT_EQ(table.Shadow().normalOffset, 0.05F);
    // 缺陷 1：提交配置的投射体扩展下限（覆盖测试地图地标塔）。
    EXPECT_FLOAT_EQ(table.Shadow().casterHeightMin, 160.0F);
    // 缺陷 B8：提交配置的级联过渡带宽度比例。
    EXPECT_FLOAT_EQ(table.Shadow().cascadeBlend, 0.1F);
}

// 内置默认表与提交的配置一致：保证测试与运行期行为可比（口径同材质表）。
TEST(LightingTable, DefaultMatchesCommittedConfig) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "lighting.toml";

    const LightingTable loaded  = LightingTable::LoadFromFile(path);
    const LightingTable builtin = LightingTable::Default();

    EXPECT_FLOAT_EQ(loaded.Sun().intensity, builtin.Sun().intensity);
    EXPECT_FLOAT_EQ(loaded.Sky().intensity, builtin.Sky().intensity);
    EXPECT_FLOAT_EQ(loaded.Sky().zenithColor[1], builtin.Sky().zenithColor[1]);
    EXPECT_FLOAT_EQ(loaded.Sky().groundColor[0], builtin.Sky().groundColor[0]);
    EXPECT_FLOAT_EQ(loaded.Fog().density, builtin.Fog().density);
    EXPECT_FLOAT_EQ(loaded.Fog().heightFalloff, builtin.Fog().heightFalloff);
    for (std::size_t i = 0; i < loaded.Sun().direction.size(); ++i) {
        EXPECT_FLOAT_EQ(loaded.Sun().direction[i], builtin.Sun().direction[i]) << "i=" << i;
    }
    // T21b：内置默认阴影设置与提交配置一致。
    EXPECT_EQ(loaded.Shadow().enabled, builtin.Shadow().enabled);
    EXPECT_EQ(loaded.Shadow().cascadeCount, builtin.Shadow().cascadeCount);
    EXPECT_EQ(loaded.Shadow().resolution, builtin.Shadow().resolution);
    EXPECT_FLOAT_EQ(loaded.Shadow().maxDistance, builtin.Shadow().maxDistance);
    EXPECT_FLOAT_EQ(loaded.Shadow().splitLambda, builtin.Shadow().splitLambda);
    EXPECT_FLOAT_EQ(loaded.Shadow().depthBias, builtin.Shadow().depthBias);
    EXPECT_FLOAT_EQ(loaded.Shadow().normalOffset, builtin.Shadow().normalOffset);
    EXPECT_FLOAT_EQ(loaded.Shadow().casterHeightMin, builtin.Shadow().casterHeightMin);
    EXPECT_FLOAT_EQ(loaded.Shadow().cascadeBlend, builtin.Shadow().cascadeBlend);
}

// T21b / 缺陷 1 / 缺陷 B8：表格式版本已升到 5（新增 [shadow].cascade_blend）——防止旧版文件被静默接受。
TEST(LightingTable, SchemaVersionIsFive) {
    EXPECT_EQ(LightingTable::kSchemaVersion, 5);
}

// 缺失配置必须显式报错，禁止静默回退。
TEST(LightingTable, MissingConfigThrows) {
    EXPECT_THROW((void)LightingTable::LoadFromFile("no_such_lighting_file.toml"), std::runtime_error);
}

// schema_version 不符必须报错。
TEST(LightingTable, InvalidSchemaThrows) {
    std::string content = kValidConfig;
    const std::string from = "schema_version = 5";
    content.replace(content.find(from), from.size(), "schema_version = 99");
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_schema.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 颜色分量越界（> 1）必须报错，而不是钳制。
TEST(LightingTable, ColorOutOfRangeThrows) {
    std::string content = kValidConfig;
    const std::string from = "color = [1.0, 1.0, 1.0]";
    content.replace(content.find(from), from.size(), "color = [1.0, 1.2, 1.0]");
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_color.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 颜色分量越界（< 0）同样必须报错。
TEST(LightingTable, NegativeColorThrows) {
    std::string content = kValidConfig;
    const std::string from = "ground_color = [0.1, 0.1, 0.1]";
    content.replace(content.find(from), from.size(), "ground_color = [-0.1, 0.1, 0.1]");
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_negcolor.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 零向量方向必须报错（无法确定光照方向）。
TEST(LightingTable, ZeroDirectionThrows) {
    std::string content = kValidConfig;
    const std::string from = "direction = [0.0, 1.0, 0.0]";
    content.replace(content.find(from), from.size(), "direction = [0.0, 0.0, 0.0]");
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_dir.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺字段（这里缺 sun.intensity）必须报错。
TEST(LightingTable, MissingFieldThrows) {
    std::string content = kValidConfig;
    const std::string from = "intensity = 1.5\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_missing.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 负的标量（密度 / 强度 / 高度衰减）必须报错。
TEST(LightingTable, NegativeScalarThrows) {
    std::string content = kValidConfig;
    const std::string from = "density = 0.01";
    content.replace(content.find(from), from.size(), "density = -0.01");
    const std::filesystem::path path = WriteTempConfig("vx_invalid_lighting_density.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// fog.color 缺失时默认取 sky.horizon_color（远景自然融入天空的单一来源）。
TEST(LightingTable, FogColorDefaultsToHorizonWhenAbsent) {
    const std::filesystem::path path = WriteTempConfig("vx_lighting_no_fog_color.toml", kValidConfig);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    EXPECT_FLOAT_EQ(table.Fog().color[0], table.Sky().horizonColor[0]);
    EXPECT_FLOAT_EQ(table.Fog().color[1], table.Sky().horizonColor[1]);
    EXPECT_FLOAT_EQ(table.Fog().color[2], table.Sky().horizonColor[2]);
    RemoveTempConfig(path);
}

// fog.color 显式给出时必须**覆盖**默认值（否则"可选"就变成了"忽略"）。
TEST(LightingTable, ExplicitFogColorOverridesHorizon) {
    // 把 color 插进 [fog] 段（kValidConfig 末尾是 [shadow]，直接追加会落错段）。
    std::string content = kValidConfig;
    const std::string from = "density = 0.01";
    content.replace(content.find(from), from.size(), "density = 0.01\ncolor = [0.9, 0.1, 0.2]");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_fog_color.toml", content);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    EXPECT_FLOAT_EQ(table.Fog().color[0], 0.9F);
    EXPECT_FLOAT_EQ(table.Fog().color[1], 0.1F);
    EXPECT_FLOAT_EQ(table.Fog().color[2], 0.2F);
    RemoveTempConfig(path);
}

// 颜色在 **CPU 侧**一次性转线性：sRGB 0.5 → 0.5^2.2 ≈ 0.2176（与着色器的 pow(c, 2.2) 同口径）。
TEST(LightingTable, UniformLinearizesColorOnCpu) {
    const std::filesystem::path path = WriteTempConfig("vx_lighting_linearize.toml", kValidConfig);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    const LightingUniform uniform = BuildLightingUniform(table, 0.0, 0.0, 0.0);

    EXPECT_NEAR(uniform.skyZenithR, kHalfLinear, 1e-4F);
    EXPECT_NEAR(uniform.skyZenithG, kHalfLinear, 1e-4F);
    EXPECT_NEAR(uniform.skyZenithB, kHalfLinear, 1e-4F);
    // 太阳色 1.0 线性化后仍是 1.0；天空强度原样透传。
    EXPECT_FLOAT_EQ(uniform.sunColorR, 1.0F);
    EXPECT_FLOAT_EQ(uniform.skyIntensity, 0.8F);
    // 雾色默认取地平色 [0.25, 0.5, 0.75] → 线性化后 0.25^2.2 / 0.5^2.2 / 0.75^2.2。
    EXPECT_NEAR(uniform.fogColorR, SrgbToLinear(ColorRgb { 0.25F, 0.5F, 0.75F })[0], 1e-4F);
    EXPECT_NEAR(uniform.fogColorG, kHalfLinear, 1e-4F);
    RemoveTempConfig(path);
}

// 太阳方向在投影时归一化：配置只给非零方向，uniform 里恒为单位向量。
TEST(LightingTable, UniformNormalizesSunDirection) {
    std::string content = kValidConfig;
    const std::string from = "direction = [0.0, 1.0, 0.0]";
    content.replace(content.find(from), from.size(), "direction = [0.0, 2.0, 0.0]");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_direction.toml", content);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    const LightingUniform uniform = BuildLightingUniform(table, 0.0, 0.0, 0.0);

    EXPECT_NEAR(uniform.sunDirectionX, 0.0F, 1e-6F);
    EXPECT_NEAR(uniform.sunDirectionY, 1.0F, 1e-6F);
    EXPECT_NEAR(uniform.sunDirectionZ, 0.0F, 1e-6F);
    EXPECT_FLOAT_EQ(uniform.sunIntensity, 1.5F);
    RemoveTempConfig(path);
}

// uniform 的字节数必须与 mesh.frag 的 std140 块逐字节一致（8 个 vec4），并承载相机世界位置与雾参数。
TEST(LightingTable, UniformLayoutAndCameraPosition) {
    const LightingTable table = LightingTable::Default();

    EXPECT_EQ(sizeof(LightingUniform), static_cast<std::size_t>(8 * 16));

    const LightingUniform uniform = BuildLightingUniform(table, 12.0, 34.0, -56.0);
    EXPECT_FLOAT_EQ(uniform.cameraWorldX, 12.0F);
    EXPECT_FLOAT_EQ(uniform.cameraWorldY, 34.0F);
    EXPECT_FLOAT_EQ(uniform.cameraWorldZ, -56.0F);
    EXPECT_FLOAT_EQ(uniform.fogEnabled, 1.0F);
    EXPECT_FLOAT_EQ(uniform.fogDensity, table.Fog().density);
    EXPECT_FLOAT_EQ(uniform.fogHeightFalloff, table.Fog().heightFalloff);
}

// 关闭雾时 uniform 携带 0（着色器据此整体跳过雾计算）。
TEST(LightingTable, DisabledFogIsFlaggedInUniform) {
    std::string content = kValidConfig;
    const std::string from = "enabled = true";
    content.replace(content.find(from), from.size(), "enabled = false");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_fog_off.toml", content);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    const LightingUniform uniform = BuildLightingUniform(table, 0.0, 0.0, 0.0);
    EXPECT_FLOAT_EQ(uniform.fogEnabled, 0.0F);
    RemoveTempConfig(path);
}

// ---- T21b：级联阴影配置的解析与校验（越界 / 非法一律抛异常，禁止静默钳制）----

// 合法配置逐字段落到 ShadowSettings。
TEST(LightingTable, ShadowValidConfigParses) {
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_valid.toml", kValidConfig);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    EXPECT_TRUE(table.Shadow().enabled);
    EXPECT_EQ(table.Shadow().cascadeCount, 3);
    EXPECT_EQ(table.Shadow().resolution, 2048);
    EXPECT_FLOAT_EQ(table.Shadow().maxDistance, 180.0F);
    EXPECT_FLOAT_EQ(table.Shadow().splitLambda, 0.75F);
    EXPECT_FLOAT_EQ(table.Shadow().depthBias, 0.0015F);
    EXPECT_FLOAT_EQ(table.Shadow().normalOffset, 0.05F);
    EXPECT_FLOAT_EQ(table.Shadow().casterHeightMin, 80.0F);
    EXPECT_FLOAT_EQ(table.Shadow().cascadeBlend, 0.3F);
    RemoveTempConfig(path);
}

// 结构体默认值必须与提交配置同源（防止默认值与文件各写一份）。
TEST(LightingTable, ShadowSettingsDefaultsMatchCommittedConfig) {
    const ShadowSettings defaults {};

    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.cascadeCount, 3);
    EXPECT_EQ(defaults.resolution, 2048);
    EXPECT_FLOAT_EQ(defaults.maxDistance, 180.0F);
    EXPECT_FLOAT_EQ(defaults.splitLambda, 0.75F);
    EXPECT_FLOAT_EQ(defaults.depthBias, 0.0015F);
    EXPECT_FLOAT_EQ(defaults.normalOffset, 0.05F);
    EXPECT_FLOAT_EQ(defaults.casterHeightMin, 160.0F);
    EXPECT_FLOAT_EQ(defaults.cascadeBlend, 0.1F);
}

// [shadow] 段整段缺失必须报错。
TEST(LightingTable, ShadowMissingSectionThrows) {
    std::string content = kValidConfig;
    const std::string from = "[shadow]\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_missing_section.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺字段（这里缺 cascade_count）必须报错。
TEST(LightingTable, ShadowMissingFieldThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_count = 3\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_missing_field.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 级数越界（> 4）必须报错。
TEST(LightingTable, ShadowCascadeCountTooLargeThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_count = 3";
    content.replace(content.find(from), from.size(), "cascade_count = 5");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_count_high.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 级数为 0 必须报错（至少一级）。
TEST(LightingTable, ShadowCascadeCountZeroThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_count = 3";
    content.replace(content.find(from), from.size(), "cascade_count = 0");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_count_zero.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 分辨率非 2 的幂必须报错（300）。
TEST(LightingTable, ShadowResolutionNotPowerOfTwoThrows) {
    std::string content = kValidConfig;
    const std::string from = "resolution = 2048";
    content.replace(content.find(from), from.size(), "resolution = 300");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_res_npot.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 分辨率低于 256 必须报错（128）。
TEST(LightingTable, ShadowResolutionTooSmallThrows) {
    std::string content = kValidConfig;
    const std::string from = "resolution = 2048";
    content.replace(content.find(from), from.size(), "resolution = 128");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_res_small.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 分割系数越界（1.5）必须报错。
TEST(LightingTable, ShadowSplitLambdaOutOfRangeThrows) {
    std::string content = kValidConfig;
    const std::string from = "split_lambda = 0.75";
    content.replace(content.find(from), from.size(), "split_lambda = 1.5");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_lambda.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 覆盖距离非正必须报错。
TEST(LightingTable, ShadowMaxDistanceNonPositiveThrows) {
    std::string content = kValidConfig;
    const std::string from = "max_distance = 180.0";
    content.replace(content.find(from), from.size(), "max_distance = 0.0");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_distance.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 负的深度偏移必须报错。
TEST(LightingTable, ShadowDepthBiasNegativeThrows) {
    std::string content = kValidConfig;
    const std::string from = "depth_bias = 0.0015";
    content.replace(content.find(from), from.size(), "depth_bias = -0.001");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_bias.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 负的法线偏移必须报错。
TEST(LightingTable, ShadowNormalOffsetNegativeThrows) {
    std::string content = kValidConfig;
    const std::string from = "normal_offset = 0.05";
    content.replace(content.find(from), from.size(), "normal_offset = -0.05");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_normal.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺陷 1：缺 caster_height_min 必须报错（结构变更，旧文件不得被静默接受）。
TEST(LightingTable, ShadowMissingCasterHeightMinThrows) {
    std::string content = kValidConfig;
    const std::string from = "caster_height_min = 80.0\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_no_caster.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺陷 1：负的 caster_height_min 必须报错。
TEST(LightingTable, ShadowCasterHeightMinNegativeThrows) {
    std::string content = kValidConfig;
    const std::string from = "caster_height_min = 80.0";
    content.replace(content.find(from), from.size(), "caster_height_min = -1.0");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_caster_neg.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺陷 B8：缺 cascade_blend 必须报错（结构变更，旧文件不得被静默接受）。
TEST(LightingTable, ShadowMissingCascadeBlendThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_blend = 0.3\n";
    content.erase(content.find(from), from.size());
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_no_blend.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺陷 B8：cascade_blend 越界（> 0.5）必须报错，禁止静默钳制。
TEST(LightingTable, ShadowCascadeBlendTooLargeThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_blend = 0.3";
    content.replace(content.find(from), from.size(), "cascade_blend = 0.6");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_blend_high.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 缺陷 B8：负的 cascade_blend 必须报错。
TEST(LightingTable, ShadowCascadeBlendNegativeThrows) {
    std::string content = kValidConfig;
    const std::string from = "cascade_blend = 0.3";
    content.replace(content.find(from), from.size(), "cascade_blend = -0.1");
    const std::filesystem::path path = WriteTempConfig("vx_lighting_shadow_blend_neg.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// ---- T67：环境贴图（[environment] 可选段）与 IBL 启用位的 CPU→GPU 投影 ----

// 提交的配置必须启用 IBL 并给出 HDRI 路径（与 Default() 同源，见下一条）。
TEST(LightingTable, CommittedEnvironmentIsEnabled) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "lighting.toml";

    const LightingTable table = LightingTable::LoadFromFile(path);

    EXPECT_TRUE(table.Environment().enabled);
    EXPECT_FALSE(table.Environment().hdri.empty()) << "enabled = true 时路径不得为空";
    // 路径指向 `assets/textures/env/` 下的 `.hdr`（资源不入库，由 tools/fetch_assets.ps1 获取）。
    EXPECT_EQ(table.Environment().hdri.extension().string(), ".hdr");
}

// 内置默认表与提交配置的 [environment] 一致（防"默认值与文件各写一份"）。
TEST(LightingTable, EnvironmentDefaultsMatchCommittedConfig) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "lighting.toml";

    const LightingTable loaded  = LightingTable::LoadFromFile(path);
    const LightingTable builtin = LightingTable::Default();

    EXPECT_EQ(loaded.Environment().enabled, builtin.Environment().enabled);
    EXPECT_EQ(loaded.Environment().hdri, builtin.Environment().hdri);
}

// `[environment]` 段**整段缺失** ⇒ 不启用（旧版文件照旧可用）；这是"可选段"的契约本身。
TEST(LightingTable, MissingEnvironmentSectionDisablesIbl) {
    const std::filesystem::path path = WriteTempConfig("vx_lighting_no_environment.toml", kValidConfig);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    EXPECT_FALSE(table.Environment().enabled);
    EXPECT_TRUE(table.Environment().hdri.empty());
    RemoveTempConfig(path);
}

// 段在但缺 hdri ⇒ 报错（字段缺失不得被静默当成"不启用"）。
TEST(LightingTable, EnvironmentMissingHdriThrows) {
    std::string content = kValidConfig;
    content += "[environment]\nenabled = true\n";
    const std::filesystem::path path = WriteTempConfig("vx_lighting_env_no_hdri.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// enabled = true 但路径为空串 ⇒ 报错（空路径只可能是配置写漏）。
TEST(LightingTable, EnvironmentEmptyPathWhenEnabledThrows) {
    std::string content = kValidConfig;
    content += "[environment]\nenabled = true\nhdri = \"\"\n";
    const std::filesystem::path path = WriteTempConfig("vx_lighting_env_empty_path.toml", content);
    EXPECT_THROW((void)LightingTable::LoadFromFile(path), std::runtime_error);
    RemoveTempConfig(path);
}

// 合法段逐字段落地（enabled = false 时允许留空路径 —— 关闭态不要求资源）。
TEST(LightingTable, EnvironmentParsesExplicitValues) {
    std::string content = kValidConfig;
    content += "[environment]\nenabled = true\nhdri = \"assets/textures/env/x.hdr\"\n";
    const std::filesystem::path path = WriteTempConfig("vx_lighting_env_valid.toml", content);

    const LightingTable table = LightingTable::LoadFromFile(path);
    EXPECT_TRUE(table.Environment().enabled);
    EXPECT_EQ(table.Environment().hdri.generic_string(), "assets/textures/env/x.hdr");

    content.replace(content.find("enabled = true\nhdri"), std::string("enabled = true\nhdri").size(),
                    "enabled = false\nhdri");
    const std::filesystem::path disabledPath = WriteTempConfig("vx_lighting_env_disabled.toml", content);
    const LightingTable         disabled     = LightingTable::LoadFromFile(disabledPath);
    EXPECT_FALSE(disabled.Environment().enabled);
    RemoveTempConfig(path);
    RemoveTempConfig(disabledPath);
}

// IBL 启用位与预过滤 mip 级号：**级数为 0 必须是不启用**（回落），且两格同时为 0（不留半启用态）。
TEST(LightingTable, UniformFlagsIblState) {
    const LightingTable table = LightingTable::Default();

    const LightingUniform fallback = BuildLightingUniform(table, 0.0, 0.0, 0.0, /*environmentPrefilterMipCount=*/0U);
    EXPECT_FLOAT_EQ(fallback.fogIblEnabled, 0.0F);
    EXPECT_FLOAT_EQ(fallback.fogIblPrefilterLodMax, 0.0F);

    const LightingUniform enabled = BuildLightingUniform(table, 0.0, 0.0, 0.0, /*environmentPrefilterMipCount=*/6U);
    EXPECT_FLOAT_EQ(enabled.fogIblEnabled, 1.0F);
    // 级号 = 级数 − 1：着色器按 `lod = roughness × 本值` 选级（与烘焙侧 PrefilterRoughnessForMip 同源）。
    EXPECT_FLOAT_EQ(enabled.fogIblPrefilterLodMax, 5.0F);
}

// 默认省参调用 = 不启用 IBL：保证既有调用点（旧测试 / 离线工具）语义不变。
TEST(LightingTable, UniformDefaultArgumentDisablesIbl) {
    const LightingUniform uniform = BuildLightingUniform(LightingTable::Default(), 0.0, 0.0, 0.0);
    EXPECT_FLOAT_EQ(uniform.fogIblEnabled, 0.0F);
    EXPECT_FLOAT_EQ(uniform.fogIblPrefilterLodMax, 0.0F);
}

// V0.9 / ADR 0036 决策一：室内变暗的全局默认值经**现成的空闲分量** `sunColorLinear.a` 投影。
//   缺省 = `kDefaultInteriorDarkening`（0.45，与 ADR 0035 的写死常量同值 ⇒ 缺省行为与 V0.8 逐位一致）；
//   `1.0` = 完全不调暗（着色器里乘 1.0 = 恒等 ⇒ 逐位退回"引入室内变暗之前"）。
TEST(LightingTable, UniformCarriesInteriorDarkening) {
    const LightingTable table = LightingTable::Default();

    const LightingUniform byDefault = BuildLightingUniform(table, 0.0, 0.0, 0.0);
    EXPECT_FLOAT_EQ(byDefault.interiorDarkening, vx::kDefaultInteriorDarkening);
    EXPECT_FLOAT_EQ(byDefault.interiorDarkening, 0.45F);

    const LightingUniform off = BuildLightingUniform(table, 0.0, 0.0, 0.0, /*environmentPrefilterMipCount=*/0U,
                                                     /*interiorDarkening=*/1.0F);
    EXPECT_FLOAT_EQ(off.interiorDarkening, 1.0F);

    const LightingUniform custom = BuildLightingUniform(table, 0.0, 0.0, 0.0, 0U, /*interiorDarkening=*/0.20F);
    EXPECT_FLOAT_EQ(custom.interiorDarkening, 0.20F);
}

// 复用空闲分量 ⇒ **不扩结构**：`sizeof(LightingUniform)` 仍 = 8 个 vec4。
TEST(LightingTable, InteriorDarkeningDoesNotGrowUniform) {
    EXPECT_EQ(sizeof(LightingUniform), static_cast<std::size_t>(8 * 16));
}
