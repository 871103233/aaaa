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

/// 度 → 弧度（构件朝向换算；与 `game/main.cpp` 的 `glm::radians` 同口径）。
constexpr double kPiOver180 = 3.14159265358979323846 / 180.0;

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
    if (text == "kit") {
        return ObjectAssetKind::Kit;  // V0.8：模块化建筑构件（必须带 kit_role + module_blocks）
    }
    throw std::runtime_error(path.string() + ": 未知的物件形态 [" + text +
                             "]（可选：dirt_pile / stone / crate / portal / model / kit）");
}

/// 解析 `kit_role`（V0.8；**仅 `kind = "kit"` 可给**）。
[[nodiscard]] ObjectKitRole ParseKitRole(const std::string& text, const std::filesystem::path& path) {
    if (text == "floor") {
        return ObjectKitRole::Floor;
    }
    if (text == "wall") {
        return ObjectKitRole::Wall;
    }
    if (text == "wall_door") {
        return ObjectKitRole::WallDoor;
    }
    if (text == "roof") {
        return ObjectKitRole::Roof;
    }
    throw std::runtime_error(path.string() + ": 未知的构件角色 kit_role [" + text +
                             "]（可选：floor / wall / wall_door / roof）");
}

/// 解析 `landing_mode`（V0.9 起；`[[building]]` 可选；[ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策四）。
///
/// **V0.10 / S5 起放行 ① `flatten` / ③ `fill`**：[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md)
/// 已解除 ADR 0035 决策五（首期只放地形之上、不裁地形），且**地形改动有持久化**（走 `.voxr` 的脏列通路）
/// ⇒ 这两个"会改地形"的落点模式不再报错，由 `game/main.cpp` 在**摆放时**按 footprint 改地形。
[[nodiscard]] ObjectBuildingLandingMode ParseLandingMode(const std::string& text, const std::filesystem::path& path) {
    if (text == "sink") {
        return ObjectBuildingLandingMode::Sink;
    }
    if (text == "flat_only") {
        return ObjectBuildingLandingMode::FlatOnly;
    }
    if (text == "flatten") {
        return ObjectBuildingLandingMode::Flatten;
    }
    if (text == "fill") {
        return ObjectBuildingLandingMode::Fill;
    }
    throw std::runtime_error(path.string() + ": 未知的落点模式 landing_mode [" + text +
                             "]（可选：sink / flat_only / flatten / fill）");
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

        // V0.8：`kit_role` / `module_blocks`（**仅 `kind == "kit"` 可给**；与 `model_file` 同口径：非法即抛）。
        const toml::node* kitRoleNode = entry->get("kit_role");
        const toml::node* moduleNode  = entry->get("module_blocks");
        if (type.kind == ObjectAssetKind::Kit) {
            if (kitRoleNode == nullptr) {
                throw std::runtime_error(path.string() + ": 形态 kit 的类型 [" + type.id + "] 缺少 kit_role");
            }
            const std::optional<std::string> kitRole = kitRoleNode->value<std::string>();
            if (!kitRole.has_value()) {
                throw std::runtime_error(Describe(path, "kit_role") + "不是字符串（id = " + type.id + "）");
            }
            type.kitRole = ParseKitRole(*kitRole, path);

            if (moduleNode == nullptr) {
                throw std::runtime_error(path.string() + ": 形态 kit 的类型 [" + type.id + "] 缺少 module_blocks");
            }
            const std::optional<double> moduleBlocks = moduleNode->value<double>();
            if (!moduleBlocks.has_value() || !(*moduleBlocks > 0.0)) {
                throw std::runtime_error(path.string() + ": 类型 [" + type.id + "] 的 module_blocks 必须是正数");
            }
            type.moduleBlocks = static_cast<float>(*moduleBlocks);

            // **可判定不变量**（ADR 0035 判据③"接缝错位 = 0"）：水平占地必须正好覆盖**整数个模数格**。
            // 容差 1e-4 格（远小于任何可见缝宽），越界即抛 —— 避免"看起来能拼、贴上去有缝"的静默错配。
            const auto isMultiple = [](double extent, double module) {
                const double cells = extent / module;
                return std::abs(cells - std::round(cells)) <= 1.0e-4;
            };
            if (!isMultiple(2.0 * static_cast<double>(type.halfExtentX), *moduleBlocks) ||
                !isMultiple(2.0 * static_cast<double>(type.halfExtentZ), *moduleBlocks)) {
                throw std::runtime_error(path.string() + ": 类型 [" + type.id +
                                         "] 的 module_blocks 必须整除水平尺寸 2*half_extent.x / 2*half_extent.z");
            }
            // **可进入性的硬保证**（ADR 0035 判据①/②）：门洞墙必须**留得下门楣**、且**两侧留得下墙垛**。
            if (type.kitRole == ObjectKitRole::WallDoor) {
                if (!(2.0F * type.halfExtentY > kKitDoorClearanceBlocks)) {
                    throw std::runtime_error(path.string() + ": 类型 [" + type.id + "] 的 wall_door 太矮：2*half_extent.y 必须 > " +
                                             std::to_string(kKitDoorClearanceBlocks) + " 格（否则门洞通到顶、没有门楣）");
                }
                if (!(kKitDoorWidthBlocks < 2.0F * type.halfExtentX)) {
                    throw std::runtime_error(path.string() + ": 类型 [" + type.id + "] 的 wall_door 太窄：门洞宽 " +
                                             std::to_string(kKitDoorWidthBlocks) + " 格必须小于 2*half_extent.x（否则没有墙垛）");
                }
            }
        } else {
            if (kitRoleNode != nullptr) {
                throw std::runtime_error(path.string() + ": 只有形态 kit 可以给出 kit_role（类型 [" + type.id + "]）");
            }
            if (moduleNode != nullptr) {
                throw std::runtime_error(path.string() + ": 只有形态 kit 可以给出 module_blocks（类型 [" + type.id +
                                         "]）");
            }
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

    // V0.6 C3：`[[scatter_tiled]]`（可选）—— **流式（地形感知）散布**（[ADR 0033](../../docs/adr/0033-world-content-placement-and-streaming.md) 决策五）。
    // 与 `[[scatter]]`（圆域、局部手工散布）**并存**：本段是"按 tile 归属 + 地形判据过滤"的形态，内容随 tile 常驻窗口增删。
    // 校验（非法即抛，ADR 0005）：`type` 必须存在；`cell_blocks > 0`；坡度 / 高度区间不得倒置；`landforms` 非空且取值合法。
    if (const toml::array* tiledArray = root["scatter_tiled"].as_array(); tiledArray != nullptr) {
        for (const toml::node& node : *tiledArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[scatter_tiled]] 的每个元素都必须是表");
            }
            ObjectScatterTiled tiled;
            tiled.typeId = ReadString(*entry, path, "type");
            const ObjectType* streamedType = findType(tiled.typeId);
            if (streamedType == nullptr) {
                throw std::runtime_error(path.string() + ": [[scatter_tiled]].type 引用了不存在的类型 [" +
                                         tiled.typeId + "]");
            }
            // 传送门**不可流式**：它需要 `target_world` 与交互登记（`game` 层的门表）⇒ 由流式生成会得到一个
            // "摆着但按 E 没反应"的门（写了却不生效的静默配置）。故一律拒绝，传送门只能走显式 `[[placement]]`。
            if (streamedType->kind == ObjectAssetKind::Portal) {
                throw std::runtime_error(path.string() + ": [[scatter_tiled]] 不支持传送门类型 [" + tiled.typeId +
                                         "]（需 target_world 与交互登记 ⇒ 只能显式 [[placement]]）");
            }
            const std::int64_t seed = static_cast<std::int64_t>(ReadNumber(*entry, path, "seed"));
            if (seed < 0) {
                throw std::runtime_error(path.string() + ": [[scatter_tiled]].seed 不能为负（type = " + tiled.typeId +
                                         "）");
            }
            tiled.seed = static_cast<std::uint64_t>(seed);

            tiled.cellBlocks = static_cast<float>(ReadNumberOr(*entry, path, "cell_blocks", 16.0));
            if (!(tiled.cellBlocks > 0.0F)) {
                throw std::runtime_error(path.string() + ": [[scatter_tiled]].cell_blocks 必须为正（type = " +
                                         tiled.typeId + "）");
            }
            tiled.minSlopeDegrees = static_cast<float>(ReadNumberOr(*entry, path, "min_slope_deg", 0.0));
            tiled.maxSlopeDegrees = static_cast<float>(ReadNumberOr(*entry, path, "max_slope_deg", 45.0));
            if (!(tiled.minSlopeDegrees >= 0.0F && tiled.minSlopeDegrees <= tiled.maxSlopeDegrees &&
                  tiled.maxSlopeDegrees <= 90.0F)) {
                throw std::runtime_error(path.string() +
                                         ": [[scatter_tiled]] 的坡度区间非法（需 0 ≤ min_slope_deg ≤ max_slope_deg ≤ 90，type = " +
                                         tiled.typeId + "）");
            }
            tiled.minHeightBlocks = static_cast<float>(ReadNumberOr(*entry, path, "min_height_blocks", 0.0));
            tiled.maxHeightBlocks = static_cast<float>(ReadNumberOr(*entry, path, "max_height_blocks", 512.0));
            if (!(tiled.minHeightBlocks <= tiled.maxHeightBlocks)) {
                throw std::runtime_error(path.string() +
                                         ": [[scatter_tiled]] 的高度带非法（需 min_height_blocks ≤ max_height_blocks，type = " +
                                         tiled.typeId + "）");
            }

            // `landforms`（可选）：给出后**只**放列出的地貌（值域 plains / hills / mountains；非空、未知值即抛）。
            if (const toml::node* landformsNode = entry->get("landforms"); landformsNode != nullptr) {
                const toml::array* landforms = landformsNode->as_array();
                if (landforms == nullptr) {
                    throw std::runtime_error(path.string() + ": [[scatter_tiled]].landforms 不是数组（type = " +
                                             tiled.typeId + "）");
                }
                if (landforms->empty()) {
                    throw std::runtime_error(path.string() + ": [[scatter_tiled]].landforms 不能为空（type = " +
                                             tiled.typeId + "）");
                }
                tiled.allowPlains    = false;
                tiled.allowHills     = false;
                tiled.allowMountains = false;
                for (const toml::node& item : *landforms) {
                    const std::optional<std::string> name = item.value<std::string>();
                    if (!name.has_value()) {
                        throw std::runtime_error(path.string() +
                                                 ": [[scatter_tiled]].landforms 的元素必须是字符串（type = " +
                                                 tiled.typeId + "）");
                    }
                    if (*name == "plains") {
                        tiled.allowPlains = true;
                    } else if (*name == "hills") {
                        tiled.allowHills = true;
                    } else if (*name == "mountains") {
                        tiled.allowMountains = true;
                    } else {
                        throw std::runtime_error(path.string() + ": [[scatter_tiled]].landforms 含未知值 [" + *name +
                                                 "]（合法值 = plains / hills / mountains）");
                    }
                }
            }

            // 气候区间（V0.6 C7；**可选**，缺省 = 全区间 ⇒ 不约束）。
            tiled.minTemperature = static_cast<float>(ReadNumberOr(*entry, path, "min_temperature", 0.0));
            tiled.maxTemperature = static_cast<float>(ReadNumberOr(*entry, path, "max_temperature", 1.0));
            tiled.minHumidity    = static_cast<float>(ReadNumberOr(*entry, path, "min_humidity", 0.0));
            tiled.maxHumidity    = static_cast<float>(ReadNumberOr(*entry, path, "max_humidity", 1.0));
            if (!(tiled.minTemperature >= 0.0F && tiled.minTemperature <= tiled.maxTemperature &&
                  tiled.maxTemperature <= 1.0F)) {
                throw std::runtime_error(
                    path.string() + ": [[scatter_tiled]] 的温度区间非法（需 0 ≤ min_temperature ≤ max_temperature ≤ 1，type = " +
                    tiled.typeId + "）");
            }
            if (!(tiled.minHumidity >= 0.0F && tiled.minHumidity <= tiled.maxHumidity && tiled.maxHumidity <= 1.0F)) {
                throw std::runtime_error(
                    path.string() + ": [[scatter_tiled]] 的湿度区间非法（需 0 ≤ min_humidity ≤ max_humidity ≤ 1，type = " +
                    tiled.typeId + "）");
            }

            out.tiledScatters.push_back(std::move(tiled));
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

/// 解析 `[[building]]`（V0.8；[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策三）。
///
/// 规则（非法即抛）：`id` 非空且不与**已有建筑**重复；`pieces` 非空；每个 `pieces[].type` 必须存在；
/// **不得引用 `Portal`**（传送门需要 `target_world`，而构件项没有该字段 ⇒ 写了就是静默失效，一律拒绝）。
/// 缺省（配置里没有 `[[building]]`）⇒ 本表为空 ⇒ **与引入本形态之前逐位一致**。
void ParseBuildings(const toml::table& root, const std::filesystem::path& path, const TypeLookup& findType,
                    ObjectTable& out) {
    const toml::array* buildingArray = root["building"].as_array();
    if (buildingArray == nullptr) {
        return;
    }
    for (const toml::node& node : *buildingArray) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            throw std::runtime_error(path.string() + ": [[building]] 的每个元素都必须是表");
        }
        ObjectBuilding building;
        building.id = ReadString(*entry, path, "id");
        if (building.id.empty()) {
            throw std::runtime_error(path.string() + ": [[building]].id 不能为空");
        }
        // V0.9 / ADR 0036 决策四：**建筑 id 不得与任何类型 id 相同** —— 这是"F2 选择器里能无歧义判定
        // 选中的是类型还是建筑"的可判定不变量（按"先查类型、再查建筑"解析即可，不需要额外的类别位）。
        if (findType(building.id) != nullptr) {
            throw std::runtime_error(path.string() + ": [[building]].id [" + building.id +
                                     "] 与某个 [[type]].id 相同（两者是两个命名空间，必须不重名）");
        }
        for (const ObjectBuilding& existing : out.buildings) {
            if (existing.id == building.id) {
                throw std::runtime_error(path.string() + ": [[building]].id 重复 [" + building.id + "]");
            }
        }
        // 锚点水平位置（`position` 与 `[[placement]]` 同写作口径；**y 分量忽略** —— 锚点的地表高度在加载期解算）。
        const std::array<double, 3> anchor = ReadVec3(*entry, path, "position");
        building.x                    = static_cast<float>(anchor[0]);
        building.z                    = static_cast<float>(anchor[2]);
        building.yawDegrees           = static_cast<float>(ReadNumberOr(*entry, path, "yaw_deg", 0.0));

        // V0.9 / ADR 0036 决策二：`interior_darkening`（**可选**，缺省 `-1` = 用全局值）。
        // 语义：`[0, 1]` 为该建筑的室内变暗覆盖；越界 / 非数 ⇒ 抛（非法即抛，ADR 0005）。
        if (const toml::node* darkeningNode = entry->get("interior_darkening"); darkeningNode != nullptr) {
            const std::optional<double> darkening = darkeningNode->value<double>();
            if (!darkening.has_value()) {
                throw std::runtime_error(path.string() + ": [[building]] [" + building.id +
                                         "] 的 interior_darkening 不是数值");
            }
            if (!(*darkening >= 0.0 && *darkening <= 1.0)) {
                throw std::runtime_error(path.string() + ": [[building]] [" + building.id +
                                         "] 的 interior_darkening 必须落在 [0, 1]（0 最暗、1 完全不调暗）");
            }
            building.interiorDarkening = static_cast<float>(*darkening);
        }

        // V0.9 / ADR 0036 决策四：`landing_mode`（**可选**，缺省 `Unspecified` = V0.8 行为）。
        // V0.10 / S5 起 `flatten` / `fill` 也放行（见 `ParseLandingMode`）；改地形的动作在**摆放时**由 `game/` 执行。
        if (const toml::node* landingNode = entry->get("landing_mode"); landingNode != nullptr) {
            const std::optional<std::string> landing = landingNode->value<std::string>();
            if (!landing.has_value()) {
                throw std::runtime_error(path.string() + ": [[building]] [" + building.id +
                                         "] 的 landing_mode 不是字符串");
            }
            building.landingMode = ParseLandingMode(*landing, path);
        }

        const toml::array* pieceArray = (*entry)["pieces"].as_array();
        if (pieceArray == nullptr || pieceArray->empty()) {
            throw std::runtime_error(path.string() + ": [[building]] [" + building.id + "] 的 pieces 不能为空");
        }
        for (const toml::node& pieceNode : *pieceArray) {
            const toml::table* pieceTable = pieceNode.as_table();
            if (pieceTable == nullptr) {
                throw std::runtime_error(path.string() + ": [[building]].pieces 的每个元素都必须是表");
            }
            ObjectBuildingPiece piece;
            piece.typeId = ReadString(*pieceTable, path, "type");
            const ObjectType* pieceType = findType(piece.typeId);
            if (pieceType == nullptr) {
                throw std::runtime_error(path.string() + ": [[building]].pieces.type 引用了不存在的类型 [" +
                                         piece.typeId + "]");
            }
            if (pieceType->kind == ObjectAssetKind::Portal) {
                throw std::runtime_error(path.string() + ": [[building]].pieces 不得引用传送门 [" + piece.typeId +
                                         "]（传送门需要 target_world，构件项没有该字段）");
            }
            const std::array<double, 3> offset = ReadVec3(*pieceTable, path, "offset");
            piece.offsetX   = static_cast<float>(offset[0]);
            piece.offsetY   = static_cast<float>(offset[1]);
            piece.offsetZ   = static_cast<float>(offset[2]);
            piece.yawDegrees = static_cast<float>(ReadNumberOr(*pieceTable, path, "yaw_deg", 0.0));
            building.pieces.push_back(std::move(piece));
        }
        out.buildings.push_back(std::move(building));
    }
}

/// 解析 `[[remove_building]]` 与 `[[building_darkening]]`（V0.9；[ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策三/四）。
///
/// 规则（非法即抛）：`id` 非空；`interior_darkening` 必须 ∈ [0, 1]。
/// **是否引用到存在的建筑**由 `MergeObjectTables` 判定（那里同时看得到发布清单与本层）。
void ParseBuildingEdits(const toml::table& root, const std::filesystem::path& path, ObjectTable& out) {
    if (const toml::array* removeArray = root["remove_building"].as_array(); removeArray != nullptr) {
        for (const toml::node& node : *removeArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[remove_building]] 的每个元素都必须是表");
            }
            ObjectBuildingRemoval removal;
            removal.buildingId = ReadString(*entry, path, "id");
            if (removal.buildingId.empty()) {
                throw std::runtime_error(path.string() + ": [[remove_building]].id 不能为空");
            }
            out.buildingRemovals.push_back(std::move(removal));
        }
    }
    if (const toml::array* darkenArray = root["building_darkening"].as_array(); darkenArray != nullptr) {
        for (const toml::node& node : *darkenArray) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                throw std::runtime_error(path.string() + ": [[building_darkening]] 的每个元素都必须是表");
            }
            ObjectBuildingDarkening override;
            override.buildingId = ReadString(*entry, path, "id");
            if (override.buildingId.empty()) {
                throw std::runtime_error(path.string() + ": [[building_darkening]].id 不能为空");
            }
            const double darkening = ReadNumber(*entry, path, "interior_darkening");
            if (!(darkening >= 0.0 && darkening <= 1.0)) {
                throw std::runtime_error(path.string() + ": [[building_darkening]] [" + override.buildingId +
                                         "] 的 interior_darkening 必须落在 [0, 1]");
            }
            override.darkening = static_cast<float>(darkening);
            out.buildingDarkenings.push_back(std::move(override));
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

const ObjectBuilding* ObjectTable::FindBuilding(const std::string& id) const noexcept {
    for (const ObjectBuilding& building : buildings) {
        if (building.id == id) {
            return &building;
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
    merged.tiledScatters.insert(merged.tiledScatters.end(), overlay.tiledScatters.begin(), overlay.tiledScatters.end());
    merged.removals.insert(merged.removals.end(), overlay.removals.begin(), overlay.removals.end());
    // V0.8：成套建筑按文件顺序追加（发布清单 → 可编辑层），确定性不变（红线 7）。
    // V0.9 / ADR 0036 决策四：先按 `[[remove_building]]`（**按建筑 id 精确匹配**）过滤**发布清单**的建筑，
    // 再追加本层新增建筑 —— 与单件 `[[remove]]` 同源（发布清单只读 ⇒ 差异落在可编辑层）。
    {
        merged.buildings.clear();
        merged.buildings.reserve(base.buildings.size() + overlay.buildings.size());
        for (const ObjectBuilding& building : base.buildings) {
            bool removed = false;
            for (const ObjectBuildingRemoval& removal : overlay.buildingRemovals) {
                if (removal.buildingId == building.id) {
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                merged.buildings.push_back(building);
            }
        }
        merged.buildings.insert(merged.buildings.end(), overlay.buildings.begin(), overlay.buildings.end());
        merged.buildingRemovals.insert(merged.buildingRemovals.end(), overlay.buildingRemovals.begin(),
                                       overlay.buildingRemovals.end());
        // 校验：`[[remove_building]]` 的目标必须存在（在发布清单里）。
        for (const ObjectBuildingRemoval& removal : overlay.buildingRemovals) {
            bool found = false;
            for (const ObjectBuilding& building : base.buildings) {
                if (building.id == removal.buildingId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error("可编辑层的 [[remove_building]] 指向不存在的建筑 id：[" +
                                         removal.buildingId + "]");
            }
        }
        // V0.9 / ADR 0036 决策三：把 `[[building_darkening]]` 覆盖到**合并后**的对应建筑上（**就地改**）；
        // 目标不存在 ⇒ 抛（写错的 id / 已被删 ⇒ 不静默）。
        for (const ObjectBuildingDarkening& override : overlay.buildingDarkenings) {
            bool applied = false;
            for (ObjectBuilding& building : merged.buildings) {
                if (building.id == override.buildingId) {
                    building.interiorDarkening = override.darkening;
                    applied                    = true;
                    break;
                }
            }
            if (!applied) {
                throw std::runtime_error("可编辑层的 [[building_darkening]] 指向不存在的建筑 id：[" +
                                         override.buildingId + "]");
            }
        }
        merged.buildingDarkenings.insert(merged.buildingDarkenings.end(), overlay.buildingDarkenings.begin(),
                                         overlay.buildingDarkenings.end());
    }
    return merged;
}

bool IsValidObjectCategory(const std::string& category) noexcept {
    return category == "vegetation" || category == "rock" || category == "prop" || category == "building" ||
           category == "portal" || category == "misc";
}

ObjectEnclosure ComputeBuildingEnclosure(const ObjectBuilding& building, const ObjectTable& table,
                                        float anchorSurfaceY) noexcept {
    ObjectEnclosure enclosure;
    // 与 `game/main.cpp` 的构件展开**同一约定**（绕 +Y：x' = c·x + s·z、z' = −s·x + c·z）。
    const double yawRadians = static_cast<double>(building.yawDegrees) * kPiOver180;
    const double cosYaw     = std::cos(yawRadians);
    const double sinYaw     = std::sin(yawRadians);

    bool  hasRoof = false;
    double minX = 0.0;
    double maxX = 0.0;
    double minZ = 0.0;
    double maxZ = 0.0;
    double ceiling = 0.0;
    for (const ObjectBuildingPiece& piece : building.pieces) {
        const ObjectType* type = table.Find(piece.typeId);
        if (type == nullptr || type->kind != ObjectAssetKind::Kit || type->kitRole != ObjectKitRole::Roof) {
            continue;  // 兜底：只有屋顶构件参与围合（加载期已保证类型存在）
        }
        // 构件的**总朝向** = 建筑 yaw + 构件附加 yaw ⇒ 旋转后 AABB 的半尺寸按总朝向算。
        const double pieceYaw = yawRadians + static_cast<double>(piece.yawDegrees) * kPiOver180;
        const double absCos   = std::abs(std::cos(pieceYaw));
        const double absSin   = std::abs(std::sin(pieceYaw));
        const double halfX    = absCos * static_cast<double>(type->halfExtentX) +
                             absSin * static_cast<double>(type->halfExtentZ);
        const double halfZ = absSin * static_cast<double>(type->halfExtentX) +
                             absCos * static_cast<double>(type->halfExtentZ);
        const double offsetX = cosYaw * static_cast<double>(piece.offsetX) + sinYaw * static_cast<double>(piece.offsetZ);
        const double offsetZ = -sinYaw * static_cast<double>(piece.offsetX) + cosYaw * static_cast<double>(piece.offsetZ);
        const double centerX = static_cast<double>(building.x) + offsetX;
        const double centerZ = static_cast<double>(building.z) + offsetZ;
        const double bottom  = static_cast<double>(anchorSurfaceY) + static_cast<double>(piece.offsetY);

        if (!hasRoof) {
            hasRoof = true;
            minX = centerX - halfX;
            maxX = centerX + halfX;
            minZ = centerZ - halfZ;
            maxZ = centerZ + halfZ;
            ceiling = bottom;
        } else {
            minX    = std::min(minX, centerX - halfX);
            maxX    = std::max(maxX, centerX + halfX);
            minZ    = std::min(minZ, centerZ - halfZ);
            maxZ    = std::max(maxZ, centerZ + halfZ);
            ceiling = std::min(ceiling, bottom);  // 屋顶并集的**最低**下沿
        }
    }
    if (!hasRoof || !(maxX > minX) || !(maxZ > minZ)) {
        return enclosure;  // 无屋顶（或退化为零面积）⇒ 不是可进入空间，保持 enabled = false
    }
    enclosure.enabled  = true;
    enclosure.centerX  = static_cast<float>((minX + maxX) * 0.5);
    enclosure.centerZ  = static_cast<float>((minZ + maxZ) * 0.5);
    enclosure.halfX    = static_cast<float>((maxX - minX) * 0.5);
    enclosure.halfZ    = static_cast<float>((maxZ - minZ) * 0.5);
    enclosure.ceilingY = static_cast<float>(ceiling);
    // V0.9 / ADR 0036 决策二：把逐建筑变暗覆盖原样带出（`-1` = 用全局值）。
    enclosure.darkening = building.interiorDarkening;
    return enclosure;
}

bool ComputeBuildingFootprintXZ(const ObjectBuilding& building, const ObjectTable& table, float& outMinX,
                                float& outMaxX, float& outMinZ, float& outMaxZ) noexcept {
    // 与 `game/main.cpp` 的构件展开 / `ComputeBuildingEnclosure` **同一约定**（绕 +Y：x' = c·x + s·z、z' = −s·x + c·z）。
    const double yawRadians = static_cast<double>(building.yawDegrees) * kPiOver180;
    const double cosYaw     = std::cos(yawRadians);
    const double sinYaw     = std::sin(yawRadians);

    bool   hasPiece = false;
    double minX = 0.0;
    double maxX = 0.0;
    double minZ = 0.0;
    double maxZ = 0.0;
    for (const ObjectBuildingPiece& piece : building.pieces) {
        const ObjectType* type = table.Find(piece.typeId);
        if (type == nullptr || type->kind != ObjectAssetKind::Kit) {
            continue;  // 兜底：只有 kit 构件参与占地（加载期已保证类型存在）
        }
        const double pieceYaw = yawRadians + static_cast<double>(piece.yawDegrees) * kPiOver180;
        const double absCos   = std::abs(std::cos(pieceYaw));
        const double absSin   = std::abs(std::sin(pieceYaw));
        const double halfX    = absCos * static_cast<double>(type->halfExtentX) +
                             absSin * static_cast<double>(type->halfExtentZ);
        const double halfZ = absSin * static_cast<double>(type->halfExtentX) +
                             absCos * static_cast<double>(type->halfExtentZ);
        const double offsetX = cosYaw * static_cast<double>(piece.offsetX) + sinYaw * static_cast<double>(piece.offsetZ);
        const double offsetZ = -sinYaw * static_cast<double>(piece.offsetX) + cosYaw * static_cast<double>(piece.offsetZ);
        const double centerX = static_cast<double>(building.x) + offsetX;
        const double centerZ = static_cast<double>(building.z) + offsetZ;

        if (!hasPiece) {
            hasPiece = true;
            minX = centerX - halfX;
            maxX = centerX + halfX;
            minZ = centerZ - halfZ;
            maxZ = centerZ + halfZ;
        } else {
            minX = std::min(minX, centerX - halfX);
            maxX = std::max(maxX, centerX + halfX);
            minZ = std::min(minZ, centerZ - halfZ);
            maxZ = std::max(maxZ, centerZ + halfZ);
        }
    }
    if (!hasPiece || !(maxX > minX) || !(maxZ > minZ)) {
        return false;
    }
    outMinX = static_cast<float>(minX);
    outMaxX = static_cast<float>(maxX);
    outMinZ = static_cast<float>(minZ);
    outMaxZ = static_cast<float>(maxZ);
    return true;
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
    // V0.8：`[[building]]`（成套建筑）—— 引用本清单的类型表。
    ParseBuildings(root, path, [&table](const std::string& id) { return table.Find(id); }, table);
    // V0.9：`[[remove_building]]` / `[[building_darkening]]`（存在性校验在 Merge）。
    ParseBuildingEdits(root, path, table);

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
    // V0.8：可编辑层也可追加成套建筑（引用"发布清单 ∪ 本层新增"的类型）。
    ParseBuildings(
        root, path,
        [&base, &overlay](const std::string& id) {
            const ObjectType* fromBase = base.Find(id);
            return fromBase != nullptr ? fromBase : overlay.Find(id);
        },
        overlay);
    // V0.9：`[[remove_building]]` / `[[building_darkening]]`（存在性校验在 Merge）。
    ParseBuildingEdits(root, path, overlay);

    return overlay;
}

}  // namespace vx
