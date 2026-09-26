// T19 / ADR 0009 的占位材质贴图回归：
//   - 生成必须是**确定性**的（同种子 ⇒ 逐字节相同，红线 7）；
//   - 不同种子必须产生不同字节（种子真的参与生成）；
//   - 数据尺寸与取值范围"san值"：两张等长、albedo 有变化、法线朝外；
//   - 生成的法线解码后必须是**单位长度**（误差仅来自 8 位量化）。

#include "terrain/material_textures.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using vx::GenerateMaterialTextures;
using vx::kMaterialSlotCount;
using vx::MaterialTextureSet;

constexpr std::uint64_t kSeed     = 0x5EED0019ULL;
constexpr std::uint32_t kTestSize = 32;

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
