// T17：限帧的纯函数缝与睡眠式限帧器的单元测试（headless）。
//
// 覆盖：
//   1. `FrameIntervalSeconds`（fps → 秒；0 / 负值表示"不限帧"）；
//   2. `ChooseFrameCapMode`（目标 vs 刷新率 → 垂直同步 / 睡眠限帧；未知刷新率兜底）；
//   3. `FrameLimiter` 的契约：默认不限帧、负值归零、目标为 0 时 `Throttle` 立即返回，
//      以及**确实发生睡眠**（不是忙等）——用粗粒度时间上下界断言，避免依赖精确调度。

#include "core/frame_limiter.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using vx::ChooseFrameCapMode;
using vx::FrameCapMode;
using vx::FrameIntervalSeconds;
using vx::FrameLimiter;

}  // namespace

// 纯函数：帧间隔 = 1 / fps；fps <= 0 表示"不限帧"，返回 0（文档化，不当作合法间隔）。
TEST(FrameLimiter, FrameIntervalFromFps) {
    EXPECT_DOUBLE_EQ(FrameIntervalSeconds(60), 1.0 / 60.0);
    EXPECT_DOUBLE_EQ(FrameIntervalSeconds(144), 1.0 / 144.0);
    EXPECT_DOUBLE_EQ(FrameIntervalSeconds(1), 1.0);

    EXPECT_DOUBLE_EQ(FrameIntervalSeconds(0), 0.0) << "0 表示不限帧";
    EXPECT_DOUBLE_EQ(FrameIntervalSeconds(-30), 0.0) << "负值表示不限帧";
}

// 纯函数：呈现模式选择的唯一缝。目标等于刷新率 → 垂直同步；低于 → 睡眠限帧。
TEST(FrameLimiter, ChooseFrameCapModeByTargetVersusRefresh) {
    EXPECT_EQ(ChooseFrameCapMode(144, 144), FrameCapMode::VSync);  // 等于刷新率 → 垂直同步
    EXPECT_EQ(ChooseFrameCapMode(200, 144), FrameCapMode::VSync);  // 高于（理论上不会发生）→ 垂直同步
    EXPECT_EQ(ChooseFrameCapMode(90, 144), FrameCapMode::Sleep);   // 低于刷新率 → 睡眠限帧
    EXPECT_EQ(ChooseFrameCapMode(60, 144), FrameCapMode::Sleep);

    // 未知刷新率 / 不限帧 → 安全兜底：交给垂直同步，绝不空转
    EXPECT_EQ(ChooseFrameCapMode(120, 0), FrameCapMode::VSync);
    EXPECT_EQ(ChooseFrameCapMode(120, -1), FrameCapMode::VSync);
    EXPECT_EQ(ChooseFrameCapMode(0, 144), FrameCapMode::VSync);
}

// 限帧器默认不限帧：`Throttle` 立即返回、不睡眠。
TEST(FrameLimiter, DefaultsToUnlimitedAndDoesNotSleep) {
    FrameLimiter limiter;
    EXPECT_EQ(limiter.TargetFps(), 0);

    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        EXPECT_DOUBLE_EQ(limiter.Throttle(), 0.0);
    }
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    EXPECT_LT(elapsed.count(), 50) << "不限帧时不得睡眠";
}

// 目标帧率：负值统一归为 0（不限帧）；正值原样保存。
TEST(FrameLimiter, SetTargetFpsRejectsNegative) {
    FrameLimiter limiter;
    limiter.SetTargetFps(-5);
    EXPECT_EQ(limiter.TargetFps(), 0);

    limiter.SetTargetFps(90);
    EXPECT_EQ(limiter.TargetFps(), 90);

    limiter.SetTargetFps(0);
    EXPECT_EQ(limiter.TargetFps(), 0);
}

// 确实发生**睡眠**（而非忙等）：30 Hz 目标下连续两帧，耗时应接近两个帧间隔。
//
// 只断言粗粒度下界（≥ 1 个间隔的一半），避免依赖操作系统调度精度；上界放宽到 1 s 以容忍慢机。
TEST(FrameLimiter, ThrottleSleepsTowardTargetInterval) {
    FrameLimiter limiter;
    limiter.SetTargetFps(30);  // 帧间隔 ≈ 33.3 ms

    const auto begin = std::chrono::steady_clock::now();
    (void)limiter.Throttle();
    (void)limiter.Throttle();
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);

    EXPECT_GE(elapsed.count(), 16) << "两帧 30 Hz 至少应睡过半个帧间隔";
    EXPECT_LT(elapsed.count(), 1000) << "睡眠不得异常偏长（上界仅为兜底）";
}
