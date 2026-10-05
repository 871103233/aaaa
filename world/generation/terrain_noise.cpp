#include "generation/terrain_noise.hpp"

#include "generation/seed.hpp"

#include <algorithm>
#include <cmath>

#include "FastNoiseLite.h"  // third_party/FastNoiseLite（vendored，MIT）

namespace vx {
namespace {

/// 通道编号：同一全局种子下，各噪声层必须用不同通道，避免层间耦合（方案 §3.2）。
/// 地貌掩罩的通道由 `TerrainLandformParams::seedChannel` 提供（默认 5），排在既有四层之后。
constexpr std::uint64_t kChannelBase      = 1;
constexpr std::uint64_t kChannelDetail    = 2;
constexpr std::uint64_t kChannelRough     = 3;
constexpr std::uint64_t kChannelVariation = 4;

}  // namespace

struct TerrainNoiseGenerator::Impl {
    TerrainGenerationParams params;

    FastNoiseLite base;
    FastNoiseLite detail;
    FastNoiseLite rough;
    FastNoiseLite variation;
    FastNoiseLite landform;
    FastNoiseLite overhang;
    FastNoiseLite caveA;
    FastNoiseLite caveB;
    FastNoiseLite riverJitter;
};

TerrainNoiseGenerator::TerrainNoiseGenerator(std::uint64_t worldSeed, TerrainGenerationParams params)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->params = params;

    m_impl->base.SetSeed(DeriveChannelSeed(worldSeed, kChannelBase));
    m_impl->base.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->base.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->base.SetFractalOctaves(4);
    m_impl->base.SetFrequency(params.baseFrequency);

    m_impl->detail.SetSeed(DeriveChannelSeed(worldSeed, kChannelDetail));
    m_impl->detail.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->detail.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->detail.SetFractalOctaves(3);
    m_impl->detail.SetFrequency(params.detailFrequency);

    m_impl->rough.SetSeed(DeriveChannelSeed(worldSeed, kChannelRough));
    m_impl->rough.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->rough.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->rough.SetFractalOctaves(2);
    m_impl->rough.SetFrequency(params.roughFrequency);

    m_impl->variation.SetSeed(DeriveChannelSeed(worldSeed, kChannelVariation));
    m_impl->variation.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->variation.SetFrequency(params.variationFrequency);

    // 地貌掩罩：低频 FBm。未启用时也照常初始化（不参与 `HeightUnits` 的取值路径），仅是构造成本。
    m_impl->landform.SetSeed(DeriveChannelSeed(worldSeed, params.landform.seedChannel));
    m_impl->landform.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->landform.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->landform.SetFractalOctaves(3);
    m_impl->landform.SetFrequency(params.landform.frequency);

    // 悬垂 3D 噪声（W4 地表体积壳）。2 个 octave：低频给大起伏、高频给表面颗粒。
    m_impl->overhang.SetSeed(DeriveChannelSeed(worldSeed, params.overhang.seedChannel));
    m_impl->overhang.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->overhang.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->overhang.SetFractalOctaves(2);
    m_impl->overhang.SetFrequency(params.overhang.frequency);

    // 洞穴隧道噪声（W5）：**两条**独立 3D 噪声构成 `sqrt(a²+b²) < r` 的隧道网络（见 `CaveCarveAt`）。
    // 第二条把全局种子过一遍 `SplitMix64` 再派生 ⇒ 与第一条不相关，且**只占一个通道号**（seedChannel）。
    m_impl->caveA.SetSeed(DeriveChannelSeed(worldSeed, params.caves.seedChannel));
    m_impl->caveA.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->caveA.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->caveA.SetFractalOctaves(2);
    m_impl->caveA.SetFrequency(params.caves.frequency);

    m_impl->caveB.SetSeed(DeriveChannelSeed(SplitMix64(worldSeed), params.caves.seedChannel));
    m_impl->caveB.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->caveB.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->caveB.SetFractalOctaves(2);
    m_impl->caveB.SetFrequency(params.caves.frequency);

    // 河流抖动噪声（W6）：2D、低频，只用于**河道走向的确定性域扭曲**（不参与高度生成）。
    m_impl->riverJitter.SetSeed(DeriveChannelSeed(worldSeed, params.river.seedChannel));
    m_impl->riverJitter.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->riverJitter.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->riverJitter.SetFractalOctaves(2);
    m_impl->riverJitter.SetFrequency(params.river.jitterFrequency);
}

TerrainNoiseGenerator::~TerrainNoiseGenerator() = default;

TerrainNoiseGenerator::TerrainNoiseGenerator(TerrainNoiseGenerator&&) noexcept = default;
TerrainNoiseGenerator& TerrainNoiseGenerator::operator=(TerrainNoiseGenerator&&) noexcept = default;

Height TerrainNoiseGenerator::HeightUnits(std::int64_t worldX, std::int64_t worldZ) const noexcept {
    // 世界定位仍由整数坐标承担；这里转 float 只是喂给噪声函数，不用于坐标存储（red line 6）。
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);

    const TerrainGenerationParams& params = m_impl->params;

    float heightBlocks = 0.0F;
    if (!params.landform.enabled) {
        // **逐位一致**路径：表达式与引入地貌层之前完全相同（`params` 默认即那时的常量）。
        heightBlocks = params.heightOffsetBlocks +
                       m_impl->base.GetNoise(x, z) * params.baseAmplitude +
                       m_impl->detail.GetNoise(x, z) * params.detailAmplitude +
                       m_impl->rough.GetNoise(x, z) * params.roughAmplitude;
    } else {
        // 掩罩归一化到 [0,1]（噪声原始输出约 [-1,1]），再按分档调制幅度与基线。
        const float              mask = LandformMaskAt(worldX, worldZ);
        const LandformModulation mod  = EvaluateLandformModulation(mask, params.landform);
        const float              relief = m_impl->base.GetNoise(x, z) * params.baseAmplitude +
                                          m_impl->detail.GetNoise(x, z) * params.detailAmplitude +
                                          m_impl->rough.GetNoise(x, z) * params.roughAmplitude;
        heightBlocks = params.heightOffsetBlocks + mod.offsetBlocks + relief * mod.amplitudeScale;
    }

    const int units = static_cast<int>(std::lround(heightBlocks * static_cast<float>(kHeightUnitsPerBlock)));
    return static_cast<Height>(std::clamp(units, kMinTerrainHeightUnits, kMaxTerrainHeightUnits));
}

float TerrainNoiseGenerator::VariationAt(std::int64_t worldX, std::int64_t worldZ) const noexcept {
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);
    return m_impl->variation.GetNoise(x, z);
}

float TerrainNoiseGenerator::LandformMaskAt(std::int64_t worldX, std::int64_t worldZ) const noexcept {
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);
    return (m_impl->landform.GetNoise(x, z) + 1.0F) * 0.5F;
}

float TerrainNoiseGenerator::OverhangAt(float worldX, float worldY, float worldZ) const noexcept {
    return m_impl->overhang.GetNoise(worldX, worldY, worldZ);
}

float TerrainNoiseGenerator::CaveCarveAt(float worldX, float worldY, float worldZ) const noexcept {
    const TerrainCaveParams& params = m_impl->params.caves;
    if (!params.enabled) {
        return 0.0F;  // 关闭 ⇒ 恒 0 ⇒ 壳的密度与 W4 逐位一致（既有测试不改）
    }

    const float a = m_impl->caveA.GetNoise(worldX, worldY, worldZ);
    const float b = m_impl->caveB.GetNoise(worldX, worldY, worldZ);
    const float d = std::sqrt(a * a + b * b);

    // `d < radius`：位于隧道轴线的邻域内 ⇒ 按"离轴线多近"给出雕刻量（0 ~ carveStrengthBlocks）。
    if (!(d < params.tunnelRadius)) {
        return 0.0F;
    }
    return (params.tunnelRadius - d) / params.tunnelRadius * params.carveStrengthBlocks;
}

float TerrainNoiseGenerator::RiverJitterAt(float worldX, float worldZ) const noexcept {
    return m_impl->riverJitter.GetNoise(worldX, worldZ);
}

}  // namespace vx
