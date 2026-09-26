#include "generation/terrain_noise.hpp"

#include "generation/seed.hpp"

#include <algorithm>
#include <cmath>

#include "FastNoiseLite.h"  // third_party/FastNoiseLite（vendored，MIT）

namespace vx {
namespace {

// 各噪声层参数：低频决定大的丘陵走势，中/高频给表面细节。
constexpr float kBaseFrequency   = 0.0035F;
constexpr float kDetailFrequency = 0.015F;
constexpr float kRoughFrequency  = 0.06F;

constexpr float kBaseAmplitude   = 96.0F;
constexpr float kDetailAmplitude = 22.0F;
constexpr float kRoughAmplitude  = 4.0F;

/// 让基线地面落在世界垂直范围（0~512 格）的中段。
constexpr float kHeightOffset = 128.0F;

constexpr float kVariationFrequency = 0.05F;

/// 通道编号：同一全局种子下，各噪声层必须用不同通道，避免层间耦合（方案 §3.2）。
constexpr std::uint64_t kChannelBase      = 1;
constexpr std::uint64_t kChannelDetail    = 2;
constexpr std::uint64_t kChannelRough     = 3;
constexpr std::uint64_t kChannelVariation = 4;

}  // namespace

struct TerrainNoiseGenerator::Impl {
    FastNoiseLite base;
    FastNoiseLite detail;
    FastNoiseLite rough;
    FastNoiseLite variation;
};

TerrainNoiseGenerator::TerrainNoiseGenerator(std::uint64_t worldSeed) : m_impl(std::make_unique<Impl>()) {
    m_impl->base.SetSeed(DeriveChannelSeed(worldSeed, kChannelBase));
    m_impl->base.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->base.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->base.SetFractalOctaves(4);
    m_impl->base.SetFrequency(kBaseFrequency);

    m_impl->detail.SetSeed(DeriveChannelSeed(worldSeed, kChannelDetail));
    m_impl->detail.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->detail.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->detail.SetFractalOctaves(3);
    m_impl->detail.SetFrequency(kDetailFrequency);

    m_impl->rough.SetSeed(DeriveChannelSeed(worldSeed, kChannelRough));
    m_impl->rough.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->rough.SetFractalType(FastNoiseLite::FractalType_FBm);
    m_impl->rough.SetFractalOctaves(2);
    m_impl->rough.SetFrequency(kRoughFrequency);

    m_impl->variation.SetSeed(DeriveChannelSeed(worldSeed, kChannelVariation));
    m_impl->variation.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->variation.SetFrequency(kVariationFrequency);
}

TerrainNoiseGenerator::~TerrainNoiseGenerator() = default;

TerrainNoiseGenerator::TerrainNoiseGenerator(TerrainNoiseGenerator&&) noexcept = default;
TerrainNoiseGenerator& TerrainNoiseGenerator::operator=(TerrainNoiseGenerator&&) noexcept = default;

Height TerrainNoiseGenerator::HeightUnits(std::int64_t worldX, std::int64_t worldZ) const noexcept {
    // 世界定位仍由整数坐标承担；这里转 float 只是喂给噪声函数，不用于坐标存储（red line 6）。
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);

    const float heightBlocks = kHeightOffset +
                               m_impl->base.GetNoise(x, z) * kBaseAmplitude +
                               m_impl->detail.GetNoise(x, z) * kDetailAmplitude +
                               m_impl->rough.GetNoise(x, z) * kRoughAmplitude;

    const int units = static_cast<int>(std::lround(heightBlocks * static_cast<float>(kHeightUnitsPerBlock)));
    return static_cast<Height>(std::clamp(units, kMinTerrainHeightUnits, kMaxTerrainHeightUnits));
}

float TerrainNoiseGenerator::VariationAt(std::int64_t worldX, std::int64_t worldZ) const noexcept {
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);
    return m_impl->variation.GetNoise(x, z);
}

}  // namespace vx
