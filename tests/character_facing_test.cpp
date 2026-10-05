// T86：角色朝向跟随移动方向（**纯函数**）单测。
//   - 由水平速度求目标 yaw（低于阈值 ⇒ 无目标、保持当前朝向）；
//   - 按角速度上限平滑靠拢，且跨 ±π 走**最短弧**。

#include "character_facing.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using vx::AdvanceYawTowards;
using vx::kCharacterFacingSpeedThreshold;
using vx::kCharacterTurnRateRadPerSec;
using vx::TryComputeTargetYaw;

constexpr float kPi = 3.14159265358979323846F;

}  // namespace

TEST(CharacterFacing, StillCharacterHasNoTargetYaw) {
    float yaw = 1.234F;
    EXPECT_FALSE(TryComputeTargetYaw(0.0F, 0.0F, yaw));
    EXPECT_FLOAT_EQ(yaw, 1.234F);  // 未移动时不写回（保持当前朝向）
    // 阈值处仍是"不算移动"（严格大于才算）。
    EXPECT_FALSE(TryComputeTargetYaw(kCharacterFacingSpeedThreshold, 0.0F, yaw));
    EXPECT_TRUE(TryComputeTargetYaw(kCharacterFacingSpeedThreshold + 0.01F, 0.0F, yaw));
}

TEST(CharacterFacing, TargetYawFollowsVelocityDirection) {
    float yaw = 0.0F;
    ASSERT_TRUE(TryComputeTargetYaw(0.0F, 5.0F, yaw));  // +Z ⇒ 0
    EXPECT_NEAR(yaw, 0.0F, 1e-6F);
    ASSERT_TRUE(TryComputeTargetYaw(5.0F, 0.0F, yaw));  // +X ⇒ +π/2
    EXPECT_NEAR(yaw, kPi * 0.5F, 1e-6F);
    ASSERT_TRUE(TryComputeTargetYaw(0.0F, -5.0F, yaw));  // -Z ⇒ ±π
    EXPECT_NEAR(std::fabs(yaw), kPi, 1e-6F);
    ASSERT_TRUE(TryComputeTargetYaw(-5.0F, 0.0F, yaw));  // -X ⇒ -π/2
    EXPECT_NEAR(yaw, -kPi * 0.5F, 1e-6F);
}

TEST(CharacterFacing, AdvanceClampsToStepAndSnapsWhenReachable) {
    // 目标在上限内 ⇒ 精确落到目标（避免在目标附近抖动）。
    EXPECT_FLOAT_EQ(AdvanceYawTowards(0.0F, 0.1F, 1.0F), 0.1F);
    // 超出上限 ⇒ 每次只走 maxStep，方向正确。
    EXPECT_NEAR(AdvanceYawTowards(0.0F, 1.0F, 0.1F), 0.1F, 1e-6F);
    EXPECT_NEAR(AdvanceYawTowards(0.0F, -1.0F, 0.1F), -0.1F, 1e-6F);
}

TEST(CharacterFacing, AdvanceTakesShortestArcAcrossPiBoundary) {
    // 从 +3.0 到 -3.0：大弧是 ≈ -5.99，小弧是 ≈ +0.283 ⇒ 应朝 **+** 方向走一点点。
    const float next = AdvanceYawTowards(3.0F, -3.0F, 0.1F);
    EXPECT_GT(next, 3.0F);
    EXPECT_NEAR(next, 3.1F, 1e-5F);
}

TEST(CharacterFacing, RepeatedAdvanceConvergesToTarget) {
    float       yaw  = 0.0F;
    const float step = kCharacterTurnRateRadPerSec * (1.0F / 60.0F);
    for (int i = 0; i < 600; ++i) {
        yaw = AdvanceYawTowards(yaw, kPi * 0.5F, step);
    }
    EXPECT_NEAR(yaw, kPi * 0.5F, 1e-4F);
}
