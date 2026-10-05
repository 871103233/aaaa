#include "platform/command_line.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

#include <cwchar>
#endif

namespace vx {

#if defined(_WIN32)
namespace {

/// 把一段 UTF-16 字符串转成 UTF-8；空指针 / 空串返回空串，转换失败返回空串（不抛）。
[[nodiscard]] std::string WideToUtf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int length = static_cast<int>(std::wcslen(wide));
    if (length == 0) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

}  // namespace
#endif

std::vector<std::string> CommandLineArgumentsUtf8(int argc, char** argv) {
#if defined(_WIN32)
    // `argv` / `argc` 会按 ANSI 代码页弄乱非 ASCII 参数，故一律走宽字符命令行。
    (void)argc;
    (void)argv;
    int     wideArgc = 0;
    LPWSTR* wideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wideArgc);
    if (wideArgv == nullptr) {
        VX_LOG_ERROR("取回宽字符命令行失败（GetLastError=%lu）⇒ 启动参数不可用", static_cast<unsigned long>(::GetLastError()));
        return {};
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(wideArgc > 0 ? wideArgc : 0));
    for (int i = 0; i < wideArgc; ++i) {
        arguments.push_back(WideToUtf8(wideArgv[i]));
    }
    ::LocalFree(wideArgv);
    return arguments;
#else
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return arguments;
#endif
}

}  // namespace vx
