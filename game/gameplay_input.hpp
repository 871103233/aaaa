#pragma once

namespace vx {

/// 玩法输入的抑制决策（T15）。
///
/// 当系统面板打开、或任何 ImGui 控件正在接管鼠标 / 键盘（`io.WantCaptureMouse` /
/// `io.WantCaptureKeyboard`）时，对应类别的玩法输入必须被吞掉，否则"点按钮"会顺手挖地、
/// "拖音量滑块"会带着相机转。分类与来源：
///   - `keyboardGameplay`：键盘玩法动作（移动 / 冲刺 / 跳跃 / 飞行开关与升降）—— 由 `WantCaptureKeyboard` 决定；
///   - `cameraLook`：鼠标相对位移驱动的视角旋转 —— 由 `WantCaptureMouse` 决定；
///   - `mouseAction`：鼠标左右键的玩法动作（T27 起 = 发射光球）—— 由 `WantCaptureMouse` 决定。
struct InputSuppression {
    bool keyboardGameplay = false;  ///< 键盘玩法动作是否被抑制
    bool cameraLook       = false;  ///< 鼠标视角旋转是否被抑制
    bool mouseAction      = false;  ///< 鼠标玩法动作（发射）是否被抑制
};

/// 纯函数：由「系统面板是否打开」「ImGui 是否想接管鼠标 / 键盘」决定各类玩法输入是否抑制。
///
/// 规则：**面板打开一律抑制全部玩法输入**（模态面板期间玩法输入绝不泄漏到世界），即使 ImGui 本帧
/// 恰好未报告 Want 标志（例如光标停在面板外）；面板关闭时按 ImGui 的 Want 标志分类抑制。
///
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline InputSuppression DecideInputSuppression(bool panelOpen, bool wantCaptureMouse,
                                                             bool wantCaptureKeyboard) noexcept {
    const bool blockMouse    = panelOpen || wantCaptureMouse;
    const bool blockKeyboard = panelOpen || wantCaptureKeyboard;

    InputSuppression suppression;
    suppression.keyboardGameplay = blockKeyboard;
    suppression.cameraLook       = blockMouse;
    suppression.mouseAction      = blockMouse;
    return suppression;
}

}  // namespace vx
