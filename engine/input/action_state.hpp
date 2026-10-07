#pragma once

#include <cstddef>
#include <cstdint>

namespace vx {

/// 动作标识：上层（`world/` / `game/`）只认动作，**不认 SDL 键码**（SKILL 红线）。
///
/// 新增动作只能**追加**在 `Count` 之前，不得插队、不得复用（键位绑定与测试都引用这些值）。
enum class ActionId : std::uint8_t {
    MoveForward = 0,
    MoveBackward,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    Attack,  ///< 主笔刷：挖掘
    Use,     ///< 副笔刷：堆建
    LookX,   ///< 模拟动作：本帧鼠标水平位移
    LookY,   ///< 模拟动作：本帧鼠标垂直位移
    ToggleDebugPanel,  ///< 调试面板开关（T9；只在 game/ 消费，不参与模拟）
    ToggleFly,         ///< 飞行模式开关（T12；只在 game/ 消费，不参与模拟）
    FlyDown,           ///< 飞行时下降（T12；只在 game/ 消费，不参与模拟）
    ReleaseMouseCapture,  ///< 释放鼠标相对模式（T14 遗留；T15 起 `Esc` 改绑 `ToggleSystemPanel`，本动作不再绑定）
    ToggleSystemPanel,    ///< 开关系统面板（T15；`Esc`；只在 game/ 消费，不参与模拟）
    Interact,             ///< 交互（V3；`E`；走近传送门按 E 触发切换；只在 game/ 消费，不参与模拟）
    PickPlacement,        ///< 坐标拾取辅助（V0.5 E2；`F2`；只在 game/ 消费）
    PaletteConfirm,       ///< 物件选择器确认（V0.5 E3；`Enter`；只在 game/ 消费）
    PlacementRotateLeft,  ///< 摆放模式：左旋（V0.5 E3；`Q`）
    PlacementRotateRight, ///< 摆放模式：右旋（V0.5 E3；`E`；与 `Interact` 同键 ⇒ **模式内让位**）
    PlacementRepeatLast,  ///< 摆放模式：重复上次类型 + 朝向（V0.5 E3；`F3`）
    PlacementSave,        ///< 保存到可编辑层（V0.5 E3；`F5`；模式内外均可）
    PlacementRemove,      ///< 摆放模式：删除指向的物件（V0.5 E3；鼠标右键）
    PlacementLandingMode, ///< 摆放模式：循环切换成套建筑的**落点模式**（V0.9；`T`；仅建筑摆放）
    PlacementDarkenDown,  ///< 摆放模式：室内变暗 −0.05（V0.9；`[`；预览态调待放值 / 选中态调已有建筑）
    PlacementDarkenUp,    ///< 摆放模式：室内变暗 +0.05（V0.9；`]`；同上）
    PlacementToggleRotateHold,    ///< V0.11：切换"旋转长按模式"（缺省开；`Z`）
    PlacementToggleNeighborSnap,  ///< V0.11：切换"邻居优先吸附"（缺省开；`X`）
    PlacementToggleGridSnap,      ///< V0.11：切换"世界网格吸附"（缺省开；`B`）

    Count,
};

/// 动作总数（`ActionId` 的合法下标范围是 `[0, kActionCount)`）。
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(ActionId::Count);

/// 单个动作在**本帧**的状态快照。
///
/// 语义边界：`held` / `pressed` 只对**数字动作**（键盘绑定）有意义；
/// 模拟动作（鼠标轴，见 `LookX` / `LookY`）只使用 `value`，其 `held` / `pressed` 恒为 `false`。
struct ActionState {
    /// 持续按下（持续按下）：本帧绑定的任一按键处于按下状态。
    bool held = false;

    /// 本帧按下（本帧按下）：本帧发生了一次「抬起 → 按下」的边沿。
    /// 数字动作每帧最多产生一次该边沿；消费后（见 `InputMap::ConsumePressed`）本帧内不再为 `true`。
    bool pressed = false;

    /// 本帧模拟量：鼠标轴动作为本帧累计位移；数字动作为 `held ? 1.0f : 0.0f`。
    float value = 0.0F;
};

}  // namespace vx
