#pragma once

#include "input/action_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vx {

/// 输入上下文：**同一条栈**上的层，优先级由低到高（[ADR 0040](../../docs/adr/0040-input-context-stack-and-action-ownership-table.md)）。
///
/// 为什么要有这一层：过去"按键归谁"的判定**散落**在 `game/main.cpp` 的 5 处（发射判据的手写布尔、
/// 各模式分支逐个 `ConsumePressed` 让位、`Esc` 的 5 级 `if/else` 链、`!gizmoDragging`、`suppression` 独立布尔），
/// 新增一种模式 / 一个键要改多处，**A5 已因此漏过一处**（`fireHeld` 少排除 `modifyMode`）。
/// 本模块把"键 ↔ 谁生效"收敛为**一张权威表**（`OwnerOf`），并让"未被子上下文 claim 的动作**自动穿透到 base**"
/// —— 于是移动 / 跳跃 / 飞行在三种模式下**逐位同源**（复用是显式的，不是"恰好没人 gate 它"）。
enum class InputContext : std::uint8_t {
    FreeRoam = 0,  ///< **自由活动**（base，永在栈底）：左键 = 光球、`E` = 传送门交互
    Build,         ///< **建造**（即既有"摆放模式"）：左键 = 放下
    Modify,        ///< **修改**（未拖动）：左键 = 点击已有物件选中
    ModifyDrag,    ///< **修改（正在拖动手柄）**：鼠标位移归 gizmo，**不转相机**
    Modal,         ///< **模态面板**（系统面板 / 传送门菜单 / 物件选择器）：玩法输入**全量阻断**
    Count,
};

/// 上下文总数（`InputContext` 的合法下标范围是 `[0, kInputContextCount)`）。
inline constexpr std::size_t kInputContextCount = static_cast<std::size_t>(InputContext::Count);

/// 动作在当前上下文里的**归属**（`OwnerOf` 的返回值）。
///
/// 语义边界：`Free` / `Build` / `Modify` 是"**由谁实现**"；`Inactive` / `Blocked` 是"**谁都不做**"
/// （前者 = 该上下文用不到这个键，后者 = 被模态面板阻断）；`AlwaysOn` = 不受上下文约束的**面板级热键**；
/// `PanelInternal` = **面板内部**的确认 / 控件键（由面板自身门控，**不走路由表**）。
enum class InputOwner : std::uint8_t {
    Free = 0,      ///< 归**自由活动（base）**：光球 / 传送门交互 / 相机 / 移动 / 飞行……
    Build,         ///< 归**建造模式**：放下 / 旋转 / 落点模式 / 室内变暗 / 右键删除
    Modify,        ///< 归**修改模式**：选中 / 拖动 gizmo（`ModifyDrag` 时另含鼠标位移）
    Inactive,      ///< 当前上下文内**无效果**（**消费边沿**、不做事）—— 防残余 / 防同键误触发
    Blocked,       ///< 被**模态面板阻断**（玩法输入绝不泄漏到世界）
    AlwaysOn,      ///< **面板级热键**：任何上下文（含模态）都生效
    PanelInternal, ///< **面板内部**键（`Enter` 确认等）：由面板自身处理，**不参与路由**
};

/// 当前输入上下文状态（由 `game/main.cpp` 每帧填充一次；也是 `Esc` 栈的输入）。
///
/// 不变量：`build` 与 `modify` **互斥**（进入一种即退出另一种，见 SKILL《三种模式与输入隔离》硬规则 2）；
/// `gizmoDragging` 仅在 `modify` 为真时有意义。三个面板布尔互相独立（当前三者对玩法输入的隔离行为相同）。
struct InputContextState {
    bool build             = false;  ///< 建造模式（原"摆放模式"）
    bool modify            = false;  ///< 修改模式
    bool gizmoDragging     = false;  ///< 修改模式内正在拖动手柄
    bool objectPaletteOpen = false;  ///< `F2` 物件选择器打开
    bool portalMenuOpen    = false;  ///< 传送门交互菜单打开
    bool systemPanelOpen   = false;  ///< `Esc` 系统面板打开
};

/// 是否有**模态面板**打开（系统面板 / 传送门菜单 / 物件选择器）。
/// 注意：`F1` 调试面板 / 常驻 HUD 是**只读叠加层**，**不**计入（V0.9 回归口径，见 `gameplay_input.hpp`）。
[[nodiscard]] constexpr bool ModalPanelOpen(const InputContextState& state) noexcept {
    return state.objectPaletteOpen || state.portalMenuOpen || state.systemPanelOpen;
}

/// 当前上下文 = 栈顶。优先级：`Modal` > `ModifyDrag`/`Modify` > `Build` > `FreeRoam`。
[[nodiscard]] constexpr InputContext CurrentContext(const InputContextState& state) noexcept {
    if (ModalPanelOpen(state)) {
        return InputContext::Modal;
    }
    if (state.modify) {
        return state.gizmoDragging ? InputContext::ModifyDrag : InputContext::Modify;
    }
    if (state.build) {
        return InputContext::Build;
    }
    return InputContext::FreeRoam;
}

/// `Esc` 的栈顶弹出目标（`EscPopTarget` 的返回值）。
enum class EscTarget : std::uint8_t {
    CloseObjectPalette = 0,  ///< 先关**物件选择器**（不顺带打开系统面板）
    ExitModify,              ///< 退**修改模式**
    ExitBuild,               ///< 退**建造模式**
    ClosePortalMenu,         ///< 关**传送门菜单**
    ToggleSystemPanel,       ///< 开 / 关**系统面板**（栈已到底）
};

namespace detail {

[[nodiscard]] constexpr std::size_t ActionIndex(ActionId action) noexcept {
    return static_cast<std::size_t>(action);
}

/// 构造**某一个上下文**的归属行。
///
/// 构造口径：**默认 = `Free`**（"新增动作默认归 base"是最安全的默认 —— 不写就跟着 base 走），
/// 再按上下文**覆写例外项**。这样"新增动作忘了归类"的后果是"归 base"（可预期），而不是"谁都不处理"或"误触发"。
[[nodiscard]] constexpr std::array<InputOwner, kActionCount> MakeOwnerRow(InputContext context) noexcept {
    std::array<InputOwner, kActionCount> row {};
    for (std::size_t i = 0; i < kActionCount; ++i) {
        row[i] = InputOwner::Free;
    }

    // ---- 面板级热键：任何上下文（含模态面板）都生效，**不受阻断** ----
    // 口径与既有实现一致：`F1` 调试面板 / `F2` 选择器 / `F5` 保存 / `Ctrl+Z`·`Ctrl+Y` 撤销重做 /
    // `Z`·`X`·`B` 摆放开关；`Esc` 由 `EscPopTarget` 处理（栈顶弹出），但归属上仍是"任何上下文都可用"。
    // 依据：这些是**面板级热键**（用户正是在 `F2` 选择器里看它们的状态），若被模态阻断就会出现"面板内按了没反应"
    // （V0.11 / Y1 实测缺陷）。
    row[ActionIndex(ActionId::ToggleDebugPanel)]             = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PickPlacement)]               = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementSave)]               = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementUndo)]               = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementRedo)]               = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementModifierCtrl)]       = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementToggleRotateHold)]   = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementToggleNeighborSnap)] = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::PlacementToggleGridSnap)]     = InputOwner::AlwaysOn;
    row[ActionIndex(ActionId::ToggleSystemPanel)]           = InputOwner::AlwaysOn;

    // ---- 特殊项（与上下文无关）----
    /// T14 遗留：`Esc` 语义统一后**已解绑**（见 `action_state.hpp`）⇒ 恒无效果。
    row[ActionIndex(ActionId::ReleaseMouseCapture)] = InputOwner::Inactive;
    /// 面板内部确认键（`Enter`）：由 `ObjectPaletteOpen()` 自门控，**不走路由表**。
    row[ActionIndex(ActionId::PaletteConfirm)] = InputOwner::PanelInternal;

    switch (context) {
        case InputContext::FreeRoam:
            row[ActionIndex(ActionId::Attack)]              = InputOwner::Free;
            row[ActionIndex(ActionId::Interact)]            = InputOwner::Free;
            row[ActionIndex(ActionId::PlacementRotateLeft)] = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRotateRight)] = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementLandingMode)] = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementDarkenDown)]  = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementDarkenUp)]    = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRemove)]      = InputOwner::Inactive;
            break;
        case InputContext::Build:
            // 建造模式：左键 = 放下（**夺走 base 的光球**）；`E` = 旋转 ⇒ 让位（**不触发传送门交互**）。
            row[ActionIndex(ActionId::Attack)]               = InputOwner::Build;
            row[ActionIndex(ActionId::Interact)]             = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRotateLeft)]  = InputOwner::Build;
            row[ActionIndex(ActionId::PlacementRotateRight)] = InputOwner::Build;
            row[ActionIndex(ActionId::PlacementLandingMode)] = InputOwner::Build;
            row[ActionIndex(ActionId::PlacementDarkenDown)]  = InputOwner::Build;
            row[ActionIndex(ActionId::PlacementDarkenUp)]    = InputOwner::Build;
            row[ActionIndex(ActionId::PlacementRemove)]      = InputOwner::Build;
            break;
        case InputContext::Modify:
        case InputContext::ModifyDrag:
            // 修改模式：左键 = 选中 / 拖动（**夺走 base 的光球**）；`E` 让位；摆放专有键**消费但不生效**。
            row[ActionIndex(ActionId::Attack)]               = InputOwner::Modify;
            row[ActionIndex(ActionId::Interact)]             = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRotateLeft)]  = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRotateRight)] = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementLandingMode)] = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementDarkenDown)]  = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementDarkenUp)]    = InputOwner::Inactive;
            row[ActionIndex(ActionId::PlacementRemove)]      = InputOwner::Modify;
            if (context == InputContext::ModifyDrag) {
                // 拖动期间：鼠标位移**归 gizmo**（相机本帧不转）—— 这是 `ModifyDrag` 存在的唯一理由。
                row[ActionIndex(ActionId::LookX)] = InputOwner::Modify;
                row[ActionIndex(ActionId::LookY)] = InputOwner::Modify;
            }
            break;
        case InputContext::Modal:
            // 模态面板：**玩法输入全量阻断**（绝不泄漏到世界）；面板级热键与面板内部键**保持原归属**。
            for (std::size_t i = 0; i < kActionCount; ++i) {
                if (row[i] == InputOwner::Free || row[i] == InputOwner::Build || row[i] == InputOwner::Modify ||
                    row[i] == InputOwner::Inactive) {
                    row[i] = InputOwner::Blocked;
                }
            }
            break;
        case InputContext::Count:
            break;  // 占位：列出全部枚举值，便于新增上下文时编译器提示"未处理"（无 default）
    }
    return row;
}

[[nodiscard]] constexpr std::array<std::array<InputOwner, kActionCount>, kInputContextCount> MakeOwnerTable() noexcept {
    std::array<std::array<InputOwner, kActionCount>, kInputContextCount> table {};
    for (std::size_t c = 0; c < kInputContextCount; ++c) {
        table[c] = MakeOwnerRow(static_cast<InputContext>(c));
    }
    return table;
}

/// **归属表（单一权威）**：`[上下文][动作] -> 归属`。
/// 修改"某键在某模式下归谁"**只改这张表的构造**（`MakeOwnerRow`），不得在调用点另写判断。
inline constexpr std::array<std::array<InputOwner, kActionCount>, kInputContextCount> kOwnerTable = MakeOwnerTable();

}  // namespace detail

/// 查询：动作在**当前上下文**下的归属。O(1) 查表，无分配、无字符串 ⇒ 可在热路径调用。
[[nodiscard]] constexpr InputOwner OwnerOf(const InputContextState& state, ActionId action) noexcept {
    return detail::kOwnerTable[static_cast<std::size_t>(CurrentContext(state))][detail::ActionIndex(action)];
}

/// `Esc` 的**栈顶弹出**决策（纯函数）：按"栈顶先弹出"的顺序给出目标 ——
/// 物件选择器 → 修改模式 → 建造模式 → 传送门菜单 → 系统面板（栈已到底）。
/// 顺序与引入前的 `if/else` 链**逐位一致**；任意"面板 × 模式"组合都弹出正确的一级。
[[nodiscard]] constexpr EscTarget EscPopTarget(const InputContextState& state) noexcept {
    if (state.objectPaletteOpen) {
        return EscTarget::CloseObjectPalette;
    }
    if (state.modify) {
        return EscTarget::ExitModify;
    }
    if (state.build) {
        return EscTarget::ExitBuild;
    }
    if (state.portalMenuOpen) {
        return EscTarget::ClosePortalMenu;
    }
    return EscTarget::ToggleSystemPanel;
}

}  // namespace vx
