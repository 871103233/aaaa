#pragma once

#include <cstddef>
#include <cstdint>

namespace vx {

/// 动作标识：上层（`voxel/` / `game/`）只认动作，**不认 SDL 键码**（SKILL 红线）。
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
