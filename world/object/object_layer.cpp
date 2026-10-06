#include "object/object_layer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <toml++/toml.hpp>

namespace vx {
namespace {

/// 首个物件 id（0 保留为"无效"）。
constexpr std::uint32_t kFirstObjectId = 1;

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::int64_t ReadInt(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::int64_t> value = table[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是整数");
    }
    return *value;
}

[[nodiscard]] std::string ReadString(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::string> value = table[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是字符串");
    }
    return *value;
}

[[nodiscard]] bool ReadBool(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<bool> value = table[field].value<bool>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是布尔值");
    }
    return *value;
}

/// 读取数值：既接受 TOML 浮点（`1.0`）也接受整数（`1`）。
[[nodiscard]] double ReadNumber(const toml::table& table, const std::filesystem::path& path, const char* field) {
    if (const std::optional<double> value = table[field].value<double>(); value.has_value()) {
        return *value;
    }
    if (const std::optional<std::int64_t> value = table[field].value<std::int64_t>(); value.has_value()) {
        return static_cast<double>(*value);
    }
    throw std::runtime_error(Describe(path, field) + "缺失或不是数值");
}

/// 读取可选的数值：缺失时返回 `fallback`（如 `yaw_deg`）。
[[nodiscard]] double ReadNumberOr(const toml::table& table, const std::filesystem::path& path, const char* field,
                                  double fallback) {
    if (table[field].value<double>().has_value() || table[field].value<std::int64_t>().has_value()) {
        return ReadNumber(table, path, field);
    }
    return fallback;
}

/// 读取长度为 3 的数值数组（`half_extent` / `position`）。
[[nodiscard]] std::array<double, 3> ReadVec3(const toml::table& table, const std::filesystem::path& path,
                                             const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != static_cast<std::size_t>(3)) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是恰好含 3 个数值的数组");
    }
    std::array<double, 3> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const toml::node& element = (*array)[i];
        if (const std::optional<double> number = element.value<double>(); number.has_value()) {
            out[i] = *number;
        } else if (const std::optional<std::int64_t> integer = element.value<std::int64_t>(); integer.has_value()) {
            out[i] = static_cast<double>(*integer);
        } else {
            throw std::runtime_error(Describe(path, field) + "的元素必须都是数值");
        }
    }
    return out;
}

/// 读取长度为 2 的数值数组（V8：`[[scatter]].center` = 平面 `[x, z]`；Y 由地表高度求解）。
[[nodiscard]] std::array<double, 2> ReadVec2(const toml::table& table, const std::filesystem::path& path,
                                             const char* field) {
    const toml::array* array = table[field].as_array();
    if (array == nullptr || array->size() != static_cast<std::size_t>(2)) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是恰好含 2 个数值的数组");
    }
    std::array<double, 2> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const toml::node& element = (*array)[i];
        if (const std::optional<double> number = element.value<double>(); number.has_value()) {
            out[i] = *number;
        } else if (const std::optional<std::int64_t> integer = element.value<std::int64_t>(); integer.has_value()) {
            out[i] = static_cast<double>(*integer);
        } else {
            throw std::runtime_error(Describe(path, field) + "的元素必须都是数值");
        }
    }
    return out;
}

[[nodiscard]] ObjectAssetKind ParseKind(const std::string& text, const std::filesystem::path& path) {
    if (text == "dirt_pile") {
        return ObjectAssetKind::DirtPile;
    }
    if (text == "stone") {
        return ObjectAssetKind::Stone;
    }
    if (text == "crate") {
        return ObjectAssetKind::Crate;
    }
    if (text == "portal") {
        return ObjectAssetKind::Portal;  // V3：传送门（放置时必须带 target_world）
    }
    if (text == "model") {
        return ObjectAssetKind::Model;  // V8：外部模型（必须带 model_file）
    }
    throw std::runtime_error(path.string() + ": 未知的物件形态 [" + text +
                             "]（可选：dirt_pile / stone / crate / portal / model）");
}

/// 物件类型的查找器（用于"落点引用的类型是否存在"这类校验）。
using TypeLookup = std::function<const ObjectType*(const std::string&)>;

/// 解析 `[[type]]` 段并**追加**到 `out`；`existing` 给出"已存在的类型"（重复检测用，可为恒空查找）。
///
/// 规则（非法即抛）：`id` 非空且**不与 `existing` / 本段内重复**、`kind` 合法、`half_extent` 三分量为正、
/// `destructible` 必填；`model_file` / `material_slot` **仅 `model` 可给**。
void ParseTypeEntries(const toml::array& typeArray, const std::filesystem::path& path, const TypeLookup& existing,
                      ObjectTable& out) {
    for (const toml::node& node : typeArray) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            throw std::runtime_error(path.string() + ": [[type]] 的每个元素都必须是表");
        }
        ObjectType type;
        type.id = ReadString(*entry, path, "id");
        if (type.id.empty()) {
            throw std::runtime_error(path.string() + ": [[type]].id 不能为空");
        }
        if (existing(type.id) != nullptr || out.Find(type.id) != nullptr) {
            throw std::runtime_error(path.string() + ": [[type]].id 重复 [" + type.id + "]");
        }
        type.kind                         = ParseKind(ReadString(*entry, path, "kind"), path);
        const std::array<double, 3> extent = ReadVec3(*entry, path, "half_extent");
        if (extent[0] <= 0.0 || extent[1] <= 0.0 || extent[2] <= 0.0) {
            throw std::runtime_error(path.string() + ": [[type]].half_extent 的三个分量都必须为正（id = " +
                                     type.id + "）");
        }
        type.halfExtentX = static_cast<float>(extent[0]);
        type.halfExtentY = static_cast<float>(extent[1]);
        type.halfExtentZ = static_cast<float>(extent[2]);
        type.destructible = ReadBool(*entry, path, "destructible");

        // V8：`model_file` / `material_slot` —— **只对 `Model` 形态有意义**（与 `target_world` 同口径：非法即抛）。
        if (const toml::node* modelNode = entry->get("model_file"); modelNode != nullptr) {
            const std::optional<std::string> file = modelNode->value<std::string>();
            if (!file.has_value()) {
                throw std::runtime_error(Describe(path, "model_file") + "不是字符串（id = " + type.id + "）");
            }
            type.modelFile = *file;
        }
        if (const toml::node* slotNode = entry->get("material_slot"); slotNode != nullptr) {
            const std::optional<std::int64_t> slot = slotNode->value<std::int64_t>();
            if (!slot.has_value()) {
                throw std::runtime_error(Describe(path, "material_slot") + "不是整数（id = " + type.id + "）");
            }
            type.materialSlot = static_cast<int>(*slot);
        }
        if (type.kind == ObjectAssetKind::Model) {
            if (type.modelFile.empty()) {
                throw std::runtime_error(path.string() + ": 形态 model 的类型 [" + type.id +
                                         "] 必须给出非空的 model_file");
            }
            if (type.materialSlot != -1 && (type.materialSlot < 0 || type.materialSlot > 3)) {
                throw std::runtime_error(path.string() + ": 类型 [" + type.id +
                                         "] 的 material_slot 必须在 0..3（0 草 / 1 土 / 2 岩 / 3 沙）");
            }
        } else {
            if (!type.modelFile.empty()) {
                throw std::runtime_error(path.string() + ": 只有形态 model 可以给出 model_file（类型 [" + type.id +
                                         "] 不是 model）");
            }
            if (type.materialSlot != -1) {
                throw std::runtime_error(path.string() + ": 只有形态 model 可以给出 material_slot（类型 [" + type.id + "]）");
            }
        }

        // E3：`category`（**可选**，缺省 `misc`）—— 选择器的一级列表；值域固定，非法即抛（ADR 0032 决策二）。
        if (const toml::node* categoryNode = entry->get("category"); categoryNode != nullptr) {
            const std::optional<std::string> category = categoryNode->value<std::string>();
            if (!category.has_value()) {
                throw std::runtime_error(Describe(path, "category") + "不是字符串（id = " + type.id + "）");
            }
            if (!IsValidObjectCategory(*category)) {
                throw std::runtime_error(path.string() + ": 类型 [" + type.id + "] 的 category 非法：\"" + *category +
                                         "\"（可选：vegetation / rock / prop / building / portal / misc）");
            }
            type.category = *category;
        }
        // 传送门必须归到 `portal` 类别（否则会在选择器里混进别的一级列表）。
        if (type.kind == ObjectAssetKind::Portal && type.category != "portal") {
            throw std::runtime_error(path.string() + ": 形态 portal 的类型 [" + type.id + "] 的 category 必须是 \"portal\"");
        }

        out.types.push_back(std::move(type));
    }
}

/// 解析 `[[placement]]` + `[[scatter]]` 并追加到 `out`；`findType` 给出"可引用的类型集合"
/// （发布清单、或"发布清单 ∪ 可编辑层新增类型"）。
void ParsePlacementsAndScatters(const toml::table& root, const std::filesystem::path& path, const TypeLookup& findType,
                                ObjectTable& out) {
    if (const toml::array* placementArray = root["placement"].as_array(); placementArray != nullptr) {
        for (const toml::node& node : *placementArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[placement]] 的每个元素都必须是表");
            }
            ObjectPlacement placement;
            placement.typeId = ReadString(*entry, path, "type");
            const ObjectType* placedType = findType(placement.typeId);
            if (placedType == nullptr) {
                throw std::runtime_error(path.string() + ": [[placement]].type 引用了不存在的类型 [" +
                                         placement.typeId + "]");
            }
            const std::array<double, 3> position = ReadVec3(*entry, path, "position");
            placement.x          = static_cast<float>(position[0]);
            placement.y          = static_cast<float>(position[1]);
            placement.z          = static_cast<float>(position[2]);
            placement.yawDegrees = static_cast<float>(ReadNumberOr(*entry, path, "yaw_deg", 0.0));

            // V3：`target_world`（**仅传送门可给**）。
            // 规则：Portal ⇒ 必填且非空；其它类型 ⇒ 必须为空（写了却不生效 = 静默配置，一律抛）。
            // 这里只校验"有没有、空不空"；**该世界是否注册过**由游戏层（WorldManager）判定。
            if (const toml::node* targetNode = entry->get("target_world"); targetNode != nullptr) {
                const std::optional<std::string> target = targetNode->value<std::string>();
                if (!target.has_value()) {
                    throw std::runtime_error(path.string() + ": [[placement]].target_world 不是字符串（type = [" +
                                             placement.typeId + "]）");
                }
                placement.targetWorldId = *target;
            }
            if (placedType->kind == ObjectAssetKind::Portal) {
                if (placement.targetWorldId.empty()) {
                    throw std::runtime_error(path.string() + ": 传送门放置（type = [" + placement.typeId +
                                             "]）必须给出非空的 target_world");
                }
            } else if (!placement.targetWorldId.empty()) {
                throw std::runtime_error(path.string() + ": 只有传送门可以给出 target_world（type = [" +
                                         placement.typeId + "] 是普通物件）");
            }

            // V3c：`portal_name`（**仅传送门可给**，可选）—— 门的**显示名**。
            // 命名口径来自所有者（"先用直白的名字占用，措辞后续我自己补"）⇒ 这里只搬运，不硬编码措辞；
            // **空串视为未给出**（UI 层取缺省「神秘传送门」）。
            if (const toml::node* nameNode = entry->get("portal_name"); nameNode != nullptr) {
                const std::optional<std::string> name = nameNode->value<std::string>();
                if (!name.has_value()) {
                    throw std::runtime_error(path.string() + ": [[placement]].portal_name 不是字符串（type = [" +
                                             placement.typeId + "]）");
                }
                placement.portalName = *name;
            }
            if (placedType->kind != ObjectAssetKind::Portal && !placement.portalName.empty()) {
                throw std::runtime_error(path.string() + ": 只有传送门可以给出 portal_name（type = [" +
                                         placement.typeId + "] 是普通物件）");
            }
            out.placements.push_back(std::move(placement));
        }
    }

    // V8：`[[scatter]]`（可选）—— 程序化散布（确定性抖动网格；落点由 `PlanObjectScatter` 生成）。
    if (const toml::array* scatterArray = root["scatter"].as_array(); scatterArray != nullptr) {
        for (const toml::node& node : *scatterArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[scatter]] 的每个元素都必须是表");
            }
            ObjectScatter scatter;
            scatter.typeId = ReadString(*entry, path, "type");
            if (findType(scatter.typeId) == nullptr) {
                throw std::runtime_error(path.string() + ": [[scatter]].type 引用了不存在的类型 [" + scatter.typeId + "]");
            }
            const std::array<double, 2> center = ReadVec2(*entry, path, "center");
            scatter.centerX = static_cast<float>(center[0]);
            scatter.centerZ = static_cast<float>(center[1]);
            const double radius = ReadNumber(*entry, path, "radius");
            if (radius <= 0.0) {
                throw std::runtime_error(path.string() + ": [[scatter]].radius 必须为正（type = " + scatter.typeId + "）");
            }
            scatter.radius = static_cast<float>(radius);
            const std::int64_t count = ReadInt(*entry, path, "count");
            if (count <= 0) {
                throw std::runtime_error(path.string() + ": [[scatter]].count 必须为正（type = " + scatter.typeId + "）");
            }
            scatter.count = static_cast<int>(count);
            const std::int64_t seed = ReadInt(*entry, path, "seed");
            if (seed < 0) {
                throw std::runtime_error(path.string() + ": [[scatter]].seed 不能为负（type = " + scatter.typeId + "）");
            }
            scatter.seed = static_cast<std::uint64_t>(seed);
            out.scatters.push_back(std::move(scatter));
        }
    }

    // E3：`[[remove]]`（可选）—— 表达"删掉某个落点"（仅可编辑层有意义；解析器共用）。
    // 匹配口径：同 `type` 且**平面距离 ≤ tolerance**（缺省 0.5 格）；**已知限制**：同类型同位置无法区分（ADR 0032）。
    if (const toml::array* removeArray = root["remove"].as_array(); removeArray != nullptr) {
        for (const toml::node& node : *removeArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[remove]] 的每个元素都必须是表");
            }
            ObjectRemoval removal;
            removal.typeId = ReadString(*entry, path, "type");
            if (findType(removal.typeId) == nullptr) {
                throw std::runtime_error(path.string() + ": [[remove]].type 引用了不存在的类型 [" + removal.typeId + "]");
            }
            const std::array<double, 2> position = ReadVec2(*entry, path, "position");
            removal.x           = static_cast<float>(position[0]);
            removal.z           = static_cast<float>(position[1]);
            removal.tolerance   = static_cast<float>(ReadNumberOr(*entry, path, "tolerance", 0.5));
            if (!(removal.tolerance > 0.0F)) {
                throw std::runtime_error(path.string() + ": [[remove]].tolerance 必须为正（type = " + removal.typeId + "）");
            }
            out.removals.push_back(std::move(removal));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ObjectLayer（EnTT 注册表的持有者；PIMPL 不暴露 entt 类型）
// ---------------------------------------------------------------------------

struct ObjectLayer::Impl {
    /// 物件变换（层③实体自己的数据；**不写进地形场**，ADR 0004 硬约束 1）。
    struct Transform {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float yawDegrees = 0.0f;
    };

    /// 指向类型表条目的引用（类型表生命周期须覆盖物件层的使用期）。
    struct TypeRef {
        const ObjectType* type = nullptr;
    };

    entt::registry                                  registry;
    std::unordered_map<std::uint32_t, entt::entity> byId;   ///< 稳定 id → 实体
    std::vector<std::uint32_t>                      order;  ///< 放置顺序（确定性遍历）
    std::uint32_t                                   nextId = kFirstObjectId;
};

ObjectLayer::ObjectLayer() : impl_(std::make_unique<Impl>()) {}

ObjectLayer::~ObjectLayer() = default;

ObjectLayer::ObjectLayer(ObjectLayer&&) noexcept            = default;
ObjectLayer& ObjectLayer::operator=(ObjectLayer&&) noexcept = default;

std::uint32_t ObjectLayer::Place(const ObjectTable& table, const ObjectPlacement& placement) {
    const ObjectType* type = table.Find(placement.typeId);
    if (type == nullptr) {
        throw std::invalid_argument("ObjectLayer::Place: 未知的物件类型 [" + placement.typeId + "]");
    }

    const entt::entity entity = impl_->registry.create();
    impl_->registry.emplace<Impl::Transform>(entity, Impl::Transform{placement.x, placement.y, placement.z,
                                                                     placement.yawDegrees});
    impl_->registry.emplace<Impl::TypeRef>(entity, Impl::TypeRef{type});

    const std::uint32_t id = impl_->nextId++;
    impl_->byId.emplace(id, entity);
    impl_->order.push_back(id);
    return id;
}

bool ObjectLayer::Remove(std::uint32_t id) noexcept {
    const auto found = impl_->byId.find(id);
    if (found == impl_->byId.end()) {
        return false;
    }
    impl_->registry.destroy(found->second);
    impl_->byId.erase(found);
    const auto position = std::find(impl_->order.begin(), impl_->order.end(), id);
    if (position != impl_->order.end()) {
        impl_->order.erase(position);
    }
    return true;
}

bool ObjectLayer::Get(std::uint32_t id, ObjectInstance& out) const {
    const auto found = impl_->byId.find(id);
    if (found == impl_->byId.end()) {
        return false;
    }
    const Impl::Transform& transform = impl_->registry.get<Impl::Transform>(found->second);
    const Impl::TypeRef&   typeRef   = impl_->registry.get<Impl::TypeRef>(found->second);
    out.id          = id;
    out.type        = typeRef.type;
    out.x           = transform.x;
    out.y           = transform.y;
    out.z           = transform.z;
    out.yawDegrees  = transform.yawDegrees;
    out.intact      = true;
    return true;
}

void ObjectLayer::ForEach(const std::function<void(const ObjectInstance&)>& fn) const {
    for (const std::uint32_t id : impl_->order) {
        ObjectInstance instance;
        if (Get(id, instance)) {
            fn(instance);
        }
    }
}

std::size_t ObjectLayer::Count() const noexcept {
    return impl_->order.size();
}

void ObjectLayer::Clear() noexcept {
    impl_->registry.clear();
    impl_->byId.clear();
    impl_->order.clear();
    impl_->nextId = kFirstObjectId;
}

// ---------------------------------------------------------------------------
// ObjectTable（类型表 + 放置清单的 TOML 解析）
// ---------------------------------------------------------------------------

const ObjectType* ObjectTable::Find(const std::string& id) const noexcept {
    for (const ObjectType& type : types) {
        if (type.id == id) {
            return &type;
        }
    }
    return nullptr;
}

ObjectTable MergeObjectTables(const ObjectTable& base, const ObjectTable& overlay) {
    if (base.destructibleEnabled != overlay.destructibleEnabled) {
        throw std::runtime_error(
            "可编辑层的 [destructible_enabled] 与发布清单不一致（编辑层不得改变破坏总开关）");
    }

    ObjectTable merged = base;  // 保留 base 的 schema_version 与总开关
    merged.types.reserve(merged.types.size() + overlay.types.size());
    for (const ObjectType& type : overlay.types) {
        if (base.Find(type.id) != nullptr) {
            throw std::runtime_error("可编辑层的类型 id 与发布清单重复：[" + type.id + "]");
        }
        merged.types.push_back(type);
    }
    // 加载顺序（[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策五）：
    //   **发布清单 → 应用 [[remove]] → 追加 [[placement]] → 散布**。
    // 先按删除项过滤**发布清单**的落点（同类型 + 平面距离 ≤ ε），再追加本层新增落点 ——
    // 顺序固定 ⇒ "遍历顺序 = 放置顺序"（确定性，红线 7）仍然成立。
    merged.placements = RemovePlacementsByRemoval(base.placements, overlay.removals);
    merged.placements.insert(merged.placements.end(), overlay.placements.begin(), overlay.placements.end());
    merged.scatters.insert(merged.scatters.end(), overlay.scatters.begin(), overlay.scatters.end());
    merged.removals.insert(merged.removals.end(), overlay.removals.begin(), overlay.removals.end());
    return merged;
}

bool IsValidObjectCategory(const std::string& category) noexcept {
    return category == "vegetation" || category == "rock" || category == "prop" || category == "building" ||
           category == "portal" || category == "misc";
}

std::vector<ObjectPlacement> RemovePlacementsByRemoval(const std::vector<ObjectPlacement>& placements,
                                                       const std::vector<ObjectRemoval>&   removals) {
    if (removals.empty()) {
        return placements;  // 无删除项 ⇒ 原样返回（逐位不变）
    }
    std::vector<ObjectPlacement> kept;
    kept.reserve(placements.size());
    for (const ObjectPlacement& placement : placements) {
        bool matched = false;
        for (const ObjectRemoval& removal : removals) {
            if (removal.typeId != placement.typeId) {
                continue;
            }
            if (std::abs(removal.x - placement.x) <= removal.tolerance &&
                std::abs(removal.z - placement.z) <= removal.tolerance) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            kept.push_back(placement);  // 保持原顺序（确定性）
        }
    }
    return kept;
}

ObjectTable ObjectTable::LoadFromFile(const std::filesystem::path& path) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(path.string() + ": TOML 解析失败：" + std::string(error.description()));
    }

    ObjectTable table;
    const std::int64_t version = ReadInt(root, path, "schema_version");
    if (version != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 必须为 " + std::to_string(kSchemaVersion) +
                                 "，实际为 " + std::to_string(version));
    }

    // `destructible_enabled`（**可选**，缺省 `true`）：物件可破坏能力总开关（`plans/v0.5.md` §1.5）。
    // 显式给出时必须是布尔值（`"true"` 这类字符串**不**接受 ⇒ 非法即抛，不静默回退）。
    if (root.contains("destructible_enabled")) {
        const std::optional<bool> enabled = root["destructible_enabled"].value<bool>();
        if (!enabled.has_value()) {
            throw std::runtime_error(path.string() + ": 字段 [destructible_enabled] 缺失或不是布尔值");
        }
        table.destructibleEnabled = *enabled;
    }

    const toml::array* typeArray = root["type"].as_array();
    if (typeArray == nullptr) {
        throw std::runtime_error(path.string() + ": 缺少 [[type]] 段");
    }
    ParseTypeEntries(*typeArray, path, [](const std::string&) -> const ObjectType* { return nullptr; }, table);
    ParsePlacementsAndScatters(root, path, [&table](const std::string& id) { return table.Find(id); }, table);

    return table;
}

ObjectTable ObjectTable::LoadOverlayFromFile(const std::filesystem::path& path, const ObjectTable& base) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(path.string() + ": TOML 解析失败：" + std::string(error.description()));
    }

    ObjectTable overlay;
    const std::int64_t version = ReadInt(root, path, "schema_version");
    if (version != static_cast<std::int64_t>(kSchemaVersion)) {
        throw std::runtime_error(path.string() + ": schema_version 必须为 " + std::to_string(kSchemaVersion) +
                                 "，实际为 " + std::to_string(version));
    }
    overlay.destructibleEnabled = base.destructibleEnabled;
    if (root.contains("destructible_enabled")) {
        const std::optional<bool> enabled = root["destructible_enabled"].value<bool>();
        if (!enabled.has_value()) {
            throw std::runtime_error(path.string() + ": 字段 [destructible_enabled] 缺失或不是布尔值");
        }
        if (*enabled != base.destructibleEnabled) {
            throw std::runtime_error(path.string() +
                                     ": 可编辑层的 [destructible_enabled] 与发布清单不一致（编辑层不得改变破坏总开关）");
        }
    }

    // `[[type]]`（**可选**）：可编辑层**可以**新增类型（不得与发布清单重复）；省略即"只放落点"。
    if (const toml::array* typeArray = root["type"].as_array(); typeArray != nullptr) {
        ParseTypeEntries(*typeArray, path, [&base](const std::string& id) { return base.Find(id); }, overlay);
    }
    ParsePlacementsAndScatters(
        root, path,
        [&base, &overlay](const std::string& id) {
            const ObjectType* fromBase = base.Find(id);
            return fromBase != nullptr ? fromBase : overlay.Find(id);
        },
        overlay);

    return overlay;
}

}  // namespace vx
