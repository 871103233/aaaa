#include "terrain/material_textures.hpp"

#include "generation/seed.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include "FastNoiseLite.h"  // third_party/FastNoiseLite（vendored，MIT）

namespace vx {
namespace {

/// 噪声通道基号：与地形高度（1~3）、材质抖动（4）错开，避免层间耦合（方案 §3.2）。
/// 每个"频段 / 用途"占一段连续的 4 个通道（按层递增），保证层与层、频段与频段都不同种子。
constexpr std::uint64_t kAlbedoLowChannelBase  = 10;  ///< 10~13：albedo 低频"结构"
constexpr std::uint64_t kAlbedoHighChannelBase = 30;  ///< 30~33：albedo 高频"颗粒"
constexpr std::uint64_t kNormalLowChannelBase  = 20;  ///< 20~23：法线高频段所用的低频高度
constexpr std::uint64_t kNormalHighChannelBase = 40;  ///< 40~43：法线低频段所用的高频高度
constexpr std::uint64_t kRoughnessChannelBase  = 50;  ///< 50~53：逐层粗糙度变化
constexpr std::uint64_t kAoChannelBase         = 60;  ///< 60~63：逐层 AO 细节
constexpr std::uint64_t kMacroChannel          = 70;  ///< 70：宏观变化（单层）

/// 多尺度叠加权重（ADR 0010 P2）：低频"结构"占主导、高频"颗粒"提供细节。
/// 单频噪声无论振幅多大都会"看起来平"，必须叠加高频段（见 ADR 0010 背景诊断 #4）。
constexpr float kLowBandWeight  = 0.65F;
constexpr float kHighBandWeight = 0.35F;

/// 每层风格参数，层序与材质槽一致（0 草 / 1 土 / 2 岩 / 3 沙）。
/// 高频段的周期约为低频段的 3 倍，保证两个频段在频谱上分开。
constexpr float kAlbedoLowPeriods[kMaterialSlotCount]  = { 10.0F, 6.0F, 7.0F, 14.0F };
constexpr float kAlbedoHighPeriods[kMaterialSlotCount] = { 30.0F, 18.0F, 21.0F, 42.0F };
constexpr float kAlbedoBase[kMaterialSlotCount]        = { 0.72F, 0.66F, 0.50F, 0.78F };
constexpr float kAlbedoRange[kMaterialSlotCount]       = { 0.28F, 0.34F, 0.50F, 0.22F };
constexpr float kHeightLowPeriods[kMaterialSlotCount]  = { 12.0F, 8.0F, 9.0F, 18.0F };
constexpr float kHeightHighPeriods[kMaterialSlotCount] = { 36.0F, 24.0F, 27.0F, 54.0F };
constexpr float kNormalStrength[kMaterialSlotCount]    = { 6.0F, 8.0F, 14.0F, 4.0F };
constexpr float kRoughnessPeriods[kMaterialSlotCount]  = { 16.0F, 10.0F, 12.0F, 24.0F };
constexpr float kAoPeriods[kMaterialSlotCount]         = { 8.0F, 6.0F, 7.0F, 12.0F };

/// 宏观变化：低频大尺度（周期数少 = 特征大）；与各层的 `macro_uv_scale`（远小于 `uv_scale`）配合，
/// 在约 30~60 格的尺度上起伏，用于打破基础贴图约 6~10 格一次的重复感。
constexpr float kMacroPeriods = 3.0F;

/// AO 细节值域：`[kAoFloor, 1]`——贴图下限不压到 0（真正的强度由材质表的 `ao` 决定）。
constexpr float kAoFloor = 0.60F;

/// 配置一个噪声实例：OpenSimplex2 + FBm，频率固定为 1（坐标以「特征数」为单位传入）。
void ConfigureNoise(FastNoiseLite& noise, std::int32_t seed) {
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(4);
    noise.SetFrequency(1.0F);
}

/// tile 上无缝采样：把噪声与自身按整块平移的四份按双线性权重叠起来，边界周期连续。
[[nodiscard]] float SeamlessNoise(const FastNoiseLite& noise, float u, float v, float periods) noexcept {
    const float x = u * periods;
    const float y = v * periods;
    const float n00 = noise.GetNoise(x, y);
    const float n10 = noise.GetNoise(x - periods, y);
    const float n01 = noise.GetNoise(x, y - periods);
    const float n11 = noise.GetNoise(x - periods, y - periods);
    return n00 * (1.0F - u) * (1.0F - v) + n10 * u * (1.0F - v) + n01 * (1.0F - u) * v + n11 * u * v;
}

[[nodiscard]] std::uint8_t EncodeUnorm(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

/// 把灰阶值写入 RGBA 像素的 rgb 三通道，a 固定 255（贴图只承载细节，颜色由 tint 决定）。
void WriteGray(std::vector<std::uint8_t>& rgba, std::size_t pixelIndex, std::uint8_t value) noexcept {
    const std::size_t base = pixelIndex * 4U;
    rgba[base + 0U]        = value;
    rgba[base + 1U]        = value;
    rgba[base + 2U]        = value;
    rgba[base + 3U]        = 255U;
}

}  // namespace

MaterialTextureSet GenerateMaterialTextures(std::uint64_t worldSeed, std::uint32_t size) {
    MaterialTextureSet set;
    set.size       = size;
    set.layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);

    const std::size_t layerPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    const std::size_t layerBytes  = layerPixels * 4U;
    const std::size_t totalBytes  = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);

    set.albedoRgba.assign(totalBytes, 0U);
    set.normalRgba.assign(totalBytes, 255U);  // a 通道恒为 255；rgb 稍后覆盖
    set.roughnessRgba.assign(totalBytes, 255U);
    set.aoRgba.assign(totalBytes, 255U);
    set.macroRgba.assign(layerBytes * static_cast<std::size_t>(kMaterialMacroLayerCount), 255U);

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        FastNoiseLite albedoLow;
        FastNoiseLite albedoHigh;
        FastNoiseLite heightLow;
        FastNoiseLite heightHigh;
        FastNoiseLite roughnessNoise;
        FastNoiseLite aoNoise;
        const std::uint64_t channel = static_cast<std::uint64_t>(layer);
        ConfigureNoise(albedoLow, DeriveChannelSeed(worldSeed, kAlbedoLowChannelBase + channel));
        ConfigureNoise(albedoHigh, DeriveChannelSeed(worldSeed, kAlbedoHighChannelBase + channel));
        ConfigureNoise(heightLow, DeriveChannelSeed(worldSeed, kNormalLowChannelBase + channel));
        ConfigureNoise(heightHigh, DeriveChannelSeed(worldSeed, kNormalHighChannelBase + channel));
        ConfigureNoise(roughnessNoise, DeriveChannelSeed(worldSeed, kRoughnessChannelBase + channel));
        ConfigureNoise(aoNoise, DeriveChannelSeed(worldSeed, kAoChannelBase + channel));

        const std::size_t layerOffset = static_cast<std::size_t>(layer) * layerPixels;
        std::vector<float> heights(layerPixels, 0.0F);

        for (std::uint32_t y = 0; y < size; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(size);
            for (std::uint32_t x = 0; x < size; ++x) {
                const float u         = static_cast<float>(x) / static_cast<float>(size);
                const float structure = SeamlessNoise(albedoLow, u, v, kAlbedoLowPeriods[layer]);
                const float grain     = SeamlessNoise(albedoHigh, u, v, kAlbedoHighPeriods[layer]);

                // 两个频段叠加：低频"结构" + 高频"颗粒"。岩层对低频段取 ridge（脊状）塑形，
                // 暗缝 + 亮脊，读起来像开裂的岩石；高频段仍按普通颗粒叠加。
                const float lowShape = (layer == 2) ? (1.0F - std::fabs(structure)) : structure;
                const float detail   = kLowBandWeight * lowShape + kHighBandWeight * grain;

                const std::uint8_t value = EncodeUnorm(kAlbedoBase[layer] + kAlbedoRange[layer] * detail);
                WriteGray(set.albedoRgba, layerOffset + static_cast<std::size_t>(y) * size + x, value);

                // 高度场同样双频叠加：法线因此同时承载"大起伏"与"细小颗粒"。
                heights[static_cast<std::size_t>(y) * size + x] =
                    kLowBandWeight * SeamlessNoise(heightLow, u, v, kHeightLowPeriods[layer]) +
                    kHighBandWeight * SeamlessNoise(heightHigh, u, v, kHeightHighPeriods[layer]);

                // 粗糙度变化：居中在 0.5，着色器折算成 ±20% 乘子（±20% 之外仍由材质表的基准值缩放）。
                const float rough = 0.5F + 0.5F * SeamlessNoise(roughnessNoise, u, v, kRoughnessPeriods[layer]);
                WriteGray(set.roughnessRgba, layerOffset + static_cast<std::size_t>(y) * size + x,
                          EncodeUnorm(rough));

                // AO 细节：值域 [kAoFloor, 1]，1 = 完全开阔；强度由材质表的 ao 决定。
                const float aoFactor =
                    kAoFloor + (1.0F - kAoFloor) * (0.5F + 0.5F * SeamlessNoise(aoNoise, u, v, kAoPeriods[layer]));
                WriteGray(set.aoRgba, layerOffset + static_cast<std::size_t>(y) * size + x, EncodeUnorm(aoFactor));
            }
        }

        // 由高度场梯度求切线空间法线：中心差分 + 环绕索引（贴图本身无缝，平铺后也连续）。
        const std::uint32_t last = size - 1U;
        for (std::uint32_t y = 0; y < size; ++y) {
            const std::uint32_t yPrev = (y == 0U) ? last : (y - 1U);
            const std::uint32_t yNext = (y == last) ? 0U : (y + 1U);
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::uint32_t xPrev = (x == 0U) ? last : (x - 1U);
                const std::uint32_t xNext = (x == last) ? 0U : (x + 1U);

                const float du = heights[static_cast<std::size_t>(y) * size + xNext] -
                                 heights[static_cast<std::size_t>(y) * size + xPrev];
                const float dv = heights[static_cast<std::size_t>(yNext) * size + x] -
                                 heights[static_cast<std::size_t>(yPrev) * size + x];

                const glm::vec3 normal = glm::normalize(glm::vec3(-du * kNormalStrength[layer],
                                                                  -dv * kNormalStrength[layer], 1.0F));

                const std::size_t base = (layerOffset + static_cast<std::size_t>(y) * size + x) * 4U;
                set.normalRgba[base + 0] = EncodeUnorm(normal.x * 0.5F + 0.5F);
                set.normalRgba[base + 1] = EncodeUnorm(normal.y * 0.5F + 0.5F);
                set.normalRgba[base + 2] = EncodeUnorm(normal.z * 0.5F + 0.5F);
                set.normalRgba[base + 3] = 255U;
            }
        }
    }

    // 宏观变化：**1 层**低频大尺度噪声（存入 R 通道，rgb 同值便于调试）。
    FastNoiseLite macroNoise;
    ConfigureNoise(macroNoise, DeriveChannelSeed(worldSeed, kMacroChannel));
    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u     = static_cast<float>(x) / static_cast<float>(size);
            const float value = 0.5F + 0.5F * SeamlessNoise(macroNoise, u, v, kMacroPeriods);
            WriteGray(set.macroRgba, static_cast<std::size_t>(y) * size + x, EncodeUnorm(value));
        }
    }

    return set;
}

}  // namespace vx
