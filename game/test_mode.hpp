#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vx {

/// 测试模式（T85）：运行时由**启动参数**决定，驱动 F1 面板顶部的横幅（只读展示）。
enum class TestMode {
    Manual,  ///< 人工测试（默认）：面板列出**本次需人工确认的项**
    Auto,    ///< 自动测试（脚本以 `--auto-test` 启动）：提示**请勿操作键盘 / 鼠标**
};

/// 测试模式信息：模式 + 人工验收项（UTF-8，可能为空）。全运行期不变。
struct TestModeInfo {
    TestMode                 mode = TestMode::Manual;
    std::vector<std::string> manualItems;
};

/// **纯函数**：参数是否为"开关"（以 `--` 开头）。
///
/// 供上层区分"启动开关"与"位置参数"（例：可选的着色器目录）——开关绝不能当成路径。
[[nodiscard]] inline bool IsOptionArgument(const std::string& argument) noexcept {
    return argument.rfind("--", 0) == 0;
}

/// **纯函数**：从启动参数（UTF-8，含 `argv[0]`）解析测试模式（确定性、无副作用 ⇒ 可直接单测）。
///
/// 规则：
///   - 任一参数为 `--auto-test` ⇒ [`Auto`](#TestMode)（**优先于**人工项）；
///   - `--manual-test=a;b;c` ⇒ [`Manual`](#TestMode)，按 `;` 切分并**丢弃空项**；可多次给出、累加；
///   - 其它参数一律忽略（未知 `--xxx` 与位置参数都不影响结果）。
[[nodiscard]] inline TestModeInfo ParseTestModeFromArguments(const std::vector<std::string>& arguments) {
    TestModeInfo info;
    constexpr std::string_view kManualPrefix = "--manual-test=";

    for (const std::string& argument : arguments) {
        if (argument == "--auto-test") {
            info.mode = TestMode::Auto;
            continue;
        }
        if (argument.rfind(kManualPrefix, 0) == 0) {
            const std::string rest  = argument.substr(kManualPrefix.size());
            std::size_t       begin = 0;
            while (begin <= rest.size()) {
                const std::size_t end  = rest.find(';', begin);
                const std::size_t stop = (end == std::string::npos) ? rest.size() : end;
                if (stop > begin) {
                    info.manualItems.push_back(rest.substr(begin, stop - begin));
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
        }
    }
    return info;
}

}  // namespace vx
