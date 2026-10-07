// 摆放旋转手感（点按一步 / 长按连续转）的纯函数测试（V0.11 / I1b）。
//
// 判据（可判定）：
//   1. 点按 = **恰好一步**（即使当帧同时处于"按住"状态，也不叠加按住量）；
//   2. 长按**已生效**（`PlacementHoldActive`）⇒ 增量 = 速率 × 帧时长（钳到 `maxFrameSeconds`）；
//   3. 速率 `<= 0` ⇒ 长按无效（只有点按）；
//   4. 左右同时点按 ⇒ 抵消为 0；非法 dt（负）⇒ 0；
//   5. **长按启动延迟**（V0.11，2026-10-08）：按住未满 `holdDelaySeconds` ⇒ **不**生效（轻点不漂）。

#include "placement_rotate.hpp"

#include <gtest/gtest.h>

namespace {

using vx::PlacementHoldActive;
using vx::PlacementRotateSettings;
using vx::PlacementRotationDeltaDegrees;

constexpr double kFrame = 1.0 / 60.0;

}  // namespace

TEST(PlacementRotate, TapIsExactlyOneStep) {
    const PlacementRotateSettings settings;  // 缺省 step 1 / rate 90（V0.11：15 → 1、180 → 90）
    // 点按（当帧同样"长按已生效"）⇒ 恰好一步，不叠加按住量。
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(true, true, false, false, kFrame, settings), -1.0);
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(false, false, true, true, kFrame, settings), 1.0);
}

TEST(PlacementRotate, HoldActiveAdvancesByRateTimesDelta) {
    const PlacementRotateSettings settings;  // rate 90 度/秒
    EXPECT_NEAR(PlacementRotationDeltaDegrees(false, true, false, false, kFrame, settings), -1.5, 1e-9);
    EXPECT_NEAR(PlacementRotationDeltaDegrees(false, false, false, true, kFrame, settings), 1.5, 1e-9);
    // 长按 1 秒（按 1/60 累积 60 帧）≈ 速率。
    double total = 0.0;
    for (int i = 0; i < 60; ++i) {
        total += PlacementRotationDeltaDegrees(false, true, false, false, kFrame, settings);
    }
    EXPECT_NEAR(total, -90.0, 1e-6);
}

TEST(PlacementRotate, QuickTapDoesNotDrift) {
    const PlacementRotateSettings settings;  // holdDelay 缺省 0.4 s
    // 按下那一帧：尚未达到 0.4 s ⇒ `PlacementHoldActive == false` ⇒ 恰好一步、**不叠加**连续量。
    EXPECT_FALSE(PlacementHoldActive(/*held=*/true, /*heldSeconds=*/0.0, settings));
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(true, false, false, false, kFrame, settings), -1.0);
    // 按住 0.3 s（仍未达 0.4 s）⇒ 连续量恒 0。
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(false, false, false, false, kFrame, settings), 0.0);
}

TEST(PlacementRotate, HoldDelayGate) {
    PlacementRotateSettings settings;  // holdDelay 缺省 0.4 s
    EXPECT_FALSE(PlacementHoldActive(false, 1.0, settings));          // 未按住 ⇒ 恒 false
    EXPECT_FALSE(PlacementHoldActive(true, 0.0, settings));           // 刚按下
    EXPECT_FALSE(PlacementHoldActive(true, 0.39, settings));          // 未到 0.4 s
    EXPECT_FALSE(PlacementHoldActive(true, 0.4 - 1e-9, settings));
    EXPECT_TRUE(PlacementHoldActive(true, 0.4, settings));            // 到 0.4 s ⇒ 生效
    EXPECT_TRUE(PlacementHoldActive(true, 0.8, settings));
    // 延迟设为 0 ⇒ 按住即生效（但**未按住**仍 false，避免 `0 >= 0` 误判）。
    settings.holdDelaySeconds = 0.0;
    EXPECT_TRUE(PlacementHoldActive(true, 0.0, settings));
    EXPECT_FALSE(PlacementHoldActive(false, 0.0, settings));
}

TEST(PlacementRotate, HoldIsClampedPerFrame) {
    PlacementRotateSettings settings;
    settings.maxFrameSeconds = 0.1;
    settings.rateDegPerSec   = 100.0;
    // dt 远大于上限 ⇒ 只计 0.1 s ⇒ 10 度（防空转 / 卡顿后突跳）。
    EXPECT_NEAR(PlacementRotationDeltaDegrees(false, true, false, false, 5.0, settings), -10.0, 1e-9);
    // 负 dt ⇒ 计 0。
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(false, true, false, false, -1.0, settings), 0.0);
}

TEST(PlacementRotate, NonPositiveRateDisablesHold) {
    PlacementRotateSettings settings;
    settings.rateDegPerSec = 0.0;
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(false, true, false, false, kFrame, settings), 0.0);
    // 但点按仍有效。
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(true, true, false, false, kFrame, settings), -1.0);
}

TEST(PlacementRotate, OppositeInputsCancel) {
    const PlacementRotateSettings settings;
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(true, true, true, true, kFrame, settings), 0.0);
    EXPECT_DOUBLE_EQ(PlacementRotationDeltaDegrees(false, true, false, true, kFrame, settings), 0.0);
}

TEST(PlacementRotate, IsDeterministic) {
    PlacementRotateSettings settings;
    for (int i = 0; i < 8; ++i) {
        const double dt = 0.005 * static_cast<double>(i + 1);
        EXPECT_EQ(PlacementRotationDeltaDegrees(false, true, false, false, dt, settings),
                  PlacementRotationDeltaDegrees(false, true, false, false, dt, settings));
    }
}
