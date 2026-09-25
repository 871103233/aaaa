#pragma once

namespace vx {

/// 逻辑与物理的固定步长：1/60 秒（红线 11 —— 禁止用可变帧间隔驱动物理）。
inline constexpr double kFixedDt = 1.0 / 60.0;

/// 单帧最多补的逻辑步数：超出部分**直接丢弃**，防止卡顿后陷入"死亡螺旋"。
inline constexpr int kMaxStepsPerFrame = 5;

/// 一帧的逻辑推进计划（`FixedStepAccumulator::Advance` 的返回值）。
struct StepPlan {
    /// 本帧应执行的固定逻辑步数，范围 `[0, kMaxStepsPerFrame]`。
    int steps = 0;

    /// 渲染插值系数，范围 `[0, 1)`：当前逻辑状态到下一逻辑状态的插值比例。
    /// **只读用于渲染**；调用方**不得**把它写回任何逻辑 / 物理状态（红线 11）。
    double alpha = 0.0;

    /// 本帧被丢弃的秒数。正常帧为 0；卡顿时大于 0，表示为避免死亡螺旋而放弃补齐的部分。
    double discardedSeconds = 0.0;
};

/// 固定步长累加器：把可变帧间隔换算成"本帧跑几步逻辑 + 渲染插值系数"。
///
/// 前置条件：构造参数 `fixedDt > 0`。
/// 线程约定：只在逻辑线程（主线程）推进与查询。
/// 不变量：`Accumulator()` 恒落在 `[0, fixedDt)`，因此 `alpha` 恒在 `[0, 1)`。
class FixedStepAccumulator {
public:
    explicit FixedStepAccumulator(double fixedDt = kFixedDt) noexcept;

    FixedStepAccumulator(const FixedStepAccumulator&) = delete;
    FixedStepAccumulator& operator=(const FixedStepAccumulator&) = delete;

    /// 推入一帧的真实时长并计算本帧的逻辑步计划。
    /// 前置条件：`frameSeconds` 应为该帧真实时长；非正值（含负值与 NaN）按 0 处理。
    /// 超过 `kMaxStepsPerFrame` 的步数对应的时长会计入 `StepPlan::discardedSeconds` 并被丢弃，
    /// **不会**留给后续帧补偿。
    [[nodiscard]] StepPlan Advance(double frameSeconds) noexcept;

    [[nodiscard]] double FixedDt() const noexcept { return m_fixedDt; }

    /// 当前累加器余量（秒），恒在 `[0, fixedDt)`。
    [[nodiscard]] double Accumulator() const noexcept { return m_accumulator; }

    /// 累计推入的帧时长（秒），含被丢弃的部分。
    [[nodiscard]] double TotalSeconds() const noexcept { return m_totalSeconds; }

    /// 累计被丢弃的时长（秒）。
    [[nodiscard]] double DiscardedSeconds() const noexcept { return m_discardedSeconds; }

private:
    double m_fixedDt;
    double m_accumulator      = 0.0;
    double m_totalSeconds     = 0.0;
    double m_discardedSeconds = 0.0;
};

}  // namespace vx
