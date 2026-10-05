#pragma once

#include <string>
#include <vector>

namespace vx {

/// 以 **UTF-8** 取回全部命令行参数（`argv[0]` 为首，可能是可执行文件路径）。
///
/// **为什么不能用 `main` 的 `argv`**：Windows 的 CRT 按 **ANSI 代码页**解码命令行，
/// 含中文的参数会被替换成 `?`（不可逆）。故这里在 Windows 上改经
/// `GetCommandLineW` + `CommandLineToArgvW` 取回 UTF-16，再经 `WideCharToMultiByte(CP_UTF8)`
/// 转成 UTF-8；其它平台 `argv` 本身就是 UTF-8，直接拷贝。
///
/// 失败（仅在 Windows 取宽字符命令行失败时）返回空表并记一次 ERROR —— **不抛异常**。
[[nodiscard]] std::vector<std::string> CommandLineArgumentsUtf8(int argc, char** argv);

}  // namespace vx
