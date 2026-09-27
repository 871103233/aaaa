#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace vx {

/// 由相机 yaw 与前后 / 左右输入求**世界空间**的水平移动方向（纯函数）。
///
/// 基向量约定（与第三人称相机朝向一致，正确性由 `tests/character_test.cpp` 以相机自身基向量锁定）：
///   - 相机前向 = `(sin yaw, 0, cos yaw)` —— 即 `ThirdPersonCamera::Forward()` 的水平投影；
///   - 相机右向 = `cross(前向, 世界上方)` = `(-cos yaw, 0, sin yaw)` —— 即 `glm::lookAt` 视空间的 +X 轴。
///
/// 因而 `forwardInput = +1`（W）沿相机前向、`strafeInput = +1`（D）沿相机右向；
/// 负值分别为后（S）与左（A）。返回向量**未归一化**，长度只反映输入大小，调用方可自行归一化。
///
/// 纯函数：不读取全局状态、不读取输入、不分配内存，可在固定步热路径调用。
[[nodiscard]] inline glm::vec3 CameraRelativeMoveDirection(float yawRadians, float forwardInput,
                                                           float strafeInput) noexcept {
    const glm::vec3 forward(std::sin(yawRadians), 0.0F, std::cos(yawRadians));
    // 右手系下相机右向 = cross(前向, 上方)；写成显式叉乘而非手写分量，避免再次弄错符号。
    const glm::vec3 right = glm::cross(forward, glm::vec3(0.0F, 1.0F, 0.0F));
    return forward * forwardInput + right * strafeInput;
}

/// 跳跃最高点相对角色身高的比例（设计规格：跳跃高度 = 当前身高的 60%）。
inline constexpr float kJumpApexHeightRatio = 0.6F;

/// 由**角色当前身高**与重力**推导**起跳初速度，使跳跃最高点恰为身高的 `kJumpApexHeightRatio` 倍。
///
/// 公式推导：初速度为 `v0` 的竖直抛体，在恒定重力 `g` 下能到达的最大上升高度为
///     `h = v0² / (2 g)`
/// 令设计目标 `h = 0.6 * H`（`H` 为角色总高），反解得
///     `v0 = sqrt(2 * g * 0.6 * H)`
/// 因此身高一旦改变，跳跃高度会按比例自动缩放，无需再改任何写死的速度常数。
///
/// 前置条件：`gravity > 0` 且 `characterHeight > 0`；任一不满足时返回 0（调用方按"不起跳"处理）。
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在固定步热路径调用。
[[nodiscard]] inline float JumpVelocityForHeight(float gravity, float characterHeight) noexcept {
    if (!(gravity > 0.0F) || !(characterHeight > 0.0F)) {
        return 0.0F;
    }
    return std::sqrt(2.0F * gravity * kJumpApexHeightRatio * characterHeight);
}

/// T54：跳跃辅助的两个容差窗口（**唯一口径**；`game/main.cpp` 与单测共用）。
///
/// 为什么需要（业内规范）：`jumpPressed` 是"本帧按下"的**边沿**，而"是否站在可行走地面上"是**连续**量 ——
/// 两者在帧边界上错开一拍就会变成玩家可感知的"按了没跳 / 落地了没跳"。两个窗口分别补两侧：
///   - **土狼时间**（coyote time）：刚离开可行走地面后的 `kJumpCoyoteSeconds` 内仍允许起跳；
///   - **跳跃缓冲**（jump buffer）：落地前 `kJumpBufferSeconds` 内按下的那次请求，在**落地的那一步**兑现。
/// 取值与 60 Hz 固定步（16.7 ms/步）同量级：太短（< 2 步）补不上边界错拍，
/// 太长会让"离地许久仍能起跳 / 空中多段跳"变得可感知。
inline constexpr float kJumpCoyoteSeconds = 0.10F;
inline constexpr float kJumpBufferSeconds = 0.15F;

/// T54：跳跃辅助的跨固定步状态。**必须在固定步里推进**（红线 11：不得用可变帧间隔驱动）。
struct JumpAssist {
    float coyoteTimer = 0.0F;  ///< 距"上一次站在可行走地面上"的剩余容差（秒）
    float bufferTimer = 0.0F;  ///< 距"上一次按下跳跃"的剩余容差（秒）

    void Reset() noexcept {
        coyoteTimer = 0.0F;
        bufferTimer = 0.0F;
    }
};

/// 推进**一个固定步**的跳跃辅助，返回本步是否应当起跳。
///
/// 起跳门槛 = `walkableGround`（Jolt `EGroundState::OnGround`）—— **不含**"站在过陡坡上"，
/// 因此贴着垂直岩壁下滑时**不会**被当成可起跳状态（这正是缺陷 T54 的机制）。
/// 起跳即把两个计时器清零 ⇒ 同一次按键不会被消费两次，也不会出现二段跳。
[[nodiscard]] inline bool AdvanceJumpAssist(JumpAssist& assist, bool walkableGround, bool jumpPressed,
                                            float dt) noexcept {
    assist.coyoteTimer = walkableGround ? kJumpCoyoteSeconds : std::max(0.0F, assist.coyoteTimer - dt);
    assist.bufferTimer = jumpPressed ? kJumpBufferSeconds : std::max(0.0F, assist.bufferTimer - dt);
    if (assist.bufferTimer > 0.0F && assist.coyoteTimer > 0.0F) {
        assist.Reset();
        return true;
    }
    return false;
}

}  // namespace vx
