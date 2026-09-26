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
constexpr std::uint64_t kAlbedoChannelBase = 10;  ///< 10~13：4 层的 albedo 细节
constexpr std::uint64_t kNormalChannelBase = 20;  ///< 20~23：4 层的法线高度场

/// 每层风格参数，层序与材质槽一致（0 草 / 1 土 / 2 岩 / 3 沙）。
constexpr float kAlbedoPeriods[kMaterialSlotCount]  = { 10.0F, 6.0F, 7.0F, 14.0F };
constexpr float kAlbedoBase[kMaterialSlotCount]     = { 0.72F, 0.66F, 0.50F, 0.78F };
constexpr float kAlbedoRange[kMaterialSlotCount]    = { 0.28F, 0.34F, 0.50F, 0.22F };
constexpr float kHeightPeriods[kMaterialSlotCount]  = { 12.0F, 8.0F, 9.0F, 18.0F };
constexpr float kNormalStrength[kMaterialSlotCount] = { 6.0F, 8.0F, 14.0F, 4.0F };

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

}  // namespace

MaterialTextureSet GenerateMaterialTextures(std::uint64_t worldSeed, std::uint32_t size) {
    MaterialTextureSet set;
    set.size       = size;
    set.layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);

    const std::size_t layerPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    const std::size_t totalBytes  = layerPixels * static_cast<std::size_t>(kMaterialSlotCount) * 4U;
    set.albedoRgba.assign(totalBytes, 0U);
    set.normalRgba.assign(totalBytes, 255U);  // a 通道恒为 255；rgb 稍后覆盖

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        FastNoiseLite albedoNoise;
        FastNoiseLite heightNoise;
        ConfigureNoise(albedoNoise, DeriveChannelSeed(worldSeed, kAlbedoChannelBase + static_cast<std::uint64_t>(layer)));
        ConfigureNoise(heightNoise, DeriveChannelSeed(worldSeed, kNormalChannelBase + static_cast<std::uint64_t>(layer)));

        const std::size_t layerOffset = static_cast<std::size_t>(layer) * layerPixels;
        std::vector<float> heights(layerPixels, 0.0F);

        for (std::uint32_t y = 0; y < size; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(size);
            for (std::uint32_t x = 0; x < size; ++x) {
                const float u      = static_cast<float>(x) / static_cast<float>(size);
                const float detail = SeamlessNoise(albedoNoise, u, v, kAlbedoPeriods[layer]);

                const std::size_t texel  = layerOffset + static_cast<std::size_t>(y) * size + x;
                heights[texel - layerOffset] = SeamlessNoise(heightNoise, u, v, kHeightPeriods[layer]);

                float luminance = kAlbedoBase[layer] + kAlbedoRange[layer] * detail;
                if (layer == 2) {
                    // 岩：ridge（脊状）噪声，暗缝 + 亮脊，读起来像开裂的岩石。
                    luminance = kAlbedoBase[layer] + kAlbedoRange[layer] * (1.0F - std::fabs(detail));
                }
                const std::uint8_t value = EncodeUnorm(luminance);
                const std::size_t  base  = texel * 4U;
                set.albedoRgba[base + 0] = value;
                set.albedoRgba[base + 1] = value;
                set.albedoRgba[base + 2] = value;
                set.albedoRgba[base + 3] = 255U;
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

    return set;
}

}  // namespace vx
