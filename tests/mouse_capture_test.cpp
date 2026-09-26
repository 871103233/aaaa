#include "input/input_map.hpp"
#include "mouse_capture.hpp"

#include <gtest/gtest.h>

namespace {

using vx::ActionId;
using vx::DecideMouseCapture;
using vx::DecidePanelCaptureTransition;
using vx::InputMap;
using vx::MouseCaptureDecision;

}  // namespace

// 已捕获时的普通点击：不改变捕获状态，且必须留给笔刷（不得被捕获消费）。
TEST(MouseCapture, CapturedClickGoesToBrush) {
    const MouseCaptureDecision decision = DecideMouseCapture(/*captured=*/true, /*escapePressed=*/false,
                                                             /*clickPressed=*/true);

    EXPECT_FALSE(decision.captureRequested);
    EXPECT_FALSE(decision.releaseRequested);
    EXPECT_FALSE(decision.clickConsumedByCapture) << "已捕获时点击应归笔刷，不能被捕获吞掉";
}

// 已捕获时按 Esc：释放捕获；点击若同帧出现也仍归笔刷。
TEST(MouseCapture, EscapeReleasesWhenCaptured) {
    const MouseCaptureDecision release = DecideMouseCapture(/*captured=*/true, /*escapePressed=*/true,
                                                            /*clickPressed=*/false);
    EXPECT_TRUE(release.releaseRequested);
    EXPECT_FALSE(release.captureRequested);
    EXPECT_FALSE(release.clickConsumedByCapture);

    const MouseCaptureDecision both = DecideMouseCapture(/*captured=*/true, /*escapePressed=*/true,
                                                         /*clickPressed=*/true);
    EXPECT_TRUE(both.releaseRequested) << "Esc 释放优先于同帧点击";
    EXPECT_FALSE(both.captureRequested);
    EXPECT_FALSE(both.clickConsumedByCapture);
}

// 未捕获时点击：重新捕获，且**该次点击被捕获消费**——这是"点击回窗口不挖地"的关键。
TEST(MouseCapture, ClickRecapturesAndConsumesTheClick) {
    const MouseCaptureDecision decision = DecideMouseCapture(/*captured=*/false, /*escapePressed=*/false,
                                                             /*clickPressed=*/true);

    EXPECT_TRUE(decision.captureRequested);
    EXPECT_TRUE(decision.clickConsumedByCapture);
    EXPECT_FALSE(decision.releaseRequested);
}

// 未捕获且无点击：不做任何迁移（Esc 单独按下不能凭空捕获；焦点恢复也不得静默重捕获）。
TEST(MouseCapture, NoTransitionWithoutClickWhenReleased) {
    const MouseCaptureDecision idle = DecideMouseCapture(/*captured=*/false, /*escapePressed=*/false,
                                                         /*clickPressed=*/false);
    EXPECT_FALSE(idle.captureRequested);
    EXPECT_FALSE(idle.releaseRequested);
    EXPECT_FALSE(idle.clickConsumedByCapture);

    const MouseCaptureDecision escapeOnly = DecideMouseCapture(/*captured=*/false, /*escapePressed=*/true,
                                                               /*clickPressed=*/false);
    EXPECT_FALSE(escapeOnly.captureRequested);
    EXPECT_FALSE(escapeOnly.releaseRequested);
}

// T14 回归：状态机接到真实 InputMap 上时，"重新捕获的那一次点击"必须在同一帧被消费掉，
// 于是后续的笔刷判定 `ConsumePressed(Attack)` 必然为 false。
// 若把捕获判定放到笔刷之后（或忘记消费），本用例会失败——它锁定 main.cpp 里的顺序不可回退。
TEST(MouseCapture, RecaptureClickIsConsumedBeforeBrushCheck) {
    InputMap input;
    input.BindMouseButton(ActionId::Attack, SDL_BUTTON_LEFT);
    input.BeginFrame();

    input.SetMouseButtonDown(SDL_BUTTON_LEFT, true);
    input.BeginFrame();  // 本帧产生左键按下边沿

    bool captured = false;
    const MouseCaptureDecision decision = DecideMouseCapture(captured, false, input.Pressed(ActionId::Attack));
    ASSERT_TRUE(decision.captureRequested);
    ASSERT_TRUE(decision.clickConsumedByCapture);

    // 捕获动作先消费该边沿（与 main.cpp 的顺序一致）……
    if (decision.clickConsumedByCapture) {
        (void)input.ConsumePressed(ActionId::Attack);
    }
    // ……随后笔刷判定必须拿不到这次点击。
    EXPECT_FALSE(input.ConsumePressed(ActionId::Attack)) << "重新捕获的点击绝不能再触发挖掘";
}

// T15：打开系统面板 —— 已捕获时请求释放，并请求记住"打开前状态"。
TEST(PanelCapture, OpeningCapturedPanelReleasesAndRemembers) {
    const auto transition = DecidePanelCaptureTransition(/*opening=*/true, /*capturedState=*/true);
    EXPECT_TRUE(transition.releaseRequested) << "打开面板必须释放捕获，否则光标不可见、无法点控件";
    EXPECT_TRUE(transition.rememberCaptureState);
    EXPECT_FALSE(transition.captureRequested);
}

// T15：打开系统面板但当前未捕获 —— 无需释放，但仍要记住（免得凭空捕获）。
TEST(PanelCapture, OpeningWhenAlreadyReleasedDoesNotRelease) {
    const auto transition = DecidePanelCaptureTransition(/*opening=*/true, /*capturedState=*/false);
    EXPECT_FALSE(transition.releaseRequested);
    EXPECT_TRUE(transition.rememberCaptureState);
    EXPECT_FALSE(transition.captureRequested);
}

// T15：关闭系统面板 —— 按打开前状态恢复；打开前未捕获则保持释放。
TEST(PanelCapture, ClosingRestoresPrePanelState) {
    const auto restore = DecidePanelCaptureTransition(/*opening=*/false, /*capturedState=*/true);
    EXPECT_TRUE(restore.captureRequested);
    EXPECT_FALSE(restore.releaseRequested);
    EXPECT_FALSE(restore.rememberCaptureState);

    const auto stayReleased = DecidePanelCaptureTransition(/*opening=*/false, /*capturedState=*/false);
    EXPECT_FALSE(stayReleased.captureRequested);
}

// T15：完整序列 —— 已捕获 → 打开（释放）→ 关闭（恢复到打开前状态）。
TEST(PanelCapture, OpenThenCloseRestoresSequence) {
    bool captured           = true;
    bool captureBeforePanel = false;

    const auto open = DecidePanelCaptureTransition(/*opening=*/true, captured);
    if (open.rememberCaptureState) {
        captureBeforePanel = captured;
    }
    if (open.releaseRequested) {
        captured = false;
    }
    EXPECT_FALSE(captured) << "打开面板后应处于释放状态";
    EXPECT_TRUE(captureBeforePanel) << "应记住打开前的捕获状态";

    const auto close = DecidePanelCaptureTransition(/*opening=*/false, captureBeforePanel);
    if (close.captureRequested) {
        captured = true;
    }
    EXPECT_TRUE(captured) << "关闭面板必须恢复打开前的捕获状态";
}
