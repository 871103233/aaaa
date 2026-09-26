#include "core/frame_limiter.hpp"

#include <SDL3/SDL.h>

namespace vx {

FrameLimiter::FrameLimiter() noexcept
    : m_frequency(SDL_GetPerformanceFrequency()),
      m_lastCounter(SDL_GetPerformanceCounter()) {}

void FrameLimiter::SetTargetFps(int fps) noexcept {
    if (fps < 0) {
        fps = 0;  // 负值统一按"不限帧"处理（文档化：0 / 负值均不限帧）
    }
    if (fps == m_targetFps) {
        return;
    }
    m_targetFps   = fps;
    m_lastCounter = SDL_GetPerformanceCounter();  // 重置基准：切换目标不补睡
}

double FrameLimiter::Throttle() noexcept {
    if (m_targetFps <= 0 || m_frequency == 0) {
        return 0.0;  // 不限帧（或计时器不可用）：不睡眠
    }

    const std::uint64_t now          = SDL_GetPerformanceCounter();
    const std::uint64_t elapsedTicks = (now >= m_lastCounter) ? (now - m_lastCounter) : 0U;
    const double        elapsed      = static_cast<double>(elapsedTicks) / static_cast<double>(m_frequency);
    const double        sleepSeconds = FrameIntervalSeconds(m_targetFps) - elapsed;
    if (sleepSeconds <= 0.0) {
        m_lastCounter = now;  // 本帧已经不慢于目标，无需睡眠
        return 0.0;
    }

    // 单次睡眠，绝不忙等；SDL_DelayNS 保证"至少睡够"，因此绝不会睡得比请求更短。
    const std::uint64_t sleepNanoseconds = static_cast<std::uint64_t>(sleepSeconds * 1.0e9);
    SDL_DelayNS(sleepNanoseconds);
    m_lastCounter = SDL_GetPerformanceCounter();
    return sleepSeconds;
}

}  // namespace vx
