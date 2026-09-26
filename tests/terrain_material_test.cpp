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
using vx::MaterialBandFactor;
using vx::MaterialBlender;
using vx::MaterialLayer;
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

/// 以 `step` 扫描 `[start, end]`，返回 `fn` 取值落在开区间 `(0, 1)` 的跨度——即「过渡带」宽度。
/// 用于把"窄带"变成可测数字（片元着色器与 `ComputeBlendWeights` 用的是同一个 `MaterialBandFactor`）。
template <typename Fn>
[[nodiscard]] float TransitionWidth(float start, float step, float end, Fn fn) {
    float first = 0.0F;
    float last  = 0.0F;
    bool  found = false;
    for (float value = start; value <= end; value += step) {
        const float factor = fn(value);
        if (factor > 0.0F && factor < 1.0F) {
            if (!found) {
                first = value;
                found = true;
            }
            last = value;
        }
    }
    return found ? (last - first) : 0.0F;
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
        EXPECT_FLOAT_EQ(table.Layer(slot).uvScale, fallback.Layer(slot).uvScale) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).tintR, fallback.Layer(slot).tintR) << "slot=" << slot;
    }
}

// T19 / ADR 0009：过渡带必须是**窄带**——带宽由表里的 *_blend 决定，而不是跨整段 [min, max] 的斜坡。
// 这是"宽色带消失"的可测代理：片元着色器与 CPU 的 `ComputeBlendWeights` 用的是同一个
// `MaterialBandFactor`（逐字镜像，见 mesh.frag 的 bandFactor）。
TEST(TerrainMaterial, TransitionBandWidthMatchesConfiguredNarrowBand) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    // 高度带上边界：在 [height_max, height_max + height_blend] 内从 1 降到 0。
    const MaterialLayer& grass = table.Layer(0);
    const float          heightEdge =
        TransitionWidth(grass.heightMax, 0.005F, grass.heightMax + grass.heightBlend + 0.5F, [&grass](float value) {
            return MaterialBandFactor(value, grass.heightMin, grass.heightMax, grass.heightBlend);
        });
    EXPECT_GT(heightEdge, 0.0F);
    EXPECT_NEAR(heightEdge, grass.heightBlend, 0.05F) << "高度带过渡宽度必须等于配置的 height_blend";

    // 坡度带上边界：在 [slope_max, slope_max + slope_blend] 内从 1 降到 0。
    const MaterialLayer& rock      = table.Layer(2);
    const float          slopeEdge =
        TransitionWidth(rock.slopeMax, 0.0005F, rock.slopeMax + rock.slopeBlend + 0.05F, [&rock](float value) {
            return MaterialBandFactor(value, rock.slopeMin, rock.slopeMax, rock.slopeBlend);
        });
    EXPECT_GT(slopeEdge, 0.0F);
    EXPECT_NEAR(slopeEdge, rock.slopeBlend, 0.005F) << "坡度带过渡宽度必须等于配置的 slope_blend";
}

// 回归：窄带合计宽度必须**远窄于**旧做法（把变化摊在整段量程上的线性斜坡）。旧实现逐顶点插值把
// 整段变化摊在约 1 格宽的三角形里，形成明显色带；新带宽只由 *_blend 决定。
// 参照"全域"取各量自身的完整量程：坡度 ∈ [0, 1]，高度 ∈ 世界垂直范围 [0, 512]（ADR 0008）。
TEST(TerrainMaterial, NarrowBandIsMuchNarrowerThanFullRangeRamp) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    // 坡度：全域 = [0, 1]（ADR 0009 的主要场景是岩壁处的坡度过渡）。
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const float narrowBand = 2.0F * table.Layer(slot).slopeBlend;
        EXPECT_LT(narrowBand, 0.5F) << "slot=" << slot << " 坡度窄带必须远窄于全域 0..1";
    }

    // 高度：全域 = 世界垂直范围（ADR 0008：0~512 格）。
    constexpr float kWorldHeightRange = 512.0F;
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const float narrowBand = 2.0F * table.Layer(slot).heightBlend;
        EXPECT_LT(narrowBand, kWorldHeightRange * 0.5F) << "slot=" << slot << " 高度窄带必须远窄于世界垂直范围";
    }
}

// T19 / ADR 0009：GPU uniform 块必须由**加载出来的材质表**投影得到，禁止手抄第二份常量。
// 写一份与原表不同的临时 TOML，加载后构建 uniform，断言 uniform 逐字段等于该表的值，
// 且与内置默认表给出的 uniform **不同**（证明它确实随表变化）。
TEST(TerrainMaterial, UniformIsDerivedFromLoadedTableNotHandCopied) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vx_modified_materials.toml";
    {
        std::ofstream out(path, std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "schema_version = 2\n";
        // 槽位 0 刻意改掉 height_max / uv_scale / tint；其余层给合法值。
        out << "[[layer]]\nname = \"grass\"\ntexture_layer = 1\nheight_min = 0.0\nheight_max = 111.0\n"
               "height_blend = 16.0\nslope_min = 0.0\nslope_max = 0.35\nslope_blend = 0.10\n"
               "uv_scale = 0.99\ntint_r = 0.11\ntint_g = 0.22\ntint_b = 0.33\n";
        out << "[[layer]]\nname = \"dirt\"\ntexture_layer = 2\nheight_min = 0.0\nheight_max = 160.0\n"
               "height_blend = 24.0\nslope_min = 0.20\nslope_max = 0.60\nslope_blend = 0.15\n"
               "uv_scale = 0.10\ntint_r = 0.45\ntint_g = 0.33\ntint_b = 0.21\n";
        out << "[[layer]]\nname = \"rock\"\ntexture_layer = 3\nheight_min = 40.0\nheight_max = 512.0\n"
               "height_blend = 24.0\nslope_min = 0.45\nslope_max = 1.0\nslope_blend = 0.15\n"
               "uv_scale = 0.16\ntint_r = 0.55\ntint_g = 0.55\ntint_b = 0.56\n";
        out << "[[layer]]\nname = \"sand\"\ntexture_layer = 4\nheight_min = 0.0\nheight_max = 6.0\n"
               "height_blend = 3.0\nslope_min = 0.0\nslope_max = 0.30\nslope_blend = 0.10\n"
               "uv_scale = 0.18\ntint_r = 0.83\ntint_g = 0.74\ntint_b = 0.48\n";
    }

    const TerrainMaterialTable table   = TerrainMaterialTable::LoadFromFile(path);
    const vx::MaterialUniform  uniform = vx::BuildMaterialUniform(table, 12.0, 34.0, 56.0);

    EXPECT_FLOAT_EQ(uniform.renderOriginX, 12.0F);
    EXPECT_FLOAT_EQ(uniform.renderOriginY, 34.0F);
    EXPECT_FLOAT_EQ(uniform.renderOriginZ, 56.0F);
    EXPECT_EQ(sizeof(vx::MaterialUniform), static_cast<std::size_t>(16 + 48 * kMaterialSlotCount));

    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const MaterialLayer&            layer = table.Layer(slot);
        const vx::MaterialLayerUniform& out   = uniform.layers[static_cast<std::size_t>(slot)];

        EXPECT_FLOAT_EQ(out.heightMin, layer.heightMin) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.heightMax, layer.heightMax) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.heightBlend, layer.heightBlend) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.textureIndex, static_cast<float>(layer.textureLayer - 1)) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.slopeMin, layer.slopeMin) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.slopeMax, layer.slopeMax) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.slopeBlend, layer.slopeBlend) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.tintR, layer.tintR) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.tintG, layer.tintG) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.tintB, layer.tintB) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.uvScale, layer.uvScale) << "slot=" << slot;
    }

    const vx::MaterialUniform fallbackUniform =
        vx::BuildMaterialUniform(TerrainMaterialTable::Default(), 0.0, 0.0, 0.0);
    bool differs = false;
    for (std::size_t slot = 0; slot < uniform.layers.size(); ++slot) {
        if (uniform.layers[slot].uvScale != fallbackUniform.layers[slot].uvScale ||
            uniform.layers[slot].tintR != fallbackUniform.layers[slot].tintR ||
            uniform.layers[slot].heightMax != fallbackUniform.layers[slot].heightMax) {
            differs = true;
        }
    }
    EXPECT_TRUE(differs) << "uniform 必须随材质表变化（改动 TOML 必须改变 uniform），不能是手抄常量";

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
