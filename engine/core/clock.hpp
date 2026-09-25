#pragma once

#include <cstdint>

namespace vx {

/// 单调时钟：基于 `SDL_GetPerformanceCounter` / `SDL_GetPerformanceFrequency`。
///
/// 意图：为固定步长主循环提供**不受系统校时影响**的帧间隔（红线 11）。
/// 禁止改用 `std::chrono::system_clock`——它会被 NTP / 手动校时回拨，
/// 导致帧间隔出现负值或跳变。
///
/// 前置条件：无。SDL 的性能计数器在 `SDL_Init` 之前也可安全调用。
/// 线程约定：查询接口（`DeltaSeconds` / `ElapsedSeconds`）可在任意线程调用；
/// **同一实例不得被多线程并发 `Tick`**（`Tick` 会推进内部状态）。
class Clock {
public:
    Clock() noexcept;

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    /// 采样一次并推进内部时间。
    /// 返回距上次 `Tick` 的秒数，恒 `>= 0`（计数器出现回绕时钳制为 0，绝不返回负值）。
    [[nodiscard]] double Tick() noexcept;

    /// 最近一次 `Tick` 得到的时间间隔（秒）。
    [[nodiscard]] double DeltaSeconds() const noexcept { return m_deltaSeconds; }

    /// 自构造以来累计的秒数，单调不减。
    [[nodiscard]] double ElapsedSeconds() const noexcept { return m_elapsedSeconds; }

private:
    std::uint64_t m_frequency    = 0;  ///< 每秒钟计数器增量（启动期读一次）
    std::uint64_t m_lastCounter  = 0;  ///< 上次采样值
    double        m_deltaSeconds = 0.0;
    double        m_elapsedSeconds = 0.0;
};

}  // namespace vx
