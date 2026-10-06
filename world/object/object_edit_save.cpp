#include "object/object_edit_save.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <toml++/toml.hpp>

namespace vx {
namespace {

/// `ObjectAssetKind` → 配置串（与 `ParseKind` 的取值**必须一致**；不一致 ⇒ 写出的文件读不回 ⇒ 单测会挡）。
[[nodiscard]] const char* KindToString(ObjectAssetKind kind) noexcept {
    switch (kind) {
        case ObjectAssetKind::DirtPile:
            return "dirt_pile";
        case ObjectAssetKind::Stone:
            return "stone";
        case ObjectAssetKind::Crate:
            return "crate";
        case ObjectAssetKind::Portal:
            return "portal";
        case ObjectAssetKind::Model:
            return "model";
    }
    return "stone";
}

[[nodiscard]] toml::table SerializeType(const ObjectType& type) {
    toml::table entry;
    (void)entry.insert_or_assign("id", type.id);
    (void)entry.insert_or_assign("kind", std::string(KindToString(type.kind)));
    toml::array extent;
    extent.push_back(static_cast<double>(type.halfExtentX));
    extent.push_back(static_cast<double>(type.halfExtentY));
    extent.push_back(static_cast<double>(type.halfExtentZ));
    (void)entry.insert_or_assign("half_extent", std::move(extent));
    (void)entry.insert_or_assign("destructible", type.destructible);
    if (!type.modelFile.empty()) {
        (void)entry.insert_or_assign("model_file", type.modelFile);
    }
    if (type.materialSlot >= 0) {
        (void)entry.insert_or_assign("material_slot", static_cast<std::int64_t>(type.materialSlot));
    }
    (void)entry.insert_or_assign("category", type.category);
    return entry;
}

[[nodiscard]] toml::table SerializePlacement(const ObjectPlacement& placement) {
    toml::table entry;
    (void)entry.insert_or_assign("type", placement.typeId);
    toml::array position;
    position.push_back(static_cast<double>(placement.x));
    position.push_back(static_cast<double>(placement.y));
    position.push_back(static_cast<double>(placement.z));
    (void)entry.insert_or_assign("position", std::move(position));
    if (placement.yawDegrees != 0.0F) {
        (void)entry.insert_or_assign("yaw_deg", static_cast<double>(placement.yawDegrees));
    }
    if (!placement.targetWorldId.empty()) {
        (void)entry.insert_or_assign("target_world", placement.targetWorldId);
    }
    if (!placement.portalName.empty()) {
        (void)entry.insert_or_assign("portal_name", placement.portalName);
    }
    return entry;
}

[[nodiscard]] toml::table SerializeRemoval(const ObjectRemoval& removal) {
    toml::table entry;
    (void)entry.insert_or_assign("type", removal.typeId);
    toml::array position;
    position.push_back(static_cast<double>(removal.x));
    position.push_back(static_cast<double>(removal.z));
    (void)entry.insert_or_assign("position", std::move(position));
    (void)entry.insert_or_assign("tolerance", static_cast<double>(removal.tolerance));
    return entry;
}

[[nodiscard]] toml::table SerializeScatter(const ObjectScatter& scatter) {
    toml::table entry;
    (void)entry.insert_or_assign("type", scatter.typeId);
    toml::array center;
    center.push_back(static_cast<double>(scatter.centerX));
    center.push_back(static_cast<double>(scatter.centerZ));
    (void)entry.insert_or_assign("center", std::move(center));
    (void)entry.insert_or_assign("radius", static_cast<double>(scatter.radius));
    (void)entry.insert_or_assign("count", static_cast<std::int64_t>(scatter.count));
    // `[[scatter]].seed` 的文件口径 = **整数**（见 `ParsePlacementsAndScatters` 的 `ReadInt`）⇒ 按 int64 写，
    // 保证"写 → 读回"逐字段一致；散射种子是作者给定的小整数，不涉及 u64 大值（区别于存档槽的 `seed`）。
    (void)entry.insert_or_assign("seed", static_cast<std::int64_t>(scatter.seed));
    return entry;
}

}  // namespace

void SaveObjectEditLayer(const std::filesystem::path& path, const ObjectTable& editLayer) {
    toml::table document;
    (void)document.insert_or_assign("schema_version", static_cast<std::int64_t>(ObjectTable::kSchemaVersion));
    (void)document.insert_or_assign("destructible_enabled", editLayer.destructibleEnabled);

    toml::array types;
    types.reserve(editLayer.types.size());
    for (const ObjectType& type : editLayer.types) {
        types.push_back(SerializeType(type));
    }
    (void)document.insert_or_assign("type", std::move(types));

    toml::array placements;
    placements.reserve(editLayer.placements.size());
    for (const ObjectPlacement& placement : editLayer.placements) {
        placements.push_back(SerializePlacement(placement));
    }
    (void)document.insert_or_assign("placement", std::move(placements));

    toml::array removals;
    removals.reserve(editLayer.removals.size());
    for (const ObjectRemoval& removal : editLayer.removals) {
        removals.push_back(SerializeRemoval(removal));
    }
    (void)document.insert_or_assign("remove", std::move(removals));

    toml::array scatters;
    scatters.reserve(editLayer.scatters.size());
    for (const ObjectScatter& scatter : editLayer.scatters) {
        scatters.push_back(SerializeScatter(scatter));
    }
    (void)document.insert_or_assign("scatter", std::move(scatters));

    // 原子替换：先写临时文件，再 rename 覆盖 ⇒ 崩溃不会留下半个文件（口径同 ADR 0030）。
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("无法创建可编辑层临时文件 " + tempPath.string());
        }
        out << "# 物件可编辑层（V0.5 E3 由游戏保存生成；**覆盖式写入 ⇒ 注释不会保留**）\n";
        out << "# 加载顺序 = 发布清单（只读）→ 应用 [[remove]] → 追加 [[placement]] → [[scatter]]。\n";
        out << document;
        out.flush();
        if (!out) {
            throw std::runtime_error("写入可编辑层临时文件失败 " + tempPath.string());
        }
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);  // 清理临时文件（尽力而为）
        throw std::runtime_error("可编辑层原子替换失败（" + path.string() + "）：" + renameError.message());
    }
}

}  // namespace vx
