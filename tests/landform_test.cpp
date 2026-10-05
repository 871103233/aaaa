// W3 / [ADR 0026 无关，见 plans/v0.4.md §1.2]：**地貌分区**（山川 / 平原 / 丘陵）的单测。
//
// 覆盖四条判据：
//   ① 分档正确 —— `ClassifyLandform` 在阈值处取值符合定义；
//   ② 调制连续且单调 —— 幅度倍数与基线平移随掩罩不减；
//   ③ **默认关闭 ⇒ 与引入前逐位一致**（`enabled == false` 恒为 {1, 0}，生成器输出不受影响）；
//   ④ 地貌在统计上可区分 —— 三类面积占比各自非空、平均高度与平均坡度满足 平原 < 丘陵 < 山川。

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "terrain/terrain_types.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using vx::ClassifyLandform;
using vx::EvaluateLandformModulation;
using vx::LandformKind;
using vx::LandformModulation;
using vx::TerrainGenerationParams;
using vx::TerrainLandformParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED1234ULL;

/// 启用掩罩、阈值明确的测试参数。
[[nodiscard]] TerrainGenerationParams EnabledParams() {
    TerrainGenerationParams params = TerrainGenerationParams::Default();
    params.landform.enabled        = true;
    return params;
}

}  // namespace

// ① 分档：`mask < hills_start + blend/2` ⇒ 平原；`< mountains_start + blend/2` ⇒ 丘陵；否则山川。
TEST(Landform, ClassifyUsesConfiguredThresholds) {
    TerrainLandformParams params;
    params.enabled        = true;
    params.hillsStart     = 0.40F;
    params.mountainsStart = 0.70F;
    params.blend          = 0.20F;  // half = 0.10 ⇒ 分界在 0.50 / 0.80

    EXPECT_EQ(ClassifyLandform(0.00F, params), LandformKind::Plains);
    EXPECT_EQ(ClassifyLandform(0.49F, params), LandformKind::Plains);
    EXPECT_EQ(ClassifyLandform(0.50F, params), LandformKind::Hills);
    EXPECT_EQ(ClassifyLandform(0.79F, params), LandformKind::Hills);
    EXPECT_EQ(ClassifyLandform(0.80F, params), LandformKind::Mountains);
    EXPECT_EQ(ClassifyLandform(1.00F, params), LandformKind::Mountains);
}

// ② 调制：关闭 ⇒ 恒等；开启 ⇒ 端点取段值，且全程**单调不减**。
TEST(Landform, ModulationIsIdentityWhenDisabledAndMonotonicWhenEnabled) {
    TerrainLandformParams disabled;
    disabled.enabled = false;
    for (const float mask : { 0.0F, 0.25F, 0.5F, 0.75F, 1.0F }) {
        const LandformModulation mod = EvaluateLandformModulation(mask, disabled);
        EXPECT_FLOAT_EQ(mod.amplitudeScale, 1.0F);
        EXPECT_FLOAT_EQ(mod.offsetBlocks, 0.0F);
    }

    const TerrainGenerationParams params = EnabledParams();
    const TerrainLandformParams& lf      = params.landform;

    // 段内取值 == 该段的参数（含端点）。
    EXPECT_FLOAT_EQ(EvaluateLandformModulation(0.0F, lf).amplitudeScale, lf.plainsAmplitudeScale);
    EXPECT_FLOAT_EQ(EvaluateLandformModulation(0.5F, lf).amplitudeScale, lf.hillsAmplitudeScale);
    EXPECT_FLOAT_EQ(EvaluateLandformModulation(1.0F, lf).amplitudeScale, lf.mountainsAmplitudeScale);

    float previousAmplitude = -1.0F;
    float previousOffset    = -1e9F;
    for (int i = 0; i <= 100; ++i) {
        const float             mask = static_cast<float>(i) / 100.0F;
        const LandformModulation mod = EvaluateLandformModulation(mask, lf);
        EXPECT_GE(mod.amplitudeScale, previousAmplitude) << "幅度倍数必须随掩罩单调不减（mask=" << mask << "）";
        EXPECT_GE(mod.offsetBlocks, previousOffset) << "基线平移必须随掩罩单调不减（mask=" << mask << "）";
        previousAmplitude = mod.amplitudeScale;
        previousOffset    = mod.offsetBlocks;
    }
}

// ③ 默认（关闭）⇒ 生成器输出与"显式传默认参数"逐位一致；且确定性（两次构造同值）。
TEST(Landform, DisabledGeneratorIsBitIdenticalAndDeterministic) {
    const TerrainNoiseGenerator implicitDefault(kSeed);
    const TerrainNoiseGenerator explicitDefault(kSeed, TerrainGenerationParams::Default());
    const TerrainNoiseGenerator again(kSeed);

    for (int z = -300; z <= 300; z += 37) {
        for (int x = -300; x <= 300; x += 37) {
            const vx::Height h1 = implicitDefault.HeightUnits(x, z);
            EXPECT_EQ(h1, explicitDefault.HeightUnits(x, z)) << "默认构造必须与显式默认参数逐位一致";
            EXPECT_EQ(h1, again.HeightUnits(x, z)) << "同种子两次构造必须逐位一致（红线 7）";
        }
    }
}

// ④ 地貌分布：三类面积占比各自非空；平均高度与平均坡度满足 平原 < 丘陵 < 山川。
TEST(Landform, ShapesTerrainIntoThreeDistinguishableBands) {
    const TerrainGenerationParams params = EnabledParams();
    const TerrainNoiseGenerator   noise(kSeed, params);

    std::size_t counts[3]    = { 0, 0, 0 };
    double      heightSum[3] = { 0.0, 0.0, 0.0 };
    double      slopeSum[3]  = { 0.0, 0.0, 0.0 };

    // 采样一片较大的区域（跨度 4000 格 ≫ 掩罩波长 833 格）⇒ 三类都应出现。
    for (int z = -2000; z < 2000; z += 40) {
        for (int x = -2000; x < 2000; x += 40) {
            const LandformKind kind = ClassifyLandform(noise.LandformMaskAt(x, z), params.landform);
            const std::size_t  index = static_cast<std::size_t>(kind);

            const double height = static_cast<double>(vx::HeightToBlocks(noise.HeightUnits(x, z)));
            const double next   = static_cast<double>(vx::HeightToBlocks(noise.HeightUnits(x + 1, z)));

            ++counts[index];
            heightSum[index] += height;
            slopeSum[index] += (next > height) ? (next - height) : (height - next);
        }
    }

    const std::size_t total = counts[0] + counts[1] + counts[2];
    ASSERT_GT(total, 0U);
    for (int k = 0; k < 3; ++k) {
        const double share = static_cast<double>(counts[k]) / static_cast<double>(total);
        EXPECT_GT(counts[k], 0U) << "第 " << k << " 类地貌必须存在";
        EXPECT_GE(share, 0.03) << "第 " << k << " 类地貌占比过低（" << share << "）";
        EXPECT_LE(share, 0.90) << "第 " << k << " 类地貌占比过高（" << share << "）";
    }

    const double meanHeightPlains    = heightSum[0] / static_cast<double>(counts[0]);
    const double meanHeightHills     = heightSum[1] / static_cast<double>(counts[1]);
    const double meanHeightMountains = heightSum[2] / static_cast<double>(counts[2]);
    EXPECT_LT(meanHeightPlains, meanHeightHills) << "平原平均高度应低于丘陵";
    EXPECT_LT(meanHeightHills, meanHeightMountains) << "丘陵平均高度应低于山川";

    const double meanSlopePlains    = slopeSum[0] / static_cast<double>(counts[0]);
    const double meanSlopeHills     = slopeSum[1] / static_cast<double>(counts[1]);
    const double meanSlopeMountains = slopeSum[2] / static_cast<double>(counts[2]);
    EXPECT_LT(meanSlopePlains, meanSlopeHills) << "平原平均坡度应小于丘陵";
    EXPECT_LT(meanSlopeHills, meanSlopeMountains) << "丘陵平均坡度应小于山川";
}

// 随仓库发布的 `assets/config/terrain.toml` 必须可加载且**启用**地貌分区（否则游戏世界没有分区）。
TEST(Landform, ShippedConfigLoadsAndEnablesLandform) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/terrain.toml";
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(path);

    EXPECT_TRUE(params.landform.enabled) << "发布配置必须启用地貌分区";
    EXPECT_LT(params.landform.hillsStart, params.landform.mountainsStart);
    EXPECT_GT(params.landform.mountainsAmplitudeScale, params.landform.plainsAmplitudeScale);
}

// 非法配置 ⇒ 加载即抛（ADR 0005 口径，不静默回退）；文件缺失同样抛。
TEST(Landform, RejectsIllegalOrMissingConfig) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "voxel_terrain_params_missing.toml";
    std::error_code error;
    std::filesystem::remove(missing, error);
    EXPECT_THROW((void)TerrainGenerationParams::LoadFromFile(missing), std::runtime_error);

    // 非法：hills_start > mountains_start（违反 0 ≤ hills_start ≤ mountains_start ≤ 1）。
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "voxel_terrain_params_bad.toml";
    {
        std::ofstream out(bad, std::ios::trunc);
        out << "schema_version = 1\n"
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
               "hills_start = 0.90\n"
               "mountains_start = 0.50\n"
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
               "seed_channel = 6\n";
    }
    EXPECT_THROW((void)TerrainGenerationParams::LoadFromFile(bad), std::runtime_error);
    std::filesystem::remove(bad, error);
}
