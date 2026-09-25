#include "core/fixed_step.hpp"

#include <cmath>

namespace vx {

FixedStepAccumulator::FixedStepAccumulator(double fixedDt) noexcept : m_fixedDt(fixedDt) {}

StepPlan FixedStepAccumulator::Advance(double frameSeconds) noexcept {
    // 非正值（含负值与 NaN）按 0 处理：时钟理论上不会给出，但守卫成本只有一个比较。
    const double frame = (frameSeconds > 0.0) ? frameSeconds : 0.0;
    m_totalSeconds += frame;

    const double total = m_accumulator + frame;

    // 先按双精度判断需要补几步，再钳制到上限——这样强制转换 int 时必落在 [0, kMaxStepsPerFrame]。
    double       stepsToRun = std::floor(total / m_fixedDt);
    double       discarded  = 0.0;
    if (stepsToRun > static_cast<double>(kMaxStepsPerFrame)) {
        // 超出上限的整步时长直接丢弃，不留给后续帧，避免"越欠越多"的死亡螺旋。
        discarded  = (stepsToRun - static_cast<double>(kMaxStepsPerFrame)) * m_fixedDt;
        stepsToRun = static_cast<double>(kMaxStepsPerFrame);
    }

    m_accumulator = total - (stepsToRun * m_fixedDt) - discarded;
    if (m_accumulator < 0.0) {
        m_accumulator = 0.0;  // 消除浮点减法的微小负值，保证 alpha >= 0
    }
    m_discardedSeconds += discarded;

    StepPlan plan;
    plan.steps            = static_cast<int>(stepsToRun);
    plan.alpha            = m_accumulator / m_fixedDt;
    plan.discardedSeconds = discarded;
    return plan;
}

}  // namespace vx
