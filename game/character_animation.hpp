#pragma once

namespace vx {

/// 主角动画状态（T69 的**最小集合**：idle / run / jump / fall）。
///
/// 与移动状态的对应关系由 `SelectCharacterAnimState` 决定；**播哪段 clip** 由
/// `ClipNameForCharacterState` 决定（注意 `Fall` 的已知取舍）。
enum class CharacterAnimState {
    Idle = 0,  ///< 站在地面上、几乎不动
    Run,       ///< 在地面上移动
    Jump,      ///< 离地且**上升**中
    Fall,      ///< 离地且**下降**中
};

/// 判定"算在移动"的水平速度阈值（格/秒）：低于它算静止。
/// 取小值（0.25）以免微抖动让状态在 Idle / Run 之间反复跳。
inline constexpr float kCharacterRunSpeedThreshold = 0.25F;

/// 判定"在上升 / 下降"的竖直速度阈值（格/秒）：越过它才区分 Jump / Fall。
inline constexpr float kCharacterVerticalSpeedThreshold = 0.05F;

/// **纯函数**：由移动状态选动画状态（确定性、无副作用 ⇒ 可直接单测）。
///
/// `grounded` = 角色是否**被支撑**（`CharacterState::onGround`；比 `walkableGround` 宽，
/// 贴坡下滑也算"在地面上"，这部分动画上按地面处理）。
[[nodiscard]] inline CharacterAnimState SelectCharacterAnimState(bool  grounded,
                                                                 float horizontalSpeed,
                                                                 float verticalSpeed) noexcept {
    if (grounded) {
        return (horizontalSpeed > kCharacterRunSpeedThreshold) ? CharacterAnimState::Run : CharacterAnimState::Idle;
    }
    return (verticalSpeed > kCharacterVerticalSpeedThreshold) ? CharacterAnimState::Jump : CharacterAnimState::Fall;
}

/// **纯函数**：状态 → 模型里的 clip 名。
///
/// **已知取舍（T69，所有者 2026-10-05 裁定）**：占位模型（Quaternius《Casual Female》）**没有独立的
/// `Fall` 动画** ⇒ `Fall` **复用 `Jump`**（用它的空中姿态）。状态机仍区分 `jump` / `fall` 两个状态，
/// 只是两者驱动同一段 clip；将来有正式动作再换（登记见 `docs/plans/v0.3.md` §1.4）。
[[nodiscard]] inline const char* ClipNameForCharacterState(CharacterAnimState state) noexcept {
    switch (state) {
        case CharacterAnimState::Run:
            return "Run";
        case CharacterAnimState::Jump:
            return "Jump";
        case CharacterAnimState::Fall:
            return "Jump";  // 占位：无独立 Fall（见上）
        case CharacterAnimState::Idle:
        default:
            return "Idle";
    }
}

/// **纯函数**：该状态的动画是否**循环播放**。
///
/// `Idle` / `Run` 是**循环**动作 —— 尤其是跑动：若不循环，时间会在 `duration` 处被采样钳位、
/// 角色停在末帧，走动时看着就像"原地滑步"（T69 目视缺陷）。
/// `Jump` / `Fall` 是**一次性**空中姿态：播完定格在末帧，符合"长时间下落时保持该姿态"的占位口径，
/// 循环反而会出现空中重复起跳的怪象。
[[nodiscard]] inline bool LoopsCharacterAnimation(CharacterAnimState state) noexcept {
    return state == CharacterAnimState::Idle || state == CharacterAnimState::Run;
}

}  // namespace vx
