#pragma once

#include "platform/settings.hpp"
#include "platform/window.hpp"
#include "ui_text.hpp"

#include <vector>

namespace vx {

/// 系统面板一帧的输入 / 输出上下文（T15）。
///
/// `settings` 为面板**直接编辑**的当前值（就地改写）；其余标志描述"用户本帧做了什么"，
/// 由 `main` 据此调用平台层与设置落盘——面板本身不碰 SDL、不写文件。
struct SystemPanelContext {
    SystemSettings            settings;                ///< 当前设置（面板就地编辑）
    const std::vector<DisplaySize>* resolutions = nullptr;  ///< 可选分辨率档位（由平台层提供，须含当前尺寸）
    int  displayRefreshRate  = kFallbackRefreshRate;  ///< 当前显示器刷新率（Hz，帧率上限上界，T17）
    bool displayModeChanged = false;  ///< 用户本帧改了显示模式（立即作用于窗口）
    bool resolutionChanged  = false;  ///< 用户本帧选了新分辨率（仅窗口模式；立即作用于窗口）
    bool volumeCommitted    = false;  ///< 用户本帧**结束**了一次音量调整（应用到音频增益并落盘）
    bool frameRateCapChanged = false; ///< 用户本帧改了帧率上限（立即应用到限帧 / 垂直同步路径）
    bool requestQuit        = false;  ///< 点击了"退出游戏"
};

/// ESC 系统面板（T15，[`docs/ui-inventory.md` §2.1]）。
///
/// 位置：屏幕中央的模态式 ImGui 窗口（打开期间抑制全部玩法输入，见 `gameplay_input.hpp`）。
/// 打开方式：`Esc`；关闭方式：再次 `Esc`。开关对鼠标捕获的影响由 `main` 通过
/// `DecidePanelCaptureTransition` 执行（打开释放、关闭恢复打开前状态）。
///
/// 只负责 UI 与"用户改了什么"：**不**直接调用 SDL（显示模式 / 分辨率由 `engine/platform/` 落实），
/// 也**不**持有任何音频系统。线程约定：只在渲染线程使用。
class SystemPanel final {
public:
    void Open() noexcept { m_open = true; }
    void Close() noexcept { m_open = false; }
    void Toggle() noexcept { m_open = !m_open; }
    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }

    /// 构建面板内容（仅在打开时）。前置条件：已在可见的 ImGui 帧内调用（`ImGui::NewFrame` 之后）。
    ///
    /// `cjkLabels` 来自 [`ApplyUiFont`](ui_font.hpp) 的返回值：true 用中文标签，false 用**纯 ASCII**
    /// 英文标签（保证无 CJK 字体的机器上也不出现缺字 `?`）。标签一律经 [`UiText`](ui_text.hpp) 取得。
    void Build(SystemPanelContext& context, bool cjkLabels);

private:
    bool m_open = false;
};

}  // namespace vx
