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

}  // namespace

float MaterialBandFactor(float value, float min, float max, float blend) noexcept {
    if (blend <= 0.0F) {
        return (value >= min && value <= max) ? 1.0F : 0.0F;
    }
    return SmoothStep(min - blend, min, value) * (1.0F - SmoothStep(max, max + blend, value));
}

float TriplanarBlendWeight(float slope, const TriplanarSettings& settings) noexcept {
    if (!settings.enabled) {
        return 0.0F;  // 关闭：恒为 0 → 着色器整段走平面路径（零额外采样）
    }
    // 与着色器 triplanarWeight 逐字镜像：clamp 到 [0,1] 后走同一 smoothstep。
    return SmoothStep(settings.slopeMin, settings.slopeMax, std::clamp(slope, 0.0F, 1.0F));
}

std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> ComputeBlendWeights(const TerrainMaterialTable& table,
                                                                                    float heightBlocks,
                                                                                    float slope) noexcept {
    const float clampedSlope = std::clamp(slope, 0.0F, 1.0F);

    std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> weights {};
    float total = 0.0F;
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const MaterialLayer& layer = table.Layer(slot);
        const float weight = MaterialBandFactor(heightBlocks, layer.heightMin, layer.heightMax, layer.heightBlend) *
                             MaterialBandFactor(clampedSlope, layer.slopeMin, layer.slopeMax, layer.slopeBlend);
        weights[static_cast<std::size_t>(slot)] = weight;
        total += weight;
    }

    if (total <= kWeightEpsilon) {
        // 无槽位匹配：退化为槽位 0，仍满足"权重非负且和为 1"。
        //
        // ⚠ 警示：这是**给异常输入的兜底**，不应在正常地形上大面积触发（ADR 0009 / 缺陷 2）。
        // 触发它意味着 `assets/config/materials.toml` 的高度 / 坡度带出现**覆盖空洞**——某 (高度, 坡度)
        // 处四层隶属度全为 0，于是整片区域被强制涂成槽位 0（草）的颜色。近期就发生过一次（高度 >112 的
        // 平地全落此分支）。排查方法：在材料高度 × 坡度网格上核对各层带的并集是否覆盖全域
        // （见 tests/terrain_material_test.cpp 的不变量测试）。此处是热路径，不打印日志，只在源头留警示。
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
