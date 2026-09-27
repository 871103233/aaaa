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

/// 读一个**可选**数值（T43）：缺失即取 `fallback`；写了但不是数值 ⇒ 抛（口径同其它字段，不静默忽略类型错）。
[[nodiscard]] float ReadOptionalFloat(const toml::table& layer, const std::filesystem::path& path, std::size_t slot,
                                      const char* field, float fallback) {
    if (!layer.contains(field)) {
        return fallback;
    }
    return ReadFloat(layer, path, slot, field);
}

/// 读一个**可选**布尔（T43）：缺失即取 `fallback`；类型错 ⇒ 抛。
[[nodiscard]] bool ReadOptionalBool(const toml::table& layer, const std::filesystem::path& path, std::size_t slot,
                                    const char* field, bool fallback) {
    if (!layer.contains(field)) {
        return fallback;
    }
    const std::optional<bool> value = layer[field].value<bool>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, slot, field) + "不是布尔值");
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
    if (layer.toughness < 0.0F) {
        throw std::runtime_error(Describe(path, slot, "toughness") + "不能为负（点/格³；0 = 不可破坏）");
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
    // T43 / ADR 0016：物理参数（可选；非法即抛）。
    if (!(layer.density > 0.0F)) {
        throw std::runtime_error(Describe(path, slot, "density") + "必须大于 0（每格³ 质量）");
    }
    if (layer.friction < 0.0F || layer.friction > 1.0F) {
        throw std::runtime_error(Describe(path, slot, "friction") + "必须落在 [0, 1]");
    }
    if (layer.restitution < 0.0F || layer.restitution > 1.0F) {
        throw std::runtime_error(Describe(path, slot, "restitution") + "必须落在 [0, 1]");
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

    // ADR 0014：每层的 `subsurface` 用**材质名**引用（可读、与层顺序无关），故先收集名字、
    // 等所有层都解析完再映射成槽位号（见紧随其后的循环）。
    std::array<std::string, static_cast<std::size_t>(kMaterialSlotCount)> subsurfaceNames {};

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
        // `subsurface` 可缺省（缺省 = 自身，兼容旧文件）；一旦写了就参与下面的名字校验。
        subsurfaceNames[slot] = layer->contains("subsurface") ? ReadString(*layer, path, slot, "subsurface")
                                                             : parsed.name;
        // T43 / ADR 0016：物理参数（可选，缺省 = `MaterialLayer` 的默认值 ⇒ 旧文件行为不变）。
        parsed.density        = ReadOptionalFloat(*layer, path, slot, "density", parsed.density);
        parsed.friction       = ReadOptionalFloat(*layer, path, slot, "friction", parsed.friction);
        parsed.restitution    = ReadOptionalFloat(*layer, path, slot, "restitution", parsed.restitution);
        parsed.indestructible = ReadOptionalBool(*layer, path, slot, "indestructible", parsed.indestructible);
        // T46 / ADR 0017：落地后的表示（可选，缺省 = 散体 ⇒ 旧文件行为不变）。
        parsed.rigidDebris = ReadOptionalBool(*layer, path, slot, "rigid_debris", parsed.rigidDebris);
        // T31 / ADR 0013：伤害模型的坚固度（**必填** —— 缺失 / 非数即抛，见 TOML 头部口径）。
        parsed.toughness = ReadFloat(*layer, path, slot, "toughness");

        ValidateLayer(parsed, path, slot);
        table.m_layers[slot] = std::move(parsed);
    }

    // 名字 → 槽位号（ADR 0014）。必须等所有层解析完才能解析引用；未知名字抛异常（不静默回退）。
    for (std::size_t slot = 0; slot < static_cast<std::size_t>(kMaterialSlotCount); ++slot) {
        int found = -1;
        for (std::size_t other = 0; other < static_cast<std::size_t>(kMaterialSlotCount); ++other) {
            if (table.m_layers[other].name == subsurfaceNames[slot]) {
                found = static_cast<int>(other);
                break;
            }
        }
        if (found < 0) {
            throw std::runtime_error(path.string() + ": layer[" + std::to_string(slot) +
                                     "].subsurface 指向未知材质 \"" + subsurfaceNames[slot] + "\"");
        }
        table.m_layers[slot].subsurfaceSlot = found;
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

    // ADR 0014：表层 → 次表层映射，与 assets/config/materials.toml 的 `subsurface` 一致。
    // 草 / 沙只该出现在地表薄层，在体积内映射为土；岩 / 土保持自身。
    table.m_layers[0].subsurfaceSlot = 1;  // grass → dirt
    table.m_layers[1].subsurfaceSlot = 1;  // dirt  → dirt
    table.m_layers[2].subsurfaceSlot = 2;  // rock  → rock
    table.m_layers[3].subsurfaceSlot = 1;  // sand  → dirt

    // T43 / ADR 0016：物理参数（与 materials.toml 逐值一致；参照真实材料：土 ~1.5、花岗岩 ~2.6）。
    table.m_layers[0].density        = 1.3F;   // grass
    table.m_layers[0].friction       = 0.75F;
    table.m_layers[0].restitution    = 0.02F;
    table.m_layers[0].indestructible = false;
    table.m_layers[1].density        = 1.5F;   // dirt
    table.m_layers[1].friction       = 0.60F;
    table.m_layers[1].restitution    = 0.02F;
    table.m_layers[1].indestructible = false;
    table.m_layers[2].density        = 2.6F;   // rock
    table.m_layers[2].friction       = 0.70F;
    table.m_layers[2].restitution    = 0.12F;
    // T52（2026-09-28，项目所有者指定）：**岩 = 完全不可破坏**（"无法击毁无法挖洞，但仍能被光球打到"）。
    // 与 materials.toml 逐值一致；`toughness = 5.0` 保留，改回 false 即恢复"可挖的硬岩"。
    table.m_layers[2].indestructible = true;
    table.m_layers[3].density        = 1.6F;   // sand
    table.m_layers[3].friction       = 0.50F;
    table.m_layers[3].restitution    = 0.05F;
    table.m_layers[3].indestructible = false;

    // T46 / ADR 0017：落地后的表示（与 materials.toml 逐值一致）。
    // 岩 = 刚性（碎块落地后保留几何体）；草 / 土 / 沙 = 散体（回写并与地面融合、且接地沉降）。
    table.m_layers[0].rigidDebris = false;  // grass
    table.m_layers[1].rigidDebris = false;  // dirt
    table.m_layers[2].rigidDebris = true;   // rock
    table.m_layers[3].rigidDebris = false;  // sand

    // T31 / ADR 0013：坚固度（点/格³；与 materials.toml 逐值一致）。
    // 锚点来自项目所有者：泥土 3、岩石 5（配合 destruction.toml 的换算系数 271 ⇒ 泥 r≈6、岩 r≈5.06）。
    // 草 / 沙 由本实现补齐为 2（真实世界：松散表层 / 砂比黏土更易挖）。
    table.m_layers[0].toughness = 2.0F;  // grass
    table.m_layers[1].toughness = 3.0F;  // dirt
    table.m_layers[2].toughness = 5.0F;  // rock
    table.m_layers[3].toughness = 2.0F;  // sand

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
