// T69 主角动画状态机（**纯函数**）单测：
//   - 由移动状态选动画状态（静止 / 地面移动 / 上升 / 下降）；
//   - 状态 → clip 名的映射，含 **`Fall` 复用 `Jump`** 这一已登记取舍（owner 2026-10-05 裁定）；
//   - 状态的动画是否**循环播放**（Idle / Run 循环；Jump / Fall 一次性）。

#include "character_animation.hpp"

#include <gtest/gtest.h>

namespace {

using vx::CharacterAnimState;
using vx::ClipNameForCharacterState;
using vx::kCharacterRunSpeedThreshold;
using vx::kCharacterVerticalSpeedThreshold;
using vx::LoopsCharacterAnimation;
using vx::SelectCharacterAnimState;

}  // namespace

TEST(CharacterAnimation, GroundedAndStillIsIdle) {
    EXPECT_EQ(SelectCharacterAnimState(/*grounded=*/true, /*horizontalSpeed=*/0.0F, 0.0F), CharacterAnimState::Idle);
    // 阈值处仍是 Idle（严格大于才算移动）。
    EXPECT_EQ(SelectCharacterAnimState(true, kCharacterRunSpeedThreshold, 0.0F), CharacterAnimState::Idle);
    // 竖直速度在着地时不影响判定。
    EXPECT_EQ(SelectCharacterAnimState(true, 0.0F, -5.0F), CharacterAnimState::Idle);
}

TEST(CharacterAnimation, GroundedAndMovingIsRun) {
    EXPECT_EQ(SelectCharacterAnimState(true, kCharacterRunSpeedThreshold + 0.01F, 0.0F), CharacterAnimState::Run);
    EXPECT_EQ(SelectCharacterAnimState(true, 9.0F, -1.0F), CharacterAnimState::Run);
}

TEST(CharacterAnimation, AirborneRisingIsJumpAndDescendingIsFall) {
    EXPECT_EQ(SelectCharacterAnimState(/*grounded=*/false, 0.0F, kCharacterVerticalSpeedThreshold + 0.01F),
              CharacterAnimState::Jump);
    EXPECT_EQ(SelectCharacterAnimState(false, 0.0F, -2.0F), CharacterAnimState::Fall);
    // 恰好落在阈值上不算上升（区分方向要明确）。
    EXPECT_EQ(SelectCharacterAnimState(false, 0.0F, kCharacterVerticalSpeedThreshold), CharacterAnimState::Fall);
    // 水平速度不影响空中状态。
    EXPECT_EQ(SelectCharacterAnimState(false, 20.0F, -2.0F), CharacterAnimState::Fall);
}

TEST(CharacterAnimation, ClipNameMappingAndFallSubstitution) {
    EXPECT_STREQ(ClipNameForCharacterState(CharacterAnimState::Idle), "Idle");
    EXPECT_STREQ(ClipNameForCharacterState(CharacterAnimState::Run), "Run");
    EXPECT_STREQ(ClipNameForCharacterState(CharacterAnimState::Jump), "Jump");
    // **已登记取舍**：占位模型无独立 Fall ⇒ 复用 Jump（详见 docs/plans/v0.3.md §1.4）。
    EXPECT_STREQ(ClipNameForCharacterState(CharacterAnimState::Fall), "Jump");
}

TEST(CharacterAnimation, LoopOnlyAppliesToGroundLocomotion) {
    // 循环动作：Idle / Run（跑动必须循环，否则停在末帧 ⇒ 走动看着像滑步）。
    EXPECT_TRUE(LoopsCharacterAnimation(CharacterAnimState::Idle));
    EXPECT_TRUE(LoopsCharacterAnimation(CharacterAnimState::Run));
    // 一次性动作：Jump / Fall（播完定格在末帧，循环反而会出现空中重复起跳）。
    EXPECT_FALSE(LoopsCharacterAnimation(CharacterAnimState::Jump));
    EXPECT_FALSE(LoopsCharacterAnimation(CharacterAnimState::Fall));
}
