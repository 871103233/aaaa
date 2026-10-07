#pragma once

#include "object/object_layer.hpp"
#include "terrain/terrain_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace vx {

/// **一次编辑操作的种类**（V0.11 / I3）。
///
/// 覆盖摆放模式里**会改可编辑层数据**的动作（[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) /
/// [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md)）。
enum class EditOpKind : std::uint8_t {
    PlaceObject,           ///< 放置单件物件（可编辑层 `[[placement]]` 追加）
    RemoveObject,          ///< 删除单件物件（本层条目删除 **或** `[[remove]]` 追加）
    PlaceBuilding,         ///< 放置成套建筑（`[[building]]` 追加；可能连带改地形）
    RemoveBuilding,        ///< 删除成套建筑（本层条目删除 **或** `[[remove_building]]` 追加）
    SetBuildingDarkening,  ///< 改某座建筑的室内变暗
    MoveObject,            ///< V0.11 / I4：gizmo 拖动**单件物件**（改落点 x/z/yaw）
    MoveBuilding,          ///< V0.11 / I4：gizmo 拖动**成套建筑**（改锚点 x/z/yaw）
};

/// **室内变暗改动的落点**（撤销 / 重做要知道改的是哪一处）。
enum class EditDarkeningTarget : std::uint8_t {
    EditLayerBuilding,  ///< 本层新增建筑的 `interiorDarkening` 字段（`buildings[i]`）
    ExistingOverride,   ///< 已有 `[[building_darkening]]` 覆盖（`buildingDarkenings[i]`）
    NewOverride,        ///< 新增 `[[building_darkening]]` 覆盖（`buildingDarkenings[i]`）
};

/// **一次编辑操作的可逆记录**（**纯数据**；`Apply` / `Revert` 都只改**可编辑层数据**，不碰 GPU / 物理）。
///
/// 业界参照（命令栈形态）：UE5 Editor Transaction、Unity `Undo`、Godot `UndoRedo` —— 每次操作记一条可逆记录，
/// 新操作**清空重做栈**，栈有**容量上限**（见 [`EditHistory`](#EditHistory)）。
/// 运行期（渲染网格 / 碰撞体 / 地形）的对应动作由 `game/` 侧按同一条命令执行（本模块不认识 GPU / Jolt）。
struct EditCommand {
    EditOpKind kind = EditOpKind::PlaceObject;

    // ---- PlaceObject / RemoveObject（单件）----
    ObjectPlacement placement {};                 ///< 该单件物件的落点（运行期重建 / 撤销回插都用它）
    ObjectRemoval   removal {};                   ///< `!objectFromEditLayer` 时，追加到 `removals` 的那条
    bool            objectFromEditLayer = false;  ///< RemoveObject：删的是**本层 placements**（true）还是记 `removals`（false）
    std::size_t     objectIndex = 0;              ///< 对应上面那条：`placements` 的被删下标 **或** `removals` 的新增下标
    std::uint32_t   runtimeObjectId = 0;          ///< PlaceObject：运行期实例 id（撤销时按 id 精确销毁；0 = 未知）

    // ---- PlaceBuilding / RemoveBuilding（成套建筑）----
    ObjectBuilding building {};                     ///< 整座建筑定义（撤销重建 / 重做展开都用它）
    bool           buildingFromEditLayer = false;   ///< RemoveBuilding：删的是**本层 buildings**（true）还是记 `remove_building`（false）
    std::size_t    buildingIndex = 0;               ///< `buildings` 的被删下标 **或** `buildingRemovals` 的新增下标
    /// RemoveBuilding（!buildingFromEditLayer）：被**一并清掉**的变暗覆盖（原下标 + 值）⇒ 撤销时按原下标回插。
    std::vector<std::pair<std::size_t, ObjectBuildingDarkening>> erasedDarkenings {};

    // ---- SetBuildingDarkening ----
    std::string         buildingId {};                                        ///< 目标建筑 id
    float               darkeningBefore = 0.0F;                               ///< 改前值
    float               darkeningAfter  = 0.0F;                               ///< 改后值
    EditDarkeningTarget darkeningTarget = EditDarkeningTarget::EditLayerBuilding;
    std::size_t         darkeningIndex  = 0;                                  ///< 见 `EditDarkeningTarget`

    // ---- 地形改动（落点 ①压平 / ③填充产生；可为空）----
    std::vector<TerrainColumnEdit> terrainEdits {};

    // ---- MoveObject（gizmo 拖动单件；V0.11 / I4）----
    ObjectPlacement moveObjectFrom {};             ///< 改前落点
    ObjectPlacement moveObjectTo {};               ///< 改后落点
    bool            moveObjectInEditLayer = false; ///< 改的是**本层 placements**（true）还是"发布清单条目 ⇒ 记删 + 落新"（false）
    std::size_t     moveObjectIndex = 0;           ///< `true`：`placements` 下标
    ObjectRemoval   moveObjectRemoval {};          ///< `false`：追加的 `[[remove]]`（匹配旧落点）
    std::size_t     moveObjectRemovalIndex = 0;    ///< `false`：`removals` 下标
    std::size_t     moveObjectPlacementIndex = 0;  ///< `false`：`placements` 下标（新落点）

    // ---- MoveBuilding（gizmo 拖动成套建筑；V0.11 / I4）----
    ObjectBuilding moveBuildingFrom {};               ///< 改前建筑定义
    ObjectBuilding moveBuildingTo {};                 ///< 改后建筑定义（`false` 分支用**新 id**，避免与发布清单重名）
    bool           moveBuildingInEditLayer = false;
    std::size_t    moveBuildingIndex = 0;             ///< `true`：`buildings` 下标
    std::size_t    moveBuildingRemovalIndex = 0;      ///< `false`：`buildingRemovals` 下标
    std::size_t    moveBuildingPlacementIndex = 0;    ///< `false`：`buildings` 下标（新建筑）
    /// `false` 分支：被一并清掉的**旧 id** 的变暗覆盖（原下标 + 值）⇒ 撤销时按原下标回插。
    std::vector<std::pair<std::size_t, ObjectBuildingDarkening>> moveErasedDarkenings {};
};

/// 把一条命令**正向**落到可编辑层数据（重做 / 与"首次执行后的状态"对齐）。
inline void ApplyEditCommand(ObjectTable& editLayer, const EditCommand& command) {
    switch (command.kind) {
        case EditOpKind::PlaceObject: {
            const std::size_t index = std::min(command.objectIndex, editLayer.placements.size());
            editLayer.placements.insert(editLayer.placements.begin() + static_cast<std::ptrdiff_t>(index),
                                        command.placement);
            break;
        }
        case EditOpKind::RemoveObject: {
            if (command.objectFromEditLayer) {
                if (command.objectIndex < editLayer.placements.size()) {
                    editLayer.placements.erase(editLayer.placements.begin() +
                                               static_cast<std::ptrdiff_t>(command.objectIndex));
                }
            } else {
                const std::size_t index = std::min(command.objectIndex, editLayer.removals.size());
                editLayer.removals.insert(editLayer.removals.begin() + static_cast<std::ptrdiff_t>(index),
                                          command.removal);
            }
            break;
        }
        case EditOpKind::PlaceBuilding: {
            const std::size_t index = std::min(command.buildingIndex, editLayer.buildings.size());
            editLayer.buildings.insert(editLayer.buildings.begin() + static_cast<std::ptrdiff_t>(index),
                                       command.building);
            break;
        }
        case EditOpKind::RemoveBuilding: {
            if (command.buildingFromEditLayer) {
                if (command.buildingIndex < editLayer.buildings.size()) {
                    editLayer.buildings.erase(editLayer.buildings.begin() +
                                              static_cast<std::ptrdiff_t>(command.buildingIndex));
                }
            } else {
                const std::size_t index = std::min(command.buildingIndex, editLayer.buildingRemovals.size());
                ObjectBuildingRemoval entry;
                entry.buildingId = command.buildingId;
                editLayer.buildingRemovals.insert(
                    editLayer.buildingRemovals.begin() + static_cast<std::ptrdiff_t>(index), entry);
                // 覆盖按**下标降序**删除（先删大的，避免前面下标失效）。
                for (auto it = command.erasedDarkenings.rbegin(); it != command.erasedDarkenings.rend(); ++it) {
                    if (it->first < editLayer.buildingDarkenings.size()) {
                        editLayer.buildingDarkenings.erase(
                            editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(it->first));
                    }
                }
            }
            break;
        }
        case EditOpKind::SetBuildingDarkening: {
            if (command.darkeningTarget == EditDarkeningTarget::EditLayerBuilding) {
                if (command.darkeningIndex < editLayer.buildings.size()) {
                    editLayer.buildings[command.darkeningIndex].interiorDarkening = command.darkeningAfter;
                }
            } else if (command.darkeningTarget == EditDarkeningTarget::ExistingOverride) {
                if (command.darkeningIndex < editLayer.buildingDarkenings.size()) {
                    editLayer.buildingDarkenings[command.darkeningIndex].darkening = command.darkeningAfter;
                }
            } else {
                const std::size_t index = std::min(command.darkeningIndex, editLayer.buildingDarkenings.size());
                ObjectBuildingDarkening entry;
                entry.buildingId = command.buildingId;
                entry.darkening  = command.darkeningAfter;
                editLayer.buildingDarkenings.insert(
                    editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(index), entry);
            }
            break;
        }
        case EditOpKind::MoveObject: {
            if (command.moveObjectInEditLayer) {
                if (command.moveObjectIndex < editLayer.placements.size()) {
                    editLayer.placements[command.moveObjectIndex] = command.moveObjectTo;
                }
            } else {
                const std::size_t removalIndex = std::min(command.moveObjectRemovalIndex, editLayer.removals.size());
                editLayer.removals.insert(editLayer.removals.begin() + static_cast<std::ptrdiff_t>(removalIndex),
                                          command.moveObjectRemoval);
                const std::size_t placementIndex =
                    std::min(command.moveObjectPlacementIndex, editLayer.placements.size());
                editLayer.placements.insert(editLayer.placements.begin() + static_cast<std::ptrdiff_t>(placementIndex),
                                            command.moveObjectTo);
            }
            break;
        }
        case EditOpKind::MoveBuilding: {
            if (command.moveBuildingInEditLayer) {
                if (command.moveBuildingIndex < editLayer.buildings.size()) {
                    editLayer.buildings[command.moveBuildingIndex] = command.moveBuildingTo;
                }
            } else {
                const std::size_t removalIndex =
                    std::min(command.moveBuildingRemovalIndex, editLayer.buildingRemovals.size());
                ObjectBuildingRemoval removal;
                removal.buildingId = command.moveBuildingFrom.id;
                editLayer.buildingRemovals.insert(
                    editLayer.buildingRemovals.begin() + static_cast<std::ptrdiff_t>(removalIndex), removal);
                // 旧 id 的变暗覆盖按**下标降序**删除（与 `RemoveBuilding` 同口径）。
                for (auto it = command.moveErasedDarkenings.rbegin(); it != command.moveErasedDarkenings.rend(); ++it) {
                    if (it->first < editLayer.buildingDarkenings.size()) {
                        editLayer.buildingDarkenings.erase(
                            editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(it->first));
                    }
                }
                const std::size_t buildingIndex =
                    std::min(command.moveBuildingPlacementIndex, editLayer.buildings.size());
                editLayer.buildings.insert(editLayer.buildings.begin() + static_cast<std::ptrdiff_t>(buildingIndex),
                                           command.moveBuildingTo);
            }
            break;
        }
    }
}

/// 把一条命令**反向**从可编辑层数据撤销（与 `ApplyEditCommand` 严格互逆）。
inline void RevertEditCommand(ObjectTable& editLayer, const EditCommand& command) {
    switch (command.kind) {
        case EditOpKind::PlaceObject: {
            if (command.objectIndex < editLayer.placements.size()) {
                editLayer.placements.erase(editLayer.placements.begin() +
                                           static_cast<std::ptrdiff_t>(command.objectIndex));
            }
            break;
        }
        case EditOpKind::RemoveObject: {
            if (command.objectFromEditLayer) {
                const std::size_t index = std::min(command.objectIndex, editLayer.placements.size());
                editLayer.placements.insert(editLayer.placements.begin() + static_cast<std::ptrdiff_t>(index),
                                            command.placement);
            } else {
                if (command.objectIndex < editLayer.removals.size()) {
                    editLayer.removals.erase(editLayer.removals.begin() +
                                             static_cast<std::ptrdiff_t>(command.objectIndex));
                }
            }
            break;
        }
        case EditOpKind::PlaceBuilding: {
            if (command.buildingIndex < editLayer.buildings.size()) {
                editLayer.buildings.erase(editLayer.buildings.begin() +
                                          static_cast<std::ptrdiff_t>(command.buildingIndex));
            }
            break;
        }
        case EditOpKind::RemoveBuilding: {
            if (command.buildingFromEditLayer) {
                const std::size_t index = std::min(command.buildingIndex, editLayer.buildings.size());
                editLayer.buildings.insert(editLayer.buildings.begin() + static_cast<std::ptrdiff_t>(index),
                                           command.building);
            } else {
                if (command.buildingIndex < editLayer.buildingRemovals.size()) {
                    editLayer.buildingRemovals.erase(
                        editLayer.buildingRemovals.begin() + static_cast<std::ptrdiff_t>(command.buildingIndex));
                }
                // 覆盖按**下标升序**回插（恢复删除前的位置）。
                for (const auto& entry : command.erasedDarkenings) {
                    const std::size_t index = std::min(entry.first, editLayer.buildingDarkenings.size());
                    editLayer.buildingDarkenings.insert(
                        editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(index), entry.second);
                }
            }
            break;
        }
        case EditOpKind::SetBuildingDarkening: {
            if (command.darkeningTarget == EditDarkeningTarget::EditLayerBuilding) {
                if (command.darkeningIndex < editLayer.buildings.size()) {
                    editLayer.buildings[command.darkeningIndex].interiorDarkening = command.darkeningBefore;
                }
            } else if (command.darkeningTarget == EditDarkeningTarget::ExistingOverride) {
                if (command.darkeningIndex < editLayer.buildingDarkenings.size()) {
                    editLayer.buildingDarkenings[command.darkeningIndex].darkening = command.darkeningBefore;
                }
            } else {
                if (command.darkeningIndex < editLayer.buildingDarkenings.size()) {
                    editLayer.buildingDarkenings.erase(
                        editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(command.darkeningIndex));
                }
            }
            break;
        }
        case EditOpKind::MoveObject: {
            if (command.moveObjectInEditLayer) {
                if (command.moveObjectIndex < editLayer.placements.size()) {
                    editLayer.placements[command.moveObjectIndex] = command.moveObjectFrom;
                }
            } else {
                if (command.moveObjectPlacementIndex < editLayer.placements.size()) {
                    editLayer.placements.erase(editLayer.placements.begin() +
                                               static_cast<std::ptrdiff_t>(command.moveObjectPlacementIndex));
                }
                if (command.moveObjectRemovalIndex < editLayer.removals.size()) {
                    editLayer.removals.erase(editLayer.removals.begin() +
                                             static_cast<std::ptrdiff_t>(command.moveObjectRemovalIndex));
                }
            }
            break;
        }
        case EditOpKind::MoveBuilding: {
            if (command.moveBuildingInEditLayer) {
                if (command.moveBuildingIndex < editLayer.buildings.size()) {
                    editLayer.buildings[command.moveBuildingIndex] = command.moveBuildingFrom;
                }
            } else {
                if (command.moveBuildingPlacementIndex < editLayer.buildings.size()) {
                    editLayer.buildings.erase(editLayer.buildings.begin() +
                                              static_cast<std::ptrdiff_t>(command.moveBuildingPlacementIndex));
                }
                if (command.moveBuildingRemovalIndex < editLayer.buildingRemovals.size()) {
                    editLayer.buildingRemovals.erase(editLayer.buildingRemovals.begin() +
                                                     static_cast<std::ptrdiff_t>(command.moveBuildingRemovalIndex));
                }
                for (const auto& entry : command.moveErasedDarkenings) {
                    const std::size_t index = std::min(entry.first, editLayer.buildingDarkenings.size());
                    editLayer.buildingDarkenings.insert(
                        editLayer.buildingDarkenings.begin() + static_cast<std::ptrdiff_t>(index), entry.second);
                }
            }
            break;
        }
    }
}

/// **编辑操作的撤销 / 重做栈**（V0.11 / I3；命令栈形态，纯逻辑、无副作用、可单测）。
///
/// 契约：
///   - `Record` 记录**刚执行完**的一次操作：**清空重做栈**，并把命令压入撤销栈；
///   - 撤销栈超过 `capacity` ⇒ **丢弃最旧**（容量上限，业界形态；缺省 [`kDefaultCapacity`](#EditHistory) = 64 ≥ 计划要求的 32）；
///   - `UndoCommand` / `RedoCommand` 返回**待撤销 / 待重做**的命令（调用方据此执行运行期动作 + `Revert` / `Apply` 数据），
///     执行完再 `CommitUndo` / `CommitRedo` 把栈顶转移；
///   - `Clear` 在保存成功后调用 ⇒ **栈与文件一致**（[`plans/v0.11.md`](../../docs/plans/v0.11.md) I3 判据）。
class EditHistory {
public:
    /// 缺省容量：**64 步**（≥ `plans/v0.11.md` I3 要求的"至少 32 步"）。
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit EditHistory(std::size_t capacity = kDefaultCapacity)
        : m_capacity(capacity == 0 ? 1 : capacity) {}

    /// 记录一次**刚做完**的操作（清空重做栈；超容量丢最旧）。
    void Record(const EditCommand& command) {
        m_redo.clear();
        m_undo.push_back(command);
        while (m_undo.size() > m_capacity) {
            m_undo.pop_front();
        }
    }

    [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.empty(); }

    [[nodiscard]] const EditCommand& UndoCommand() const noexcept { return m_undo.back(); }
    [[nodiscard]] const EditCommand& RedoCommand() const noexcept { return m_redo.back(); }

    /// 运行期重建后需要回填**新实例 id** 时用（只有重做 `PlaceObject` 会用到）。
    [[nodiscard]] EditCommand& MutableRedoCommand() noexcept { return m_redo.back(); }

    /// 把撤销栈顶移到重做栈（在**已执行**撤销之后调用）。
    void CommitUndo() {
        m_redo.push_back(std::move(m_undo.back()));
        m_undo.pop_back();
    }

    /// 把重做栈顶移到撤销栈（在**已执行**重做之后调用）。
    void CommitRedo() {
        m_undo.push_back(std::move(m_redo.back()));
        m_redo.pop_back();
    }

    [[nodiscard]] std::size_t UndoCount() const noexcept { return m_undo.size(); }
    [[nodiscard]] std::size_t RedoCount() const noexcept { return m_redo.size(); }
    [[nodiscard]] std::size_t Capacity() const noexcept { return m_capacity; }

    /// 清空两侧（保存成功后调用 ⇒ 栈与文件一致）。
    void Clear() noexcept {
        m_undo.clear();
        m_redo.clear();
    }

private:
    std::deque<EditCommand> m_undo;
    std::deque<EditCommand> m_redo;
    std::size_t             m_capacity;
};

}  // namespace vx
