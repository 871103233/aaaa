// 气候（V0.6 C7）单测：温度 / 湿度噪声的**确定性 / 值域 / 互不相关**，以及 `[climate]` 配置解析
//（可选段、缺省值、非法即抛）与"**不影响地形高度**"的解耦不变量。
// 见 docs/plans/v0.6.md C7 与 tech-plan-v2.0.md §3.1。

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using vx::TerrainClimateParams;
using vx::TerrainGenerationParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED1234ULL;

/// 一份**完整合法**的地表参数 TOML（`[climate]` 段由参数决定 ⇒ 可测"缺省 / 显式 / 非法"三种情形）。
[[nodiscard]] std::string FullParamsToml(const std::string& climateSection) {
    return std::string("schema_version = 1\n"
                       "base_frequency = 0.0035\n"
                       "detail_frequency = 0.015\n"
                       "rough_frequency = 0.06\n"
                       "base_amplitude = 96.0\n"
                       "detail_amplitude = 22.0\n"
                       "rough_amplitude = 4.0\n"
                       "height_offset_blocks = 128.0\n"
                       "variation_frequency = 0.05\n"
                       "[landform]\n"
                       "enabled = true\n"
                       "frequency = 0.0012\n"
                       "hills_start = 0.42\n"
                       "mountains_start = 0.66\n"
                       "blend = 0.08\n"
                       "plains_amplitude_scale = 0.30\n"
                       "hills_amplitude_scale = 0.85\n"
                       "mountains_amplitude_scale = 1.45\n"
                       "plains_offset_blocks = -8.0\n"
                       "hills_offset_blocks = 0.0\n"
                       "mountains_offset_blocks = 24.0\n"
                       "seed_channel = 5\n"
                       "[overhang]\n"
                       "frequency = 0.05\n"
                       "amplitude_blocks = 7.0\n"
                       "seed_channel = 6\n"
                       "[caves]\n"
                       "enabled = true\n"
                       "frequency = 0.02\n"
                       "tunnel_radius = 0.32\n"
                       "carve_strength_blocks = 22.0\n"
                       "depth_fade_blocks = 8.0\n"
                       "seed_channel = 7\n"
                       "[river]\n"
                       "enabled = true\n"
                       "step_blocks = 8.0\n"
                       "max_nodes = 512\n"
                       "channel_depth_blocks = 4.0\n"
                       "channel_half_width_blocks = 6.0\n"
                       "bank_half_width_blocks = 4.0\n"
                       "water_depth_blocks = 2.0\n"
                       "jitter_radians = 0.35\n"
                       "jitter_frequency = 0.01\n"
                       "seed_channel = 8\n") +
           climateSection;
}

}  // namespace

// 噪声：确定性（同种子逐位一致）+ 值域 [0,1] + 温度与湿度互不相关。
TEST(Climate, NoiseIsDeterministicInRangeAndIndependent) {
    const TerrainNoiseGenerator first(kSeed);
    const TerrainNoiseGenerator second(kSeed);

    std::size_t identicalPairs = 0;
    for (int z = -900; z <= 900; z += 53) {
        for (int x = -900; x <= 900; x += 53) {
            const float temperature = first.TemperatureAt(x, z);
            const float humidity    = first.HumidityAt(x, z);

            EXPECT_FLOAT_EQ(temperature, second.TemperatureAt(x, z)) << "同种子两次构造必须逐位一致（红线 7）";
            EXPECT_GE(temperature, 0.0F);
            EXPECT_LE(temperature, 1.0F);
            EXPECT_GE(humidity, 0.0F);
            EXPECT_LE(humidity, 1.0F);

            // 两张独立噪声 ⇒ 绝大多数采样点不相等（同通道会处处相等，那才是缺陷）。
            if (std::fabs(temperature - humidity) < 1.0e-6F) {
                ++identicalPairs;
            }
        }
    }
    EXPECT_EQ(identicalPairs, 0U) << "温度与湿度用了同一个通道？（应互不相关）";
}

// 解耦不变量：改气候参数**不改变任何地形高度**（气候只供内容放置判据，见 tech-plan §3.1）。
TEST(Climate, DoesNotAffectTerrainHeights) {
    TerrainGenerationParams base = TerrainGenerationParams::Default();
    TerrainGenerationParams tuned = base;
    tuned.climate.temperatureFrequency = 0.02F;
    tuned.climate.humidityFrequency    = 0.03F;
    tuned.climate.temperatureSeedChannel = 21;
    tuned.climate.humiditySeedChannel    = 22;

    const TerrainNoiseGenerator baseNoise(kSeed, base);
    const TerrainNoiseGenerator tunedNoise(kSeed, tuned);

    for (int z = -500; z <= 500; z += 61) {
        for (int x = -500; x <= 500; x += 61) {
            EXPECT_EQ(baseNoise.HeightUnits(x, z), tunedNoise.HeightUnits(x, z))
                << "气候参数不得影响地形高度（与高度图解耦）";
        }
    }
}

// 配置：`[climate]` **可选** —— 缺省 = 代码默认（频率与通道 9 / 10）。
TEST(Climate, SectionIsOptionalAndFallsBackToDefaults) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "voxel_terrain_climate_absent.toml";
    {
        std::ofstream out(path, std::ios::trunc);
        out << FullParamsToml("");
    }
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(path);
    const TerrainClimateParams   defaults;
    EXPECT_FLOAT_EQ(params.climate.temperatureFrequency, defaults.temperatureFrequency);
    EXPECT_FLOAT_EQ(params.climate.humidityFrequency, defaults.humidityFrequency);
    EXPECT_EQ(params.climate.temperatureSeedChannel, defaults.temperatureSeedChannel);
    EXPECT_EQ(params.climate.humiditySeedChannel, defaults.humiditySeedChannel);

    std::error_code error;
    std::filesystem::remove(path, error);
}

// 配置：显式 `[climate]` 被解析；通道相同 ⇒ 抛（判据退化）。
TEST(Climate, ParsesExplicitSectionAndRejectsSameChannel) {
    const std::filesystem::path ok = std::filesystem::temp_directory_path() / "voxel_terrain_climate_ok.toml";
    {
        std::ofstream out(ok, std::ios::trunc);
        out << FullParamsToml("[climate]\n"
                              "temperature_frequency = 0.0011\n"
                              "humidity_frequency = 0.0022\n"
                              "temperature_seed_channel = 11\n"
                              "humidity_seed_channel = 12\n");
    }
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(ok);
    EXPECT_FLOAT_EQ(params.climate.temperatureFrequency, 0.0011F);
    EXPECT_FLOAT_EQ(params.climate.humidityFrequency, 0.0022F);
    EXPECT_EQ(params.climate.temperatureSeedChannel, 11U);
    EXPECT_EQ(params.climate.humiditySeedChannel, 12U);

    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "voxel_terrain_climate_bad.toml";
    {
        std::ofstream out(bad, std::ios::trunc);
        out << FullParamsToml("[climate]\n"
                              "temperature_frequency = 0.0011\n"
                              "humidity_frequency = 0.0022\n"
                              "temperature_seed_channel = 11\n"
                              "humidity_seed_channel = 11\n");
    }
    EXPECT_THROW(static_cast<void>(TerrainGenerationParams::LoadFromFile(bad)), std::runtime_error);

    std::error_code error;
    std::filesystem::remove(ok, error);
    std::filesystem::remove(bad, error);
}

// 随仓库发布的 `assets/config/terrain.toml` 必须可加载（含 [climate] 段）。
TEST(Climate, ShippedConfigLoads) {
    const std::filesystem::path path = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/terrain.toml";
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(path);
    EXPECT_GT(params.climate.temperatureFrequency, 0.0F);
    EXPECT_GT(params.climate.humidityFrequency, 0.0F);
    EXPECT_NE(params.climate.temperatureSeedChannel, params.climate.humiditySeedChannel);
}
