#pragma once

#include <cmath>

namespace vx {

/// 判定"算在移动、需要更新朝向"的水平速度阈值（格/秒）。
///
/// 与动画状态机的 `kCharacterRunSpeedThreshold` **同值同义**（低于它算静止），
/// 但独立命名：朝向与动画是两条职责，将来任一阈值调整不应隐式牵动另一条。
inline constexpr float kCharacterFacingSpeedThreshold = 0.25F;

/// 转向角速度上限（**弧度/秒**）：`640°/s ≈ 11.17 rad/s` —— 业界默认量级。
///
/// 参照：**UE `CharacterMovementComponent` 默认 `RotationRate = (0, 640, 0)`（度/秒）**、
/// **Unity 第三人称 Starter Assets** 用 `Quaternion.RotateTowards` + 转速、**《原神》**转身约 0.15~0.25 s。
/// 占位手感参数，将来按正式角色再调。
inline constexpr float kCharacterTurnRateRadPerSec = 11.17010722F;

/// 模型**局部前向轴**到"世界 +Z 前向"的偏航修正（弧度）。
///
/// 引擎的朝向约定（与相机一致）：yaw 为 0 时前向 = `+Z`，绕 `+Y` 旋转 `yaw` 后前向 = `(sin yaw, 0, cos yaw)`。
/// 若占位模型自身的正面**不是 +Z**（目视发现角色"背对行进方向"），把本值改为 `π` 即可整体翻转，无需改逻辑。
inline constexpr float kCharacterModelForwardOffsetRad = 0.0F;

/// **纯函数**：由水平速度求目标朝向 yaw（弧度，绕 `+Y`；与相机 yaw 同口径）。
///
/// 速度平方 ≤ 阈值² ⇒ 返回 `false` 表示"当前不算移动"（调用方应**保持当前朝向**，`outYaw` 不变）。
/// 否则写入 `outYaw = atan2(vx, vz)` 并返回 true —— 该式给出的是"速度方向对应的 +Y 旋转角"，
/// 即 yaw 为 0 时指向 `+Z`、`+π/2` 时指向 `+X`。
[[nodiscard]] inline bool TryComputeTargetYaw(float velocityX, float velocityZ, float& outYaw) noexcept {
    const float speedSquared = velocityX * velocityX + velocityZ * velocityZ;
    if (speedSquared <= kCharacterFacingSpeedThreshold * kCharacterFacingSpeedThreshold) {
        return false;
    }
    outYaw = std::atan2(velocityX, velocityZ);
    return true;
}

/// **纯函数**：把 `currentYaw` 朝 `targetYaw` 靠拢，单次最多转 `maxStepRad` 弧度（走**最短弧**）。
///
/// 用 `std::remainder` 把角度差归一到 `(-π, π]` ⇒ 跨 ±π 边界时**不会绕远路**（例：从 `+3.0` 转到 `-3.0`
/// 应走 +0.283 的小弧，而不是 -5.99 的大弧）。差在上限内 ⇒ **精确落到目标**，避免抖动。
[[nodiscard]] inline float AdvanceYawTowards(float currentYaw, float targetYaw, float maxStepRad) noexcept {
    constexpr float kTwoPi = 6.28318530717958647692F;
    const float     delta  = std::remainder(targetYaw - currentYaw, kTwoPi);
    if (delta > maxStepRad) {
        return currentYaw + maxStepRad;
    }
    if (delta < -maxStepRad) {
        return currentYaw - maxStepRad;
    }
    return targetYaw;
}

}  // namespace vx
