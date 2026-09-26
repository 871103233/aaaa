#include "terrain/material_table.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string Describe(const std::filesystem::path& path, std::size_t slot, const char* field) {
    return path.string() + ": [[layer]] #" + std::to_string(slot) + " 字段 [" + field + "] ";
}

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* section, const char* field) {
    return path.string() + ": 段 [" + section + "] 字段 [" + field + "] ";
}

[[nodiscard]] bool ReadBool(const toml::table& section, const std::filesystem::path& path, const char* sectionName,
                            const char* field) {
    const std::optional<bool> value = section[field].value<bool>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是布尔值");
    }
    return *value;
}

[[nodiscard]] float ReadSectionFloat(const toml::table& section, const std::filesystem::path& path,
                                     const char* sectionName, const char* field) {
    const std::optional<double> value = section[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, sectionName, field) + "缺失或不是数值");
    }
    return static_cast<float>(*value);
}

[[nodiscard]] float ReadFloat(const toml::table& layer, const std::filesystem::path& path, std::size_t slot,
                              const char* field) {
    const std::optional<double> value = layer[field].value<double>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, slot, field) + "缺失或不是数值");
    }
    return static_cast<float>(*value);
}

[[nodiscard]] int ReadInt(const toml::table& layer, const std::filesystem::path& path, std::size_t slot,
                          const char* field) {
    const std::optional<std::int64_t> value = layer[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, slot, field) + "缺失或不是整数");
    }
    return static_cast<int>(*value);
}

[[nodiscard]] std::string ReadString(const toml::table& layer, const std::filesystem::path& path, std::size_t slot,
                                     const char* field) {
    const std::optional<std::string> value = layer[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, slot, field) + "缺失或不是字符串");
    }
    return *value;
}

void ValidateLayer(const MaterialLayer& layer, const std::filesystem::path& path, std::size_t slot) {
    if (layer.textureLayer < 1 || layer.textureLayer > 255) {
        throw std::runtime_error(Describe(path, slot, "texture_layer") + "必须落在 [1, 255]（0 号层保留给缺失纹理）");
    }
    if (layer.heightMin > layer.heightMax) {
        throw std::runtime_error(Describe(path, slot, "height_min") + "不能大于 height_max");
    }
    if (layer.slopeMin < 0.0F || layer.slopeMax > 1.0F || layer.slopeMin > layer.slopeMax) {
        throw std::runtime_error(Describe(path, slot, "slope_min/slope_max") + "必须满足 0 ≤ min ≤ max ≤ 1");
    }
    if (layer.heightBlend < 0.0F || layer.slopeBlend < 0.0F) {
        throw std::runtime_error(Describe(path, slot, "height_blend/slope_blend") + "不能为负");
    }
    if (!(layer.uvScale > 0.0F)) {
        throw std::runtime_error(Describe(path, slot, "uv_scale") + "必须大于 0");
    }
    for (const float tint : { layer.tintR, layer.tintG, layer.tintB }) {
        if (tint < 0.0F || tint > 1.0F) {
            throw std::runtime_error(Describe(path, slot, "tint_r/tint_g/tint_b") + "必须落在 [0, 1]");
        }
    }
    if (layer.roughness < 0.0F || layer.roughness > 1.0F) {
        throw std::runtime_error(Describe(path, slot, "roughness") + "必须落在 [0, 1]");
    }
    if (layer.ao < 0.0F || layer.ao > 1.0F) {
        throw std::runtime_error(Describe(path, slot, "ao") + "必须落在 [0, 1]");
    }
    if (!(layer.macroUvScale > 0.0F)) {
        throw std::runtime_error(Describe(path, slot, "macro_uv_scale") + "必须大于 0");
    }
    if (layer.macroStrength < 0.0F || layer.macroStrength > 1.0F) {
        throw std::runtime_error(Describe(path, slot, "macro_strength") + "必须落在 [0, 1]");
    }
}

}  // namespace

TerrainMaterialTable TerrainMaterialTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法加载材质表 " + path.string() + ": " + error.what());
    }

    const std::optional<std::int64_t> schemaVersion = document["schema_version"].value<std::int64_t>();
    if (!schemaVersion.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    if (*schemaVersion != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 不匹配（期望 " + std::to_string(kSchemaVersion) +
                                 "，实际 " + std::to_string(*schemaVersion) + "）");
    }

    const toml::array* layers = document["layer"].as_array();
    if (layers == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [[layer]] 数组");
    }
    if (layers->size() != static_cast<std::size_t>(kMaterialSlotCount)) {
        throw std::runtime_error(path.string() + ": [[layer]] 数量必须等于 splat 槽位数 " +
                                 std::to_string(kMaterialSlotCount) + "，实际 " + std::to_string(layers->size()));
    }

    TerrainMaterialTable table;
    table.m_schemaVersion = static_cast<int>(*schemaVersion);

    for (std::size_t slot = 0; slot < layers->size(); ++slot) {
        const toml::table* layer = (*layers)[slot].as_table();
        if (layer == nullptr) {
            throw std::runtime_error(path.string() + ": [[layer]] #" + std::to_string(slot) + " 不是表");
        }

        MaterialLayer parsed;
        parsed.name        = ReadString(*layer, path, slot, "name");
        parsed.textureLayer = ReadInt(*layer, path, slot, "texture_layer");
        parsed.heightMin   = ReadFloat(*layer, path, slot, "height_min");
        parsed.heightMax   = ReadFloat(*layer, path, slot, "height_max");
        parsed.heightBlend = ReadFloat(*layer, path, slot, "height_blend");
        parsed.slopeMin    = ReadFloat(*layer, path, slot, "slope_min");
        parsed.slopeMax    = ReadFloat(*layer, path, slot, "slope_max");
        parsed.slopeBlend  = ReadFloat(*layer, path, slot, "slope_blend");
        parsed.uvScale     = ReadFloat(*layer, path, slot, "uv_scale");
        parsed.tintR       = ReadFloat(*layer, path, slot, "tint_r");
        parsed.tintG       = ReadFloat(*layer, path, slot, "tint_g");
        parsed.tintB       = ReadFloat(*layer, path, slot, "tint_b");
        parsed.roughness     = ReadFloat(*layer, path, slot, "roughness");
        parsed.ao            = ReadFloat(*layer, path, slot, "ao");
        parsed.macroUvScale  = ReadFloat(*layer, path, slot, "macro_uv_scale");
        parsed.macroStrength = ReadFloat(*layer, path, slot, "macro_strength");

        ValidateLayer(parsed, path, slot);
        table.m_layers[slot] = std::move(parsed);
    }

    // C 项：全局三平面（triplanar）参数。必填；缺失 / 越界一律抛异常（与其它段同口径，不静默回退）。
    const toml::table* triplanar = document["triplanar"].as_table();
    if (triplanar == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [triplanar] 段");
    }
    TriplanarSettings parsedTriplanar;
    parsedTriplanar.enabled   = ReadBool(*triplanar, path, "triplanar", "enabled");
    parsedTriplanar.slopeMin  = ReadSectionFloat(*triplanar, path, "triplanar", "slope_min");
    parsedTriplanar.slopeMax  = ReadSectionFloat(*triplanar, path, "triplanar", "slope_max");
    parsedTriplanar.sharpness = ReadSectionFloat(*triplanar, path, "triplanar", "sharpness");
    if (parsedTriplanar.slopeMin < 0.0F || parsedTriplanar.slopeMax > 1.0F ||
        parsedTriplanar.slopeMin >= parsedTriplanar.slopeMax) {
        throw std::runtime_error(Describe(path, "triplanar", "slope_min/slope_max") +
                                 "必须满足 0 ≤ slope_min < slope_max ≤ 1（否则 smoothstep 退化）");
    }
    if (!(parsedTriplanar.sharpness > 0.0F)) {
        throw std::runtime_error(Describe(path, "triplanar", "sharpness") + "必须大于 0");
    }
    table.m_triplanar = parsedTriplanar;

    return table;
}

TerrainMaterialTable TerrainMaterialTable::Default() {
    TerrainMaterialTable table;

    // 取值与 assets/config/materials.toml 一致，保证测试与运行期行为可比。
    // 字段顺序见 MaterialLayer 声明：name / textureLayer / 高度带(3) / 坡度带(3) / uvScale /
    // tintRGB / roughness / ao / macroUvScale / macroStrength。
    // 带参数（缺陷 2 修复）：使 (高度 ∈ [0,512], 坡度 ∈ [0,1]) 全域至少一层非零，见 TOML 头部论证。
    table.m_layers[0] = MaterialLayer { "grass", 1, 0.0F, 320.0F, 60.0F, 0.0F, 0.45F, 0.10F, 0.12F, 0.31F, 0.55F,
                                        0.24F, 0.90F, 0.85F, 0.020F, 0.35F };
    table.m_layers[1] = MaterialLayer { "dirt", 2, 300.0F, 512.0F, 60.0F, 0.0F, 0.55F, 0.10F, 0.10F, 0.45F, 0.33F,
                                        0.21F, 0.88F, 0.75F, 0.015F, 0.40F };
    table.m_layers[2] = MaterialLayer { "rock", 3, 0.0F, 512.0F, 0.0F, 0.55F, 1.0F, 0.10F, 0.16F, 0.55F, 0.55F,
                                        0.56F, 0.40F, 0.70F, 0.030F, 0.30F };
    table.m_layers[3] = MaterialLayer { "sand", 4, 0.0F, 6.0F, 3.0F, 0.0F, 0.30F, 0.10F, 0.18F, 0.83F, 0.74F,
                                        0.48F, 0.95F, 0.90F, 0.025F, 0.25F };

    // C 项：三平面参数（默认值即 TriplanarSettings 的成员初值，与 assets/config/materials.toml 的 [triplanar] 一致）。
    table.m_triplanar = TriplanarSettings {};

    return table;
}

MaterialUniform BuildMaterialUniform(const TerrainMaterialTable& table, double originX, double originY,
                                     double originZ) noexcept {
    MaterialUniform uniform;
    uniform.renderOriginX = static_cast<float>(originX);
    uniform.renderOriginY = static_cast<float>(originY);
    uniform.renderOriginZ = static_cast<float>(originZ);

    // C 项：全局三平面参数（单入口投影；与各层字段同源，禁止在着色器另写一份）。
    const TriplanarSettings& triplanar = table.Triplanar();
    uniform.triplanarEnabled   = triplanar.enabled ? 1.0F : 0.0F;
    uniform.triplanarSlopeMin  = triplanar.slopeMin;
    uniform.triplanarSlopeMax  = triplanar.slopeMax;
    uniform.triplanarSharpness = triplanar.sharpness;

    for (std::size_t slot = 0; slot < static_cast<std::size_t>(kMaterialSlotCount); ++slot) {
        const MaterialLayer& layer = table.Layer(static_cast<int>(slot));
        MaterialLayerUniform& out  = uniform.layers[slot];

        out.heightMin = layer.heightMin;
        out.heightMax = layer.heightMax;
        out.heightBlend = layer.heightBlend;
        // 纹理数组层号从 0 起：表里的 1 号层 = 数组第 0 层（0 号层留给"缺失纹理"）。
        out.textureIndex = static_cast<float>(layer.textureLayer - 1);
        out.slopeMin = layer.slopeMin;
        out.slopeMax = layer.slopeMax;
        out.slopeBlend = layer.slopeBlend;
        out.roughness = layer.roughness;
        out.tintR = layer.tintR;
        out.tintG = layer.tintG;
        out.tintB = layer.tintB;
        out.uvScale = layer.uvScale;
        out.macroUvScale = layer.macroUvScale;
        out.macroStrength = layer.macroStrength;
        out.ao = layer.ao;
        out.macroAoUnused = 0.0F;
    }

    return uniform;
}

}  // namespace vx
