#include "render/mesh_renderer.hpp"
#include "terrain/material_blender.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using vx::ComputeBlendWeights;
using vx::kMaterialSlotCount;
using vx::MaterialBandFactor;
using vx::MaterialBlender;
using vx::MaterialLayer;
using vx::TerrainMaterialTable;
using vx::TriplanarBlendWeight;
using vx::TriplanarSettings;

constexpr std::uint64_t kSeed = 0x5EED0001ULL;

/// 一个材质层的全部字段（含 T22 新增的 roughness / ao / macro_*），用于拼临时 TOML。
struct LayerSpec {
    const char* name          = "x";
    int         textureLayer  = 1;
    double      heightMin     = 0.0;
    double      heightMax     = 100.0;
    double      heightBlend   = 10.0;
    double      slopeMin      = 0.0;
    double      slopeMax      = 1.0;
    double      slopeBlend    = 0.1;
    double      uvScale       = 0.1;
    double      tintR         = 0.5;
    double      tintG         = 0.5;
    double      tintB         = 0.5;
    double      roughness     = 0.8;
    double      ao            = 0.8;
    double      macroUvScale  = 0.02;
    double      macroStrength = 0.3;
};

/// 与内置默认表 / 仓库 TOML 一致的四个槽位；测试改其中的单个字段即可。
/// 带参数为缺陷 2 修复后的新值（覆盖 (高度 ∈ [0,512], 坡度 ∈ [0,1]) 全域）。
[[nodiscard]] std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> DefaultLayerSpecs() {
    return { LayerSpec { "grass", 1, 0.0, 160.0, 20.0, 0.0, 0.42, 0.12, 0.12, 0.31, 0.55, 0.24, 0.90, 0.85, 0.020, 0.35 },
             LayerSpec { "dirt", 2, 0.0, 200.0, 24.0, 0.18, 0.72, 0.16, 0.10, 0.45, 0.33, 0.21, 0.88, 0.75, 0.015, 0.40 },
             LayerSpec { "rock", 3, 96.0, 512.0, 112.0, 0.0, 1.0, 0.10, 0.16, 0.55, 0.55, 0.56, 0.40, 0.70, 0.030, 0.30 },
             LayerSpec { "sand", 4, 0.0, 6.0, 3.0, 0.0, 0.30, 0.10, 0.18, 0.83, 0.74, 0.48, 0.95, 0.90, 0.025, 0.25 } };
}

[[nodiscard]] std::string EmitMaterials(int schemaVersion,
                                        const std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)>& layers,
                                        bool triEnabled = true, double triSlopeMin = 0.45, double triSlopeMax = 0.65,
                                        double triSharpness = 4.0) {
    std::ostringstream out;
    out << "schema_version = " << schemaVersion << "\n";
    for (const LayerSpec& layer : layers) {
        out << "[[layer]]\n"
            << "name = \"" << layer.name << "\"\n"
            << "texture_layer = " << layer.textureLayer << "\n"
            << "height_min = " << layer.heightMin << "\n"
            << "height_max = " << layer.heightMax << "\n"
            << "height_blend = " << layer.heightBlend << "\n"
            << "slope_min = " << layer.slopeMin << "\n"
            << "slope_max = " << layer.slopeMax << "\n"
            << "slope_blend = " << layer.slopeBlend << "\n"
            << "uv_scale = " << layer.uvScale << "\n"
            << "tint_r = " << layer.tintR << "\n"
            << "tint_g = " << layer.tintG << "\n"
            << "tint_b = " << layer.tintB << "\n"
            << "roughness = " << layer.roughness << "\n"
            << "ao = " << layer.ao << "\n"
            << "macro_uv_scale = " << layer.macroUvScale << "\n"
            << "macro_strength = " << layer.macroStrength << "\n";
    }
    // C 项：全局三平面（triplanar）小节——与仓库 TOML 同源，供解析 / 投影 / 非法值测试覆盖。
    out << "[triplanar]\n"
        << "enabled = " << (triEnabled ? "true" : "false") << "\n"
        << "slope_min = " << triSlopeMin << "\n"
        << "slope_max = " << triSlopeMax << "\n"
        << "sharpness = " << triSharpness << "\n";
    return out.str();
}

/// 写一份临时材质表并返回路径；内容由调用方给出。
[[nodiscard]] std::filesystem::path WriteTempMaterials(const std::string& fileName, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream               out(path, std::ios::trunc);
    out << content;
    return path;
}

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
    // C 项：提交的 [triplanar] 段与配置一致。
    EXPECT_TRUE(table.Triplanar().enabled);
    EXPECT_FLOAT_EQ(table.Triplanar().slopeMin, 0.45F);
    EXPECT_FLOAT_EQ(table.Triplanar().slopeMax, 0.70F);
    EXPECT_FLOAT_EQ(table.Triplanar().sharpness, 4.0F);

    // 与内置默认表一致：保证测试与运行期行为可比。
    const TerrainMaterialTable fallback = TerrainMaterialTable::Default();
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        EXPECT_EQ(table.Layer(slot).textureLayer, fallback.Layer(slot).textureLayer) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).heightMax, fallback.Layer(slot).heightMax) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).slopeMin, fallback.Layer(slot).slopeMin) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).uvScale, fallback.Layer(slot).uvScale) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).tintR, fallback.Layer(slot).tintR) << "slot=" << slot;
        // T22 / ADR 0010 P2 新增字段：仓库 TOML 与内置默认表必须同源。
        EXPECT_FLOAT_EQ(table.Layer(slot).roughness, fallback.Layer(slot).roughness) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).ao, fallback.Layer(slot).ao) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).macroUvScale, fallback.Layer(slot).macroUvScale) << "slot=" << slot;
        EXPECT_FLOAT_EQ(table.Layer(slot).macroStrength, fallback.Layer(slot).macroStrength) << "slot=" << slot;
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

// T19 / ADR 0009 / T22：GPU uniform 块必须由**加载出来的材质表**投影得到，禁止手抄第二份常量。
// 写一份与原表不同的临时 TOML，加载后构建 uniform，断言 uniform 逐字段等于该表的值，
// 且与内置默认表给出的 uniform **不同**（证明它确实随表变化）。
// 同时钉死 T22 扩展后的 std140 布局字节数与新增字段的投影位置。
TEST(TerrainMaterial, UniformIsDerivedFromLoadedTableNotHandCopied) {
    std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
    // 槽位 0 刻意改掉 height_max / uv_scale / tint / roughness / ao / macro_*。
    specs[0].heightMax     = 111.0;
    specs[0].uvScale       = 0.99;
    specs[0].tintR         = 0.11;
    specs[0].tintG         = 0.22;
    specs[0].tintB         = 0.33;
    specs[0].roughness     = 0.33;
    specs[0].ao            = 0.44;
    specs[0].macroUvScale  = 0.077;
    specs[0].macroStrength = 0.66;

    const std::filesystem::path path =
        WriteTempMaterials("vx_modified_materials.toml", EmitMaterials(4, specs));

    const TerrainMaterialTable table   = TerrainMaterialTable::LoadFromFile(path);
    const vx::MaterialUniform  uniform = vx::BuildMaterialUniform(table, 12.0, 34.0, 56.0);

    EXPECT_FLOAT_EQ(uniform.renderOriginX, 12.0F);
    EXPECT_FLOAT_EQ(uniform.renderOriginY, 34.0F);
    EXPECT_FLOAT_EQ(uniform.renderOriginZ, 56.0F);
    // C 项布局：渲染原点 + 三平面参数（各 vec4）+ 每层 4 个 vec4 = 16 + 16 + 64×4 = 288 字节（≤ 512）。
    EXPECT_EQ(sizeof(vx::MaterialUniform), static_cast<std::size_t>(16 * (2 + 4 * kMaterialSlotCount)));
    EXPECT_EQ(sizeof(vx::MaterialUniform), static_cast<std::size_t>(288));
    EXPECT_LE(sizeof(vx::MaterialUniform), vx::kMaxMaterialUniformBytes);
    // C 项：三平面参数必须由同一份表投影（临时表未改 triplanar → 等于表内值）。
    EXPECT_FLOAT_EQ(uniform.triplanarEnabled, table.Triplanar().enabled ? 1.0F : 0.0F);
    EXPECT_FLOAT_EQ(uniform.triplanarSlopeMin, table.Triplanar().slopeMin);
    EXPECT_FLOAT_EQ(uniform.triplanarSlopeMax, table.Triplanar().slopeMax);
    EXPECT_FLOAT_EQ(uniform.triplanarSharpness, table.Triplanar().sharpness);

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
        // T22 新增字段：必须由 BuildMaterialUniform 投影，且落在约定的 vec4 位置。
        EXPECT_FLOAT_EQ(out.roughness, layer.roughness) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.ao, layer.ao) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.macroUvScale, layer.macroUvScale) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.macroStrength, layer.macroStrength) << "slot=" << slot;
        EXPECT_FLOAT_EQ(out.macroAoUnused, 0.0F) << "slot=" << slot;
    }

    // 新增字段确实随 TOML 变化（改的是槽位 0）。
    EXPECT_FLOAT_EQ(uniform.layers[0].roughness, 0.33F);
    EXPECT_FLOAT_EQ(uniform.layers[0].ao, 0.44F);
    EXPECT_FLOAT_EQ(uniform.layers[0].macroUvScale, 0.077F);
    EXPECT_FLOAT_EQ(uniform.layers[0].macroStrength, 0.66F);

    const vx::MaterialUniform fallbackUniform =
        vx::BuildMaterialUniform(TerrainMaterialTable::Default(), 0.0, 0.0, 0.0);
    bool differs = false;
    for (std::size_t slot = 0; slot < uniform.layers.size(); ++slot) {
        if (uniform.layers[slot].uvScale != fallbackUniform.layers[slot].uvScale ||
            uniform.layers[slot].tintR != fallbackUniform.layers[slot].tintR ||
            uniform.layers[slot].heightMax != fallbackUniform.layers[slot].heightMax ||
            uniform.layers[slot].roughness != fallbackUniform.layers[slot].roughness ||
            uniform.layers[slot].macroStrength != fallbackUniform.layers[slot].macroStrength) {
            differs = true;
        }
    }
    EXPECT_TRUE(differs) << "uniform 必须随材质表变化（改动 TOML 必须改变 uniform），不能是手抄常量";

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// T22 / ADR 0010 P2：新字段的合法解析（roughness / ao / macro_uv_scale / macro_strength 原样进入表）。
TEST(TerrainMaterial, NewPbrFieldsAreParsedFromToml) {
    std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
    specs[1].roughness     = 0.123;
    specs[1].ao            = 0.456;
    specs[1].macroUvScale  = 0.0789;
    specs[1].macroStrength = 0.654;

    const std::filesystem::path path =
        WriteTempMaterials("vx_pbr_fields_materials.toml", EmitMaterials(4, specs));
    const TerrainMaterialTable table = TerrainMaterialTable::LoadFromFile(path);

    EXPECT_FLOAT_EQ(table.Layer(1).roughness, 0.123F);
    EXPECT_FLOAT_EQ(table.Layer(1).ao, 0.456F);
    EXPECT_FLOAT_EQ(table.Layer(1).macroUvScale, 0.0789F);
    EXPECT_FLOAT_EQ(table.Layer(1).macroStrength, 0.654F);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// T22 校验：越界 / 非法的新字段必须在启动期抛异常（沿用既有"禁止静默回退"口径）。
TEST(TerrainMaterial, InvalidNewPbrFieldsThrow) {
    const auto loadWithBadField = [](const char* fileName, auto mutate) {
        std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
        mutate(specs[0]);
        const std::filesystem::path path = WriteTempMaterials(fileName, EmitMaterials(4, specs));
        const bool                  threw = [&path] {
            try {
                (void)TerrainMaterialTable::LoadFromFile(path);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        }();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return threw;
    };

    EXPECT_TRUE(loadWithBadField("vx_bad_roughness.toml", [](LayerSpec& s) { s.roughness = 1.5; }))
        << "roughness > 1 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_roughness_neg.toml", [](LayerSpec& s) { s.roughness = -0.01; }))
        << "roughness < 0 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_ao.toml", [](LayerSpec& s) { s.ao = 2.0; })) << "ao > 1 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_macro_uv.toml", [](LayerSpec& s) { s.macroUvScale = 0.0; }))
        << "macro_uv_scale == 0 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_macro_uv_neg.toml", [](LayerSpec& s) { s.macroUvScale = -0.1; }))
        << "macro_uv_scale < 0 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_macro_strength.toml", [](LayerSpec& s) { s.macroStrength = 1.01; }))
        << "macro_strength > 1 必须报错";
    EXPECT_TRUE(loadWithBadField("vx_bad_macro_strength_neg.toml", [](LayerSpec& s) { s.macroStrength = -0.5; }))
        << "macro_strength < 0 必须报错";
}

// T22 / ADR 0010 P2 / 缺陷 2：schema_version 必须等于 4（调整带 + 新增 [triplanar]，同一版本）；
// 旧版本（3）与新版本（5）都必须报错。
TEST(TerrainMaterial, SchemaVersionMustBeFour) {
    const std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
    for (const int version : { 3, 5 }) {
        const std::filesystem::path path =
            WriteTempMaterials("vx_schema_version_materials.toml", EmitMaterials(version, specs));
        EXPECT_THROW((void)TerrainMaterialTable::LoadFromFile(path), std::runtime_error)
            << "schema_version=" << version << " 必须报错";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    EXPECT_EQ(TerrainMaterialTable::kSchemaVersion, 4);
}

// ---- 缺陷 2 回归：材质带必须在 (高度 ∈ [0,512] × 坡度 ∈ [0,1]) 全域上被覆盖 ----
//
// 契约：权重"归一化且和 = 1"的兜底分支（无槽位匹配 → 槽位 0）是给异常输入的，**不应在正常地形上大面积
//       触发**（ADR 0009）。机制：旧带在高度 >112、坡度 0 处四层隶属度全 0 ⇒ 落兜底 ⇒ 整片平地被涂成草色。
//
// 注意：`ComputeBlendWeights` 会**归一化**（未落兜底时和恒为 1），故"和 > 阈值"无法区分兜底与正常。
// 这里直接按同一套 `MaterialBandFactor`（实现与着色器共用的唯一曲线）求**未归一化的覆盖总量**：
//   覆盖总量 = Σ_slot heightFactor(slot) × slopeFactor(slot)
// 它 > 阈值 ⟺ 至少一层匹配 ⟺ 不会落兜底。这是"覆盖无空洞"的可测代理。
namespace {

[[nodiscard]] float RawCoverage(const TerrainMaterialTable& table, float height, float slope) {
    float total = 0.0F;
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        const MaterialLayer& layer = table.Layer(slot);
        total += MaterialBandFactor(height, layer.heightMin, layer.heightMax, layer.heightBlend) *
                 MaterialBandFactor(slope, layer.slopeMin, layer.slopeMax, layer.slopeBlend);
    }
    return total;
}

}  // namespace

// 在 (高度 × 坡度) 网格上逐点断言覆盖总量 > 明确的阈值；修复前在 (120, 0) 等点必为 0（落兜底）。
TEST(TerrainMaterial, WholeHeightSlopeDomainHasNoCoverageHole) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    constexpr float kCoverageThreshold = 1.0e-3F;  // 覆盖总量的下限（远大于 kWeightEpsilon = 1e-6）
    constexpr float kHeightMax         = 512.0F;   // 世界垂直范围（ADR 0008）
    constexpr float kHeightStep        = 4.0F;
    constexpr float kSlopeStep         = 0.02F;

    for (float height = 0.0F; height <= kHeightMax; height += kHeightStep) {
        for (float slope = 0.0F; slope <= 1.0F + 1e-6F; slope += kSlopeStep) {
            const float coverage = RawCoverage(table, height, slope);
            EXPECT_GT(coverage, kCoverageThreshold)
                << "覆盖空洞：height=" << height << " slope=" << slope << "（修复前此点在旧带上为 0）";
        }
    }
}

// 仓库中已提交的 materials.toml 同样不得有覆盖空洞（防止只改内置表、漏改文件）。
TEST(TerrainMaterial, CommittedConfigHasNoCoverageHole) {
    const std::filesystem::path path =
        std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "config" / "materials.toml";
    const TerrainMaterialTable table = TerrainMaterialTable::LoadFromFile(path);

    constexpr float kCoverageThreshold = 1.0e-3F;
    for (float height = 0.0F; height <= 512.0F; height += 16.0F) {
        for (float slope = 0.0F; slope <= 1.0F + 1e-6F; slope += 0.05F) {
            EXPECT_GT(RawCoverage(table, height, slope), kCoverageThreshold)
                << "height=" << height << " slope=" << slope;
        }
    }
}

// 关键点的主导层必须符合设计意图：低平偏草、高海拔偏岩、陡坡露岩。
TEST(TerrainMaterial, KeyPointsFollowDesignIntent) {
    const TerrainMaterialTable table = TerrainMaterialTable::Default();

    // (120, 0)：测试地图基底平地（旧缺陷点）——设计意图 = 低平偏草（不再是兜底的"假草"）。
    EXPECT_EQ(DominantSlot(ComputeBlendWeights(table, 120.0F, 0.0F)), 0) << "(120, 0) 应主导为草（槽位 0）";

    // (512, 0)：世界最高处的**缓坡** —— 设计意图 = 高海拔偏**土（碎石土）**。
    // 注意：岩已改为**纯坡度驱动**（坡度带 [0.55, 1.0]，与高度无关），故高海拔**平地**由土接管；
    // 换来的是两条更重要的性质：平台平地是**纯草**（不是草岩灰）、低海拔陡壁是**灰岩**（不是土色）。
    const Weights top = ComputeBlendWeights(table, 512.0F, 0.0F);
    EXPECT_EQ(DominantSlot(top), 1) << "(512, 0) 应主导为土（槽位 1）";
    EXPECT_GT(top[1], 0.5F);

    // (0, 1)：最低处的垂直陡壁 —— 设计意图 = 陡坡露岩。
    const Weights cliff = ComputeBlendWeights(table, 0.0F, 1.0F);
    EXPECT_EQ(DominantSlot(cliff), 2) << "(0, 1) 应主导为岩（槽位 2）";
    EXPECT_GT(cliff[2], 0.5F);
}

// ---- C 项：陡壁三平面混合投影（triplanar）----
//
// 自动切换的契约：混合权重必须**逐像素由世界空间法线**算出（slope = 1 - |N.y|），
// 因此地形被笔刷挖 / 堆后，法线一变混合即自动跟随，无需 CPU 侧预烘焙 / 重建网格。
// 这里把该权重的 CPU 纯函数 TriplanarBlendWeight 钉成数值断言（与着色器 triplanarWeight 逐字镜像）。

// 坡度 0 → 0（纯平面）；slope ≥ slope_max → 1（完全三平面）；区间内单调不减。
TEST(TerrainMaterial, TriplanarBlendWeightIsZeroFlatAndOneSteep) {
    const TriplanarSettings settings {};  // enabled = true, slopeMin = 0.45, slopeMax = 0.70
    ASSERT_TRUE(settings.enabled);

    EXPECT_FLOAT_EQ(TriplanarBlendWeight(0.0F, settings), 0.0F) << "水平面必须是纯平面路径（零额外采样）";
    EXPECT_FLOAT_EQ(TriplanarBlendWeight(settings.slopeMin, settings), 0.0F);
    EXPECT_FLOAT_EQ(TriplanarBlendWeight(settings.slopeMax, settings), 1.0F);
    EXPECT_FLOAT_EQ(TriplanarBlendWeight(1.0F, settings), 1.0F);

    float previous = -1.0F;
    for (float slope = 0.0F; slope <= 1.0F + 1e-6F; slope += 0.01F) {
        const float weight = TriplanarBlendWeight(slope, settings);
        EXPECT_GE(weight, previous) << "slope=" << slope << " 必须单调不减";
        EXPECT_GE(weight, 0.0F);
        EXPECT_LE(weight, 1.0F);
        previous = weight;
    }
    // 区间内确有严格上升（否则"自动切换"就退化成硬开关）。
    EXPECT_LT(TriplanarBlendWeight(0.50F, settings), TriplanarBlendWeight(0.60F, settings));
    // 越界输入被钳制，不产生 NaN / 负值。
    EXPECT_FLOAT_EQ(TriplanarBlendWeight(-1.0F, settings), 0.0F);
    EXPECT_FLOAT_EQ(TriplanarBlendWeight(2.0F, settings), 1.0F);
}

// enabled = false 时恒为 0（着色器据此整段走平面路径）。
TEST(TerrainMaterial, TriplanarBlendWeightIsZeroWhenDisabled) {
    TriplanarSettings settings {};
    settings.enabled = false;

    for (float slope = 0.0F; slope <= 1.0F + 1e-6F; slope += 0.05F) {
        EXPECT_FLOAT_EQ(TriplanarBlendWeight(slope, settings), 0.0F) << "slope=" << slope;
    }
}

// 配置解析 + CPU→GPU 单入口投影：triplanar 参数必须原样进入表与 uniform，且随 TOML 变化。
TEST(TerrainMaterial, TriplanarSettingsParsedAndProjectedToUniform) {
    const std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
    const std::filesystem::path path =
        WriteTempMaterials("vx_triplanar_materials.toml", EmitMaterials(4, specs, false, 0.2, 0.5, 2.5));

    const TerrainMaterialTable table = TerrainMaterialTable::LoadFromFile(path);
    EXPECT_FALSE(table.Triplanar().enabled);
    EXPECT_FLOAT_EQ(table.Triplanar().slopeMin, 0.2F);
    EXPECT_FLOAT_EQ(table.Triplanar().slopeMax, 0.5F);
    EXPECT_FLOAT_EQ(table.Triplanar().sharpness, 2.5F);

    const vx::MaterialUniform uniform = vx::BuildMaterialUniform(table, 0.0, 0.0, 0.0);
    EXPECT_FLOAT_EQ(uniform.triplanarEnabled, 0.0F) << "enabled=false 必须投影为 0";
    EXPECT_FLOAT_EQ(uniform.triplanarSlopeMin, 0.2F);
    EXPECT_FLOAT_EQ(uniform.triplanarSlopeMax, 0.5F);
    EXPECT_FLOAT_EQ(uniform.triplanarSharpness, 2.5F);

    // 与内置默认表不同（证明它确实随配置变化，而非手抄常量）。
    const vx::MaterialUniform fallback = vx::BuildMaterialUniform(TerrainMaterialTable::Default(), 0.0, 0.0, 0.0);
    EXPECT_FLOAT_EQ(fallback.triplanarEnabled, 1.0F);
    EXPECT_NE(uniform.triplanarSlopeMax, fallback.triplanarSlopeMax);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// 非法 / 缺失的 triplanar 配置必须在启动期抛异常（禁止静默回退或钳制）。
TEST(TerrainMaterial, InvalidTriplanarSettingsThrow) {
    const std::array<LayerSpec, static_cast<std::size_t>(kMaterialSlotCount)> specs = DefaultLayerSpecs();
    const auto loadWithTriplanar = [&specs](const char* fileName, const std::string& content) {
        const std::filesystem::path path = WriteTempMaterials(fileName, content);
        const bool                  threw = [&path] {
            try {
                (void)TerrainMaterialTable::LoadFromFile(path);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        }();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return threw;
    };

    EXPECT_TRUE(loadWithTriplanar("vx_tri_min_gt_max.toml", EmitMaterials(4, specs, true, 0.7, 0.5, 4.0)))
        << "slope_min > slope_max 必须报错";
    EXPECT_TRUE(loadWithTriplanar("vx_tri_min_eq_max.toml", EmitMaterials(4, specs, true, 0.5, 0.5, 4.0)))
        << "slope_min == slope_max（smoothstep 退化）必须报错";
    EXPECT_TRUE(loadWithTriplanar("vx_tri_max_over_one.toml", EmitMaterials(4, specs, true, 0.2, 1.5, 4.0)))
        << "slope_max > 1 必须报错";
    EXPECT_TRUE(loadWithTriplanar("vx_tri_min_negative.toml", EmitMaterials(4, specs, true, -0.1, 0.5, 4.0)))
        << "slope_min < 0 必须报错";
    EXPECT_TRUE(loadWithTriplanar("vx_tri_sharpness_zero.toml", EmitMaterials(4, specs, true, 0.2, 0.5, 0.0)))
        << "sharpness == 0 必须报错";

    // 整段缺失必须报错（结构变更，不得静默回退）。
    std::string noSection = EmitMaterials(4, specs);
    noSection.erase(noSection.find("[triplanar]"));
    EXPECT_TRUE(loadWithTriplanar("vx_tri_missing.toml", noSection)) << "缺 [triplanar] 段必须报错";
}
