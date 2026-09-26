#include "gameplay_input.hpp"

#include <gtest/gtest.h>

namespace {

using vx::DecideInputSuppression;
using vx::InputSuppression;

}  // namespace

// 面板关闭且 ImGui 未接管任何输入：玩法输入全部放行。
TEST(GameplayInput, NothingSuppressedWhenNoUiWantsInput) {
    const InputSuppression suppression = DecideInputSuppression(/*panelOpen=*/false, /*wantCaptureMouse=*/false,
                                                                /*wantCaptureKeyboard=*/false);
    EXPECT_FALSE(suppression.keyboardGameplay);
    EXPECT_FALSE(suppression.cameraLook);
    EXPECT_FALSE(suppression.mouseAction);
}

// ImGui 只接管鼠标：视角与鼠标玩法动作被抑制，键盘玩法不受影响。
TEST(GameplayInput, MouseCaptureSuppressesLookAndBrushOnly) {
    const InputSuppression suppression = DecideInputSuppression(false, /*wantCaptureMouse=*/true, false);
    EXPECT_TRUE(suppression.cameraLook) << "拖音量滑块 / 悬停面板时不得转视角";
    EXPECT_TRUE(suppression.mouseAction) << "点面板按钮不得发射光球";
    EXPECT_FALSE(suppression.keyboardGameplay) << "鼠标被接管不应连带禁用键盘移动";
}

// ImGui 只接管键盘：键盘玩法被抑制，鼠标类别不受影响。
TEST(GameplayInput, KeyboardCaptureSuppressesKeyboardOnly) {
    const InputSuppression suppression = DecideInputSuppression(false, false, /*wantCaptureKeyboard=*/true);
    EXPECT_TRUE(suppression.keyboardGameplay);
    EXPECT_FALSE(suppression.cameraLook);
    EXPECT_FALSE(suppression.mouseAction);
}

// 两者都被接管：全部抑制。
TEST(GameplayInput, BothCapturedSuppressesEverything) {
    const InputSuppression suppression = DecideInputSuppression(false, true, true);
    EXPECT_TRUE(suppression.keyboardGameplay);
    EXPECT_TRUE(suppression.cameraLook);
    EXPECT_TRUE(suppression.mouseAction);
}

// 系统面板打开：即使 ImGui 本帧未报告 Want 标志，也一律抑制（模态面板期间玩法输入绝不泄漏）。
TEST(GameplayInput, OpenPanelSuppressesEverythingRegardlessOfWantFlags) {
    const InputSuppression suppression = DecideInputSuppression(/*panelOpen=*/true, false, false);
    EXPECT_TRUE(suppression.keyboardGameplay);
    EXPECT_TRUE(suppression.cameraLook);
    EXPECT_TRUE(suppression.mouseAction);
}
