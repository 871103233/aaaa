#pragma once

#include <cstdint>

namespace vx {

/// 纯函数：目标帧率（Hz）→ 帧间隔（秒）。
///
/// `fps <= 0` 表示"不限帧"，返回 `0`（调用方据此判断"无需睡眠"，不得把 0 当作合法间隔）。
[[nodiscard]] inline double FrameIntervalSeconds(int fps) noexcept {
    return (fps > 0) ? (1.0 / static_cast<double>(fps)) : 0.0;
}

/// 限帧方式（T17）：由"目标帧率 vs 显示器刷新率"的关系决定走哪条路径。
enum class FrameCapMode : int {
    VSync = 0,  ///< 目标 == 刷新率：交给**垂直同步**（换页等待，不消耗 CPU）
    Sleep = 1,  ///< 目标 < 刷新率：用**睡眠**把帧间隔补齐（见 `FrameLimiter`）
};

/// 纯函数：按"目标帧率 vs 显示器刷新率"选择限帧方式。
///
/// 规则：
///   - 目标 **等于** 刷新率（或更高） → `VSync`（换页等待本身即可限住）；
///   - 目标 **低于** 刷新率 → `Sleep`（垂直同步限不住更低的档位，必须靠睡眠补齐）；
///   - 刷新率未知（`<= 0`）或目标为"不限帧"（`<= 0`）→ `VSync`：这是**安全兜底**，
///     宁可交给换页等待，也绝不空转。
///
/// 本函数是"呈现模式选择"的唯一纯缝：`Window::SetVSync` 与 `FrameLimiter` 的启用与否都由它派生。
[[nodiscard]] inline FrameCapMode ChooseFrameCapMode(int targetFps, int refreshRate) noexcept {
    if (refreshRate <= 0 || targetFps <= 0 || targetFps >= refreshRate) {
        return FrameCapMode::VSync;
    }
    return FrameCapMode::Sleep;
}

/// 睡眠式帧率限制器（T17）：把每帧间隔补齐到目标值，从而限制帧率。
///
/// **为什么用睡眠而不是忙等**：忙等（`while` 轮询计时器）会把一个 CPU 核心跑满，与"降低无谓消耗"
/// 的目的背道而驰。这里每帧只用**一次** `SDL_DelayNS` 睡到目标间隔，睡眠期间线程让出 CPU。
/// 注意：**禁用** `SDL_DelayPrecise`——它的文档明确写着必要时会忙等（busy wait）。
///
/// **精度权衡**：`SDL_DelayNS` 只保证"至少睡够"，实际唤醒受操作系统调度粒度影响
/// （Windows 上量级约 1 ms），通常**略长**于请求值，因此实测 FPS 会略低于目标（几 % 量级）。
/// 换来的是零忙等、CPU 占用接近零。对"限制无谓帧率"这一目的，该误差完全可接受；
/// 需要更精确的节拍时应改用垂直同步（见 `ChooseFrameCapMode`）。
///
/// 线程约定：只在渲染线程使用；`Throttle` 会睡眠，不得在持有锁时调用。
class FrameLimiter final {
public:
    FrameLimiter() noexcept;

    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    /// 设置目标帧率（Hz）。`fps <= 0` 表示**不限帧**（`Throttle` 立即返回、不睡眠）。
    ///
    /// 切换目标时会重置计时基准，避免切换瞬间按旧基准补睡一整帧。
    void SetTargetFps(int fps) noexcept;

    /// 当前目标帧率（Hz）；`<= 0` 表示不限帧。
    [[nodiscard]] int TargetFps() const noexcept { return m_targetFps; }

    /// 睡到距上一次 `Throttle` 恰好一个目标帧间隔；`TargetFps() <= 0` 时为无操作。
    ///
    /// 返回本帧**请求**睡眠的秒数（`0` 表示无需睡眠）。返回值仅用于观测，不参与任何逻辑判定。
    [[nodiscard]] double Throttle() noexcept;

private:
    std::uint64_t m_frequency   = 0;  ///< 每秒计数器增量（构造期读一次）
    std::uint64_t m_lastCounter = 0;  ///< 上一帧睡眠结束后的计数器值
    int           m_targetFps   = 0;  ///< 目标帧率（Hz），`<= 0` 表示不限帧
};

}  // namespace vx
