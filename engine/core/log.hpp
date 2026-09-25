#pragma once

namespace vx {

/// 日志级别，数值越大越严重。
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

/// 设置最低输出级别：低于该级别的日志被丢弃。默认 `Info`。
void SetLogLevel(LogLevel level) noexcept;

[[nodiscard]] LogLevel GetLogLevel() noexcept;

/// 该级别当前是否会真正输出。
/// 供调用方在拼接昂贵日志参数前短路，例如：
/// `if (vx::IsLogLevelEnabled(vx::LogLevel::Debug)) { ... }`。
[[nodiscard]] bool IsLogLevelEnabled(LogLevel level) noexcept;

/// 统一日志入口（本工程**唯一**允许直接写 stdout / stderr 的地方）。
///
/// 前缀格式固定为 `[<自进程启动的秒数>] [LEVEL] `，便于按时间与级别检索。
/// 输出目标：`Trace` / `Debug` / `Info` → stdout；`Warn` / `Error` → stderr。
///
/// 前置条件：`format` 非空，且为 printf 风格格式串（参数个数与类型须与格式串一致）。
/// 线程安全：可从任意线程调用；级别过滤在函数内完成。
/// 实现约定：消息在**固定长度栈缓冲**内格式化，不分配堆内存、不抛异常。
/// 热路径请优先用 `VX_LOG_*` 宏（先判级别再格式化）。
void LogMessage(LogLevel level, const char* format, ...) noexcept;

}  // namespace vx

/// 统一日志宏。用法：`VX_LOG_INFO("已加载 %d 个区块", count);`
#define VX_LOG_TRACE(...) ::vx::LogMessage(::vx::LogLevel::Trace, __VA_ARGS__)
#define VX_LOG_DEBUG(...) ::vx::LogMessage(::vx::LogLevel::Debug, __VA_ARGS__)
#define VX_LOG_INFO(...)  ::vx::LogMessage(::vx::LogLevel::Info, __VA_ARGS__)
#define VX_LOG_WARN(...)  ::vx::LogMessage(::vx::LogLevel::Warn, __VA_ARGS__)
#define VX_LOG_ERROR(...) ::vx::LogMessage(::vx::LogLevel::Error, __VA_ARGS__)
