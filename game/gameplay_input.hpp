#pragma once

#include "input_context.hpp"

namespace vx {

/// 玩法输入的抑制决策（T15；**V0.9 修订**；**V0.11 / A6 改为由输入上下文派生**）。
///
/// 分类（决定"哪一类玩法输入被吞"）：
///   - `keyboardGameplay`：键盘玩法动作（移动 / 冲刺 / 跳跃 / 飞行开关与升降）；
///   - `cameraLook`：鼠标相对位移驱动的视角旋转；
///   - `mouseAction`：鼠标左键的玩法动作（T27 起 = 发射光球）。
struct InputSuppression {
    bool keyboardGameplay = false;  ///< 键盘玩法动作是否被抑制
    bool cameraLook       = false;  ///< 鼠标视角旋转是否被抑制
    bool mouseAction      = false;  ///< 鼠标玩法动作（发射）是否被抑制
};

/// 纯函数：由**输入上下文**（`CurrentContext`）派生玩法输入是否抑制。
///
/// 规则：**`Modal`（任一模态面板打开）⇒ 三类全量抑制**（模态期间玩法输入绝不泄漏到世界，即使 ImGui 本帧恰好
/// 未报告 Want —— 例如光标停在面板外）；**其余上下文 ⇒ 一律放行**。
///
/// **为什么是"上下文"而不是"一个面板布尔"（V0.11 / A6）**：抑制、模式归属、`Esc` 栈顶弹出，三者本是**同一件事**
/// （"当前是谁在接管输入"）的三个侧面。原先它们各判各的（`AnyBlockingPanelOpen()` + `placementMode`/`modifyMode`
/// + 手写 `Esc` 链）⇒ 新增模式 / 键要改多处。现在统一由 [ADR 0040](../../docs/adr/0040-input-context-stack-and-action-ownership-table.md)
/// 的输入上下文栈派生；本函数只保留"抑制"这一侧面。
///
/// **V0.9 修订（缺陷：打开 F1 后无法移动 / 转视角）**：本函数**不接受** ImGui 的
/// `io.WantCaptureMouse` / `io.WantCaptureKeyboard`。原因：ImGui 在**只读叠加层**（常驻坐标 HUD、
/// **F1 调试面板**）可见 / 被悬停 / 获得键盘焦点时**也会报告这两个标志**，若据此抑制，
/// 就会出现"为了看性能而开 F1，结果键鼠被自己的面板吃掉"（实测）。只读叠加层**不提供任何操作项**
/// （见 `docs/ui-inventory.md` §2.2），因此**不得**抑制玩法输入；它们**也不进** `InputContextState`
/// （⇒ `CurrentContext` 不会是 `Modal`）⇒ 口径一致。
///
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline InputSuppression DecideInputSuppression(InputContext context) noexcept {
    const bool modal = (context == InputContext::Modal);
    InputSuppression suppression;
    suppression.keyboardGameplay = modal;
    suppression.cameraLook       = modal;
    suppression.mouseAction      = modal;
    return suppression;
}

}  // namespace vx
