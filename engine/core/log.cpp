#include "core/log.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace vx {
namespace {

/// 单条日志的最大长度；超出部分被 `vsnprintf` 截断，保证不分配堆内存。
constexpr std::size_t kLogBufferSize = 1024;

/// 进程起点：性能计数器频率 + 首个采样值。
/// 函数内 static 只初始化一次、之后只读，因此可被多线程并发读取。
struct LogEpoch {
    std::uint64_t frequency;
    std::uint64_t startCounter;
};

[[nodiscard]] const LogEpoch& log_epoch() noexcept {
    static const LogEpoch epoch { SDL_GetPerformanceFrequency(), SDL_GetPerformanceCounter() };
    return epoch;
}

/// 定宽级别名，便于日志按列对齐。
[[nodiscard]] const char* level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

/// 当前最低输出级别（原子，允许运行期调整）。
std::atomic<int> g_minLogLevel { static_cast<int>(LogLevel::Info) };

}  // namespace

void SetLogLevel(LogLevel level) noexcept {
    g_minLogLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel GetLogLevel() noexcept {
    return static_cast<LogLevel>(g_minLogLevel.load(std::memory_order_relaxed));
}

bool IsLogLevelEnabled(LogLevel level) noexcept {
    return static_cast<int>(level) >= g_minLogLevel.load(std::memory_order_relaxed);
}

void LogMessage(LogLevel level, const char* format, ...) noexcept {
    if (!IsLogLevelEnabled(level)) {
        return;
    }

    char message[kLogBufferSize] = {};

    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written < 0) {
        message[0] = '\0';  // 编码错误：丢弃内容，保留前缀仍可定位一次异常日志
    }

    const LogEpoch& epoch   = log_epoch();
    const std::uint64_t now = SDL_GetPerformanceCounter();
    const double elapsedSeconds = static_cast<double>(now - epoch.startCounter) /
                                  static_cast<double>(epoch.frequency);

    std::FILE* stream = (level >= LogLevel::Warn) ? stderr : stdout;

    // 本函数是工程内唯一直接写标准流的位置；其它模块一律经 VX_LOG_* 宏
    std::fprintf(stream, "[%8.3f] [%s] %s\n", elapsedSeconds, level_name(level), message);
    std::fflush(stream);
}

}  // namespace vx
