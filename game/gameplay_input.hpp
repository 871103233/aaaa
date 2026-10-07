#pragma once

namespace vx {

/// 玩法输入的抑制决策（T15；**V0.9 修订**）。
///
/// 分类（决定"哪一类玩法输入被吞"）：
///   - `keyboardGameplay`：键盘玩法动作（移动 / 冲刺 / 跳跃 / 飞行开关与升降）；
///   - `cameraLook`：鼠标相对位移驱动的视角旋转；
///   - `mouseAction`：鼠标左右键的玩法动作（T27 起 = 发射光球）。
struct InputSuppression {
    bool keyboardGameplay = false;  ///< 键盘玩法动作是否被抑制
    bool cameraLook       = false;  ///< 鼠标视角旋转是否被抑制
    bool mouseAction      = false;  ///< 鼠标玩法动作（发射）是否被抑制
};

/// 纯函数：由「**模态面板**（系统面板 / 传送门菜单 / 物件选择器）是否打开」决定玩法输入是否抑制。
///
/// 规则：**模态面板打开 ⇒ 三类全量抑制**（模态期间玩法输入绝不泄漏到世界，即使 ImGui 本帧恰好
/// 未报告 Want —— 例如光标停在面板外）；**面板关闭 ⇒ 一律放行**。
///
/// **V0.9 修订（缺陷：打开 F1 后无法移动 / 转视角）**：本函数**不再接受** ImGui 的
/// `io.WantCaptureMouse` / `io.WantCaptureKeyboard`。原因：ImGui 在**只读叠加层**（常驻坐标 HUD、
/// **F1 调试面板**）可见 / 被悬停 / 获得键盘焦点时**也会报告这两个标志**，若据此抑制，
/// 就会出现"为了看性能而开 F1，结果键鼠被自己的面板吃掉"（实测）。只读叠加层**不提供任何操作项**
/// （见 `docs/ui-inventory.md` §2.2），因此**不得**抑制玩法输入。
/// 三处模态面板已由 `DebugOverlay::AnyBlockingPanelOpen()` 覆盖 ⇒ Want 标志对它没有增量信息。
///
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline InputSuppression DecideInputSuppression(bool blockingPanelOpen) noexcept {
    InputSuppression suppression;
    suppression.keyboardGameplay = blockingPanelOpen;
    suppression.cameraLook       = blockingPanelOpen;
    suppression.mouseAction      = blockingPanelOpen;
    return suppression;
}

}  // namespace vx
