#pragma once

namespace vx {

/// 鼠标捕获状态机在一帧内的决策结果（T14）。
///
/// 只描述"这一帧该做什么"，不含任何平台调用或输入状态，故可被单元测试完全覆盖。
struct MouseCaptureDecision {
    bool captureRequested       = false;  ///< 本帧应**进入**捕获（重新捕获）：仅在"未捕获且有点击"时为 true
    bool releaseRequested       = false;  ///< 本帧应**退出**捕获（释放光标）：仅在"已捕获且有 Esc"时为 true
    bool clickConsumedByCapture = false;  ///< 本帧的鼠标点击已被捕获动作消费，因此**不得**再触发挥 / 堆
};

/// 纯函数：由「当前是否已捕获」「本帧 Esc 边沿」「本帧鼠标点击边沿」决定本帧的捕获状态迁移与点击归属。
///
/// 语义（逐条由 `tests/mouse_capture_test.cpp` 钉死，任何改动都必须先改测试）：
///   1. **已捕获**：出现 `Esc` 边沿即释放；此时点击仍归普通笔刷（`clickConsumedByCapture = false`）。
///   2. **未捕获**：出现鼠标点击边沿即**重新捕获**，并**消费掉这次点击**——
///      否则"点击回到窗口"会顺手挖 / 堆一块地形，这是本任务必须防住的回归。
///   3. 其余情况不做任何迁移。
///
/// 优先级：`Esc` 释放优先于同一帧的点击（已捕获时不再因点击改变状态）。
///
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline MouseCaptureDecision DecideMouseCapture(bool captured, bool escapePressed,
                                                             bool clickPressed) noexcept {
    MouseCaptureDecision decision;
    if (captured) {
        decision.releaseRequested = escapePressed;
        return decision;
    }
    if (clickPressed) {
        decision.captureRequested       = true;
        decision.clickConsumedByCapture = true;
    }
    return decision;
}

/// 系统面板（T15）开关对鼠标捕获的迁移结果。
///
/// 与 `DecideMouseCapture` 同属**一套**捕获状态机：`Esc` 的语义已由 T15 统一为"开关系统面板"，
/// 故 `main` 只把 `DecideMouseCapture` 用于"未捕获时点击重新捕获"，`escapePressed` 恒传 `false`；
/// 面板开 / 关对捕获的影响改由本函数描述，**不引入第二套捕获机制**。
struct PanelCaptureTransition {
    bool releaseRequested     = false;  ///< 需要调用平台层释放捕获（打开面板：交还光标以操作控件）
    bool captureRequested     = false;  ///< 需要调用平台层恢复捕获（关闭面板：若打开前处于捕获）
    bool rememberCaptureState = false;  ///< 需要把"打开前的捕获状态"存入记忆（供关闭时恢复）
};

/// 纯函数：由「面板切换方向」「当前是否捕获」「打开前记录的捕获状态」决定本帧的捕获动作。
///
/// 语义（由 `tests/mouse_capture_test.cpp` 钉死）：
///   - **打开面板**（`opening = true`）：若当前已捕获则请求释放；并请求记住当前捕获状态。
///     `capturedState` 传**当前**捕获状态。
///   - **关闭面板**（`opening = false`）：按打开前记录的状态请求恢复；`capturedState` 传**记忆值**。
[[nodiscard]] inline PanelCaptureTransition DecidePanelCaptureTransition(bool opening, bool capturedState) noexcept {
    PanelCaptureTransition transition;
    if (opening) {
        transition.releaseRequested     = capturedState;
        transition.rememberCaptureState = true;
    } else {
        transition.captureRequested = capturedState;
    }
    return transition;
}

}  // namespace vx
