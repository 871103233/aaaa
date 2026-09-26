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
    "schema_version = 4\n"
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
    "caster_height_min = 80.0\n";

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
}

// T21b / 缺陷 1：表格式版本已升到 4（新增 [shadow].caster_height_min）——防止旧版文件被静默接受。
TEST(LightingTable, SchemaVersionIsFour) {
    EXPECT_EQ(LightingTable::kSchemaVersion, 4);
}

// 缺失配置必须显式报错，禁止静默回退。
TEST(LightingTable, MissingConfigThrows) {
    EXPECT_THROW((void)LightingTable::LoadFromFile("no_such_lighting_file.toml"), std::runtime_error);
}

// schema_version 不符必须报错。
TEST(LightingTable, InvalidSchemaThrows) {
    std::string content = kValidConfig;
    const std::string from = "schema_version = 4";
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
