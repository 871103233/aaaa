#include "platform/console.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vx {

unsigned int EnableUtf8ConsoleOutput() noexcept {
#if defined(_WIN32)
    // 输出代码页决定 `fprintf(stdout / stderr, "<UTF-8 字节>")` 被控制台怎样解码；
    // 输入代码页本工程当前不读控制台，但一并设成 UTF-8，避免将来读入中文时按 ANSI 弄乱。
    // 两者在"无控制台"时返回 0（失败）—— 属预期情形（例：GUI 子系统启动），静默忽略即可。
    (void)::SetConsoleOutputCP(CP_UTF8);
    (void)::SetConsoleCP(CP_UTF8);
    // 回读实际生效值（0 = 没有附加控制台）⇒ 让调用方能把真实结果写进日志，而不是宣称成功。
    return static_cast<unsigned int>(::GetConsoleOutputCP());
#else
    return 0U;  // 非 Windows：终端本身即 UTF-8，无需设置
#endif
}

}  // namespace vx
