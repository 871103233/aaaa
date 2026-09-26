#include "terrain/material_blender.hpp"

#include "generation/seed.hpp"

#include <algorithm>
#include <cstddef>

#include "FastNoiseLite.h"  // third_party/FastNoiseLite（vendored，MIT）

namespace vx {
namespace {

/// 材质抖动的通道种子编号（与地形高度使用的通道错开，见 generation/terrain_noise.cpp）。
constexpr std::uint64_t kChannelMaterialVariation = 4;

/// 抖动频率：约每 20 格一次起伏，肉眼可见地打散直边。
constexpr float kVariationFrequency = 0.05F;

/// 高度抖动幅度（格）：在过渡带里把高度上下浮动几格，使边界呈自然波纹。
constexpr float kVariationAmplitudeBlocks = 3.0F;

constexpr float kWeightEpsilon = 1.0F / 1000000.0F;

/// 平滑阶跃：`value <= edge0` 返回 0、`value >= edge1` 返回 1，中间用三次曲线过渡。
[[nodiscard]] float SmoothStep(float edge0, float edge1, float value) noexcept {
    if (edge1 <= edge0) {
        return (value >= edge1) ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/// 一条「带」的隶属度：带内为 1，带外经 `blend` 宽度平滑归零。
[[nodiscard]] float BandFactor(float value, float min, float max, float blend) noexcept {
    if (blend <= 0.0F) {
        return (value >= min && value <= max) ? 1.0F : 0.0F;
    }
    return SmoothStep(min - blend, min, value) * (1.0F - SmoothStep(max, max + blend, value));
}

}  // namespace

std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> ComputeBlendWeights(const TerrainMaterialTable& table,
                                                                                    float heightBlocks,
                                                                                    float slope) noexcept {
    const float clampedSlope = std::clamp(slope, 0.0F, 1.0F);

    std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> weights {};
    float total = 0.0F;
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const MaterialLayer& layer = table.Layer(slot);
        const float weight = BandFactor(heightBlocks, layer.heightMin, layer.heightMax, layer.heightBlend) *
                             BandFactor(clampedSlope, layer.slopeMin, layer.slopeMax, layer.slopeBlend);
        weights[static_cast<std::size_t>(slot)] = weight;
        total += weight;
    }

    if (total <= kWeightEpsilon) {
        // 无槽位匹配：退化为槽位 0，仍满足"权重非负且和为 1"。
        weights.fill(0.0F);
        weights[0] = 1.0F;
        return weights;
    }

    for (float& weight : weights) {
        weight /= total;
    }
    return weights;
}

struct MaterialBlender::Impl {
    FastNoiseLite variation;
};

MaterialBlender::MaterialBlender(std::uint64_t worldSeed) : m_impl(std::make_unique<Impl>()) {
    m_impl->variation.SetSeed(DeriveChannelSeed(worldSeed, kChannelMaterialVariation));
    m_impl->variation.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    m_impl->variation.SetFrequency(kVariationFrequency);
}

MaterialBlender::~MaterialBlender() = default;

MaterialBlender::MaterialBlender(MaterialBlender&&) noexcept = default;
MaterialBlender& MaterialBlender::operator=(MaterialBlender&&) noexcept = default;

std::array<float, static_cast<std::size_t>(kMaterialSlotCount)>
MaterialBlender::WeightsAt(const TerrainMaterialTable& table, float worldX, float worldZ, float heightBlocks,
                           float slope) const noexcept {
    const float variation = m_impl->variation.GetNoise(worldX, worldZ) * kVariationAmplitudeBlocks;
    return ComputeBlendWeights(table, heightBlocks + variation, slope);
}

}  // namespace vx
