#include "core/clock.hpp"
#include "core/fixed_step.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using vx::Clock;
using vx::FixedStepAccumulator;
using vx::kFixedDt;
using vx::kMaxStepsPerFrame;
using vx::StepPlan;

struct DriveResult {
    int    steps            = 0;
    double discardedSeconds = 0.0;
};

/// 用固定帧率把累加器驱动 `totalSeconds` 秒。
/// 帧数取整到最近整数帧，并把总时长**均分**到各帧，保证不同帧率推进的总时长完全相同。
[[nodiscard]] DriveResult drive_at_fps(double fps, double totalSeconds) {
    FixedStepAccumulator accumulator;

    const int    frames = static_cast<int>(std::lround(fps * totalSeconds));
    const double frame  = totalSeconds / static_cast<double>(frames);

    DriveResult result;
    for (int i = 0; i < frames; ++i) {
        const StepPlan plan = accumulator.Advance(frame);
        result.steps += plan.steps;
        result.discardedSeconds += plan.discardedSeconds;
    }
    return result;
}

}  // namespace

// 固定步长与补步上限必须与规范一致（1/60 s、单帧最多 5 步）。
TEST(FixedStep, ConstantsMatchSpec) {
    EXPECT_DOUBLE_EQ(kFixedDt, 1.0 / 60.0);
    EXPECT_EQ(kMaxStepsPerFrame, 5);
    EXPECT_DOUBLE_EQ(FixedStepAccumulator().FixedDt(), kFixedDt);
}

// ① 帧率无关：同样总时长下，30 / 60 / 144 FPS 推进的逻辑步数一致（容差 1 步）。
TEST(FixedStep, LogicStepCountIsFrameRateIndependent) {
    const double totalSeconds = 10.0;

    const DriveResult at30  = drive_at_fps(30.0, totalSeconds);
    const DriveResult at60  = drive_at_fps(60.0, totalSeconds);
    const DriveResult at144 = drive_at_fps(144.0, totalSeconds);

    // 10 s ÷ (1/60 s) = 600 步
    EXPECT_EQ(at30.steps, 600);
    EXPECT_EQ(at60.steps, 600);
    EXPECT_NEAR(static_cast<double>(at144.steps), static_cast<double>(at30.steps), 1.0);
    EXPECT_NEAR(static_cast<double>(at144.steps), static_cast<double>(at60.steps), 1.0);

    // 正常帧率（单帧远小于 5 步）不应丢弃任何时间
    EXPECT_DOUBLE_EQ(at30.discardedSeconds, 0.0);
    EXPECT_DOUBLE_EQ(at60.discardedSeconds, 0.0);
    EXPECT_DOUBLE_EQ(at144.discardedSeconds, 0.0);
}

// ② 巨量补步被钳制到 kMaxStepsPerFrame，且余量不留给后续帧（不死亡螺旋）。
TEST(FixedStep, HugeFrameIsClampedAndDoesNotSpiral) {
    FixedStepAccumulator accumulator;

    const StepPlan plan = accumulator.Advance(10.0);

    EXPECT_EQ(plan.steps, kMaxStepsPerFrame);
    EXPECT_NEAR(plan.discardedSeconds, 10.0 - static_cast<double>(kMaxStepsPerFrame) * kFixedDt, 1e-9);

    // 累加器不得留下欠账：余量必须小于一个固定步
    EXPECT_LT(accumulator.Accumulator(), kFixedDt);

    // 紧随其后的一帧只推进 1 步，证明没有继续"追赶"
    const StepPlan next = accumulator.Advance(kFixedDt);
    EXPECT_EQ(next.steps, 1);
    EXPECT_DOUBLE_EQ(next.discardedSeconds, 0.0);
}

// ③ 无漂移：累计步数 * kFixedDt + 余量 + 丢弃量 == 推入的总时长。
TEST(FixedStep, NoDriftBetweenStepsAccumulatorAndElapsed) {
    FixedStepAccumulator accumulator;

    // 覆盖正常帧、抖动帧、零帧、负帧（钳制为 0）与一次巨量帧
    const double frameSeconds[] = {
        1.0 / 60.0, 1.0 / 30.0, 0.0121, 0.0, -0.5, 0.025, 1.0 / 144.0, 10.0, 0.0166, 0.004,
    };

    int    totalSteps = 0;
    double totalInput = 0.0;
    for (const double frame : frameSeconds) {
        totalInput += (frame > 0.0) ? frame : 0.0;
        totalSteps += accumulator.Advance(frame).steps;
    }

    const double reconstructed = static_cast<double>(totalSteps) * kFixedDt +
                                 accumulator.Accumulator() + accumulator.DiscardedSeconds();
    EXPECT_NEAR(reconstructed, totalInput, 1e-9);
    EXPECT_NEAR(accumulator.TotalSeconds(), totalInput, 1e-12);

    // 不变量：alpha ∈ [0, 1)，步数 ≤ 上限
    const StepPlan plan = accumulator.Advance(1.0 / 60.0);
    EXPECT_GE(plan.alpha, 0.0);
    EXPECT_LT(plan.alpha, 1.0);
    EXPECT_LE(plan.steps, kMaxStepsPerFrame);
    EXPECT_GE(plan.steps, 0);
}

// ④ 时钟读数为单调、非负。
TEST(Clock, ReadsAreMonotonicAndNonNegative) {
    Clock clock;

    EXPECT_GE(clock.ElapsedSeconds(), 0.0);

    double previousElapsed = clock.ElapsedSeconds();
    for (int i = 0; i < 1000; ++i) {
        const double delta = clock.Tick();
        EXPECT_GE(delta, 0.0);
        EXPECT_GE(clock.ElapsedSeconds(), previousElapsed);
        previousElapsed = clock.ElapsedSeconds();
    }
}
