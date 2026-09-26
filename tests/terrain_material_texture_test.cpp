// T22 / ADR 0010 P2 的占位材质贴图回归（在 T19 的基础上扩展）：
//   - 生成必须是**确定性**的（同种子 ⇒ 逐字节相同，红线 7）；
//   - 不同种子必须产生不同字节（种子真的参与生成）；
//   - 数据尺寸与取值范围"san值"：五张图等长结构正确、albedo 有变化、法线朝外；
//   - 生成的法线解码后必须是**单位长度**（误差仅来自 8 位量化）；
//   - 新增 roughness / AO / macro 三张图：尺寸、层数、确定性、值域与非恒定；
//   - albedo 的**多尺度叠加**必须带来显著的高频能量（"细腻"的可测代理）。

#include "terrain/material_textures.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using vx::GenerateMaterialTextures;
using vx::kMaterialMacroLayerCount;
using vx::kMaterialSlotCount;
using vx::MaterialTextureSet;

constexpr std::uint64_t kSeed     = 0x5EED0019ULL;
constexpr std::uint32_t kTestSize = 32;

/// 取出某层 R 通道为 `size × size` 的浮点图（用于高频统计）。
[[nodiscard]] std::vector<double> ExtractRed(const std::vector<std::uint8_t>& rgba, std::uint32_t size,
                                             std::size_t layerOffset) {
    std::vector<double> out(static_cast<std::size_t>(size) * size, 0.0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<double>(rgba[(layerOffset + i) * 4U]);
    }
    return out;
}

/// 高频能量代理：相邻像素一阶差分的均方。单一低频结构时很小，叠加高频"颗粒"后显著变大。
[[nodiscard]] double HighFrequencyEnergy(const std::vector<double>& image, std::uint32_t size) {
    double      sum   = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x + 1U < size; ++x) {
            const double delta = image[static_cast<std::size_t>(y) * size + x + 1U] -
                                 image[static_cast<std::size_t>(y) * size + x];
            sum += delta * delta;
            ++count;
        }
    }
    return (count > 0) ? sum / static_cast<double>(count) : 0.0;
}

/// 3×3 盒式低通（边界用 clamp 延拓，避免边缘伪造出大的相邻差分）：
/// 近似去掉高频段，作为"叠加前（纯低频结构）"的高频能量对照。
[[nodiscard]] std::vector<double> BoxBlur3x3(const std::vector<double>& image, std::uint32_t size) {
    const auto clampIndex = [size](int v) {
        if (v < 0) {
            return std::size_t { 0 };
        }
        if (v >= static_cast<int>(size)) {
            return static_cast<std::size_t>(size) - 1U;
        }
        return static_cast<std::size_t>(v);
    };
    std::vector<double> out(image.size(), 0.0);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            double sum = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const std::size_t sx = clampIndex(static_cast<int>(x) + dx);
                    const std::size_t sy = clampIndex(static_cast<int>(y) + dy);
                    sum += image[sy * size + sx];
                }
            }
            out[static_cast<std::size_t>(y) * size + x] = sum / 9.0;
        }
    }
    return out;
}

/// 某层某通道是否非恒定（至少有一个像素与该层首像素不同）。
[[nodiscard]] bool LayerChannelVaries(const std::vector<std::uint8_t>& rgba, std::size_t layerBytes,
                                      std::size_t layerIndex, std::size_t channel) {
    const std::size_t begin = layerIndex * layerBytes;
    const std::uint8_t first = rgba[begin + channel];
    for (std::size_t i = begin + channel; i < begin + layerBytes; i += 4U) {
        if (rgba[i] != first) {
            return true;
        }
    }
    return false;
}

}  // namespace

// 同一 (种子, 尺寸) ⇒ 逐字节相同。
TEST(TerrainMaterialTexture, GenerationIsDeterministic) {
    const MaterialTextureSet first  = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet second = GenerateMaterialTextures(kSeed, kTestSize);

    EXPECT_EQ(first.albedoRgba, second.albedoRgba);
    EXPECT_EQ(first.normalRgba, second.normalRgba);
}

// 不同种子 ⇒ 不同字节。
TEST(TerrainMaterialTexture, DifferentSeedChangesBytes) {
    const MaterialTextureSet a = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet b = GenerateMaterialTextures(kSeed + 1U, kTestSize);

    EXPECT_NE(a.albedoRgba, b.albedoRgba);
    EXPECT_NE(a.normalRgba, b.normalRgba);
}

// 尺寸 / 层数 / 取值范围合理。
TEST(TerrainMaterialTexture, SizesAndRangesAreSane) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;
    const std::size_t expected   = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);

    EXPECT_EQ(set.size, kTestSize);
    EXPECT_EQ(set.layerCount, static_cast<std::uint32_t>(kMaterialSlotCount));
    EXPECT_EQ(set.albedoRgba.size(), expected);
    EXPECT_EQ(set.normalRgba.size(), expected);

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        const std::size_t begin = static_cast<std::size_t>(layer) * layerBytes;
        const std::size_t end   = begin + layerBytes;

        // albedo：每层都要有噪声变化，不能是纯色。
        const std::uint8_t firstRed = set.albedoRgba[begin];
        bool               varies   = false;
        for (std::size_t i = begin; i < end; i += 4U) {
            if (set.albedoRgba[i] != firstRed) {
                varies = true;
                break;
            }
        }
        EXPECT_TRUE(varies) << "layer=" << layer << " 的 albedo 不应是纯色";
    }
}

// 法线：解码后单位长度，且 z 分量朝外（> 0）。
TEST(TerrainMaterialTexture, GeneratedNormalsAreUnitLength) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    for (std::size_t i = 0; i + 3U < set.normalRgba.size(); i += 4U) {
        const float nx = static_cast<float>(set.normalRgba[i + 0U]) / 255.0F * 2.0F - 1.0F;
        const float ny = static_cast<float>(set.normalRgba[i + 1U]) / 255.0F * 2.0F - 1.0F;
        const float nz = static_cast<float>(set.normalRgba[i + 2U]) / 255.0F * 2.0F - 1.0F;

        EXPECT_NEAR(std::sqrt(nx * nx + ny * ny + nz * nz), 1.0F, 0.02F);
        EXPECT_GT(nz, 0.0F);
    }
}

// T22：roughness / AO / macro 三张新图的尺寸与层数。
TEST(TerrainMaterialTexture, NewArraysHaveExpectedSizeAndLayerCount) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;
    const std::size_t expected4   = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);
    const std::size_t expected1   = layerBytes * static_cast<std::size_t>(kMaterialMacroLayerCount);

    EXPECT_EQ(set.roughnessRgba.size(), expected4);
    EXPECT_EQ(set.aoRgba.size(), expected4);
    EXPECT_EQ(set.macroRgba.size(), expected1);
    EXPECT_EQ(kMaterialMacroLayerCount, 1U) << "宏观变化图必须是单层";
}

// T22：roughness / AO / macro 的确定性（同种子 ⇒ 逐字节相同；红线 7）。
TEST(TerrainMaterialTexture, NewArraysAreDeterministic) {
    const MaterialTextureSet first  = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet second = GenerateMaterialTextures(kSeed, kTestSize);

    EXPECT_EQ(first.roughnessRgba, second.roughnessRgba);
    EXPECT_EQ(first.aoRgba, second.aoRgba);
    EXPECT_EQ(first.macroRgba, second.macroRgba);
}

// T22：不同种子必须改变三张新图的字节（种子确实参与各通道派生）。
TEST(TerrainMaterialTexture, NewArraysDifferBySeed) {
    const MaterialTextureSet a = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet b = GenerateMaterialTextures(kSeed + 7U, kTestSize);

    EXPECT_NE(a.roughnessRgba, b.roughnessRgba);
    EXPECT_NE(a.aoRgba, b.aoRgba);
    EXPECT_NE(a.macroRgba, b.macroRgba);
}

// T22：三张新图必须是"有细节"的灰阶图（RGB 三通道相同、A = 255；R 通道非恒定）。
TEST(TerrainMaterialTexture, NewArraysAreGrayAndNonConstant) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);
    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;

    struct ArrayView {
        const std::vector<std::uint8_t>& rgba;
        std::size_t                      layerCount;
    };
    const ArrayView views[] = { { set.roughnessRgba, static_cast<std::size_t>(kMaterialSlotCount) },
                                { set.aoRgba, static_cast<std::size_t>(kMaterialSlotCount) },
                                { set.macroRgba, static_cast<std::size_t>(kMaterialMacroLayerCount) } };

    for (const ArrayView& view : views) {
        for (std::size_t layer = 0; layer < view.layerCount; ++layer) {
            for (std::size_t i = layer * layerBytes; i < (layer + 1U) * layerBytes; i += 4U) {
                EXPECT_EQ(view.rgba[i + 0U], view.rgba[i + 1U]);  // 灰阶：rgb 同值
                EXPECT_EQ(view.rgba[i + 1U], view.rgba[i + 2U]);
                EXPECT_EQ(view.rgba[i + 3U], 255U);               // a 恒为 255
            }
            EXPECT_TRUE(LayerChannelVaries(view.rgba, layerBytes, layer, 0U)) << "layer=" << layer << " 不应恒定";
        }
    }
}

// T22：AO 细节值域必须落在 [0.6, 1.0]（1 = 完全开阔），避免把环境项压黑到 0。
TEST(TerrainMaterialTexture, AoDetailStaysInConfiguredRange) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    for (std::size_t i = 0; i + 3U < set.aoRgba.size(); i += 4U) {
        const double value = static_cast<double>(set.aoRgba[i]) / 255.0;
        EXPECT_GE(value, 0.6 - 0.01);
        EXPECT_LE(value, 1.0 + 0.01);
    }
}

// T22 多尺度：albedo 由"低频结构 + 高频颗粒"两段叠加而成。用**相邻像素一阶差分的均方**作为
// 高频能量代理：叠加后必须显著大于低通近似（≈ 只保留低频结构，即"叠加前"）的高频能量。
//
// 为什么这份用例用 128² 而非 32²：噪声周期是"整张贴图内的特征数"，32² 下高频段（周期 30）也只有
// 约 1 个周期，逐像素差分根本体现不出"高频"，测不出频段差异；128² 下高频段约 4 个像素一个特征，
// 与低频段（约 13 个像素）在频谱上明显分开。
TEST(TerrainMaterialTexture, AlbedoMultiScaleAddsHighFrequencyEnergy) {
    constexpr std::uint32_t kMultiScaleSize = 128;
    const MaterialTextureSet set            = GenerateMaterialTextures(kSeed, kMultiScaleSize);
    const std::size_t        layerPixels    = static_cast<std::size_t>(kMultiScaleSize) * kMultiScaleSize;

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        const std::vector<double> red =
            ExtractRed(set.albedoRgba, kMultiScaleSize, static_cast<std::size_t>(layer) * layerPixels);
        const std::vector<double> lowOnly = BoxBlur3x3(red, kMultiScaleSize);

        const double highFrequency    = HighFrequencyEnergy(red, kMultiScaleSize);
        const double lowFrequencyBase = HighFrequencyEnergy(lowOnly, kMultiScaleSize);

        EXPECT_GT(highFrequency, 0.0) << "layer=" << layer << " albedo 必须有细节";
        EXPECT_GT(highFrequency, lowFrequencyBase * 2.0)
            << "layer=" << layer << " 多尺度叠加后的高频能量必须显著大于纯低频结构（叠加前）";
    }
}
