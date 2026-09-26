#include "terrain/material_blender.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace {

using vx::ComputeBlendWeights;
using vx::kMaterialSlotCount;
using vx::MaterialBlender;
using vx::TerrainMaterialTable;

constexpr std::uint64_t kSeed = 0x5EED0001ULL;

using Weights = std::array<float, static_cast<std::size_t>(kMaterialSlotCount)>;

[[nodiscard]] float Sum(const Weights& weights) {
    float total = 0.0F;
    for (const float weight : weights) {
        total += weight;
    }
    return total;
}

[[nodiscard]] int DominantSlot(const Weights& weights) {
    int best = 0;
    for (int slot = 1; slot < kMaterialSlotCount; ++slot) {
        if (weights[static_cast<std::size_t>(slot)] > weights[static_cast<std::size_t>(best)]) {
            best = slot;
        }
    }
    return best;
}

}  // namespace

// T5 ①：任意高度 / 坡度下权重都必须归一化（和 ≈ 1）且非负。
TEST(TerrainMaterial, WeightsAreNormalizedAndNonNegative) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    for (const float height : { 0.0F, 5.0F, 40.0F, 120.0F, 400.0F }) {
        for (const float slope : { 0.0F, 0.2F, 0.5F, 0.8F, 1.0F }) {
            const Weights weights = ComputeBlendWeights(table, height, slope);
            EXPECT_NEAR(Sum(weights), 1.0F, 1e-4F) << "height=" << height << " slope=" << slope;
            for (const float weight : weights) {
                EXPECT_GE(weight, 0.0F) << "height=" << height << " slope=" << slope;
                EXPECT_LE(weight, 1.0F) << "height=" << height << " slope=" << slope;
            }
        }
    }
}

// T5 ②：陡坡 → 岩石主导；低平地面 → 草主导。
TEST(TerrainMaterial, SteepSlopeIsRockAndFlatLowGroundIsGrass) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    const Weights rock = ComputeBlendWeights(table, 100.0F, 0.9F);
    EXPECT_EQ(DominantSlot(rock), 2) << "陡坡应为岩（槽位 2）";
    EXPECT_GT(rock[2], 0.5F);

    const Weights grass = ComputeBlendWeights(table, 20.0F, 0.02F);
    EXPECT_EQ(DominantSlot(grass), 0) << "低平地面应为草（槽位 0）";
    EXPECT_GT(grass[0], 0.5F);
}

// T5 ③（确定性噪声抖动）：同一输入反复求值必须完全一致。
TEST(TerrainMaterial, NoiseVariationIsDeterministic) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    MaterialBlender first(kSeed);
    MaterialBlender second(kSeed);

    const Weights a = first.WeightsAt(table, 123.0F, -45.0F, 80.0F, 0.3F);
    const Weights b = second.WeightsAt(table, 123.0F, -45.0F, 80.0F, 0.3F);
    const Weights c = first.WeightsAt(table, 123.0F, -45.0F, 80.0F, 0.3F);

    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        EXPECT_FLOAT_EQ(a[static_cast<std::size_t>(slot)], b[static_cast<std::size_t>(slot)]);
        EXPECT_FLOAT_EQ(a[static_cast<std::size_t>(slot)], c[static_cast<std::size_t>(slot)]);
    }
    EXPECT_NEAR(Sum(a), 1.0F, 1e-4F);
}

// T5 ④：缺失配置必须显式报错，禁止静默回退。
TEST(TerrainMaterial, MissingConfigThrows) {
    EXPECT_THROW((void)TerrainMaterialTable::LoadFromFile("no_such_materials_file.toml"), std::runtime_error);
}

// T5 ④（非法配置）：schema_version 不符时必须报错，而不是默默接受。
TEST(TerrainMaterial, InvalidSchemaThrows) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vx_invalid_materials.toml";
    {
        std::ofstream out(path, std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "schema_version = 99\n";
    }

    EXPECT_THROW((void)TerrainMaterialTable::LoadFromFile(path), std::runtime_error);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// T5 ⑤：仓库中已提交的配置必须能加载并通过校验。
TEST(TerrainMaterial, LoadsCommittedConfig) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "materials.toml";

    const TerrainMaterialTable table = TerrainMaterialTable::LoadFromFile(path);
    EXPECT_EQ(table.SchemaVersion(), TerrainMaterialTable::kSchemaVersion);
    EXPECT_EQ(table.Layer(0).name, "grass");
    EXPECT_EQ(table.Layer(0).textureLayer, 1);

    // 与内置默认表一致：保证测试与运行期行为可比。
    const TerrainMaterialTable fallback = TerrainMaterialTable::Default();
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        EXPECT_EQ(table.Layer(slot).textureLayer, fallback.Layer(slot).textureLayer) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).heightMax, fallback.Layer(slot).heightMax) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).slopeMin, fallback.Layer(slot).slopeMin) << "slot=" << slot;
    }
}
