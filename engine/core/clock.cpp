#include "core/clock.hpp"

#include <SDL3/SDL.h>

namespace vx {

Clock::Clock() noexcept
    : m_frequency(SDL_GetPerformanceFrequency()),
      m_lastCounter(SDL_GetPerformanceCounter()) {}

double Clock::Tick() noexcept {
    const std::uint64_t counter = SDL_GetPerformanceCounter();
    const std::uint64_t ticks   = (counter >= m_lastCounter) ? (counter - m_lastCounter) : 0U;

    m_lastCounter  = counter;
    m_deltaSeconds = static_cast<double>(ticks) / static_cast<double>(m_frequency);
    m_elapsedSeconds += m_deltaSeconds;

    return m_deltaSeconds;
}

}  // namespace vx
