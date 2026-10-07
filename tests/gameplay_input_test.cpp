#include "gameplay_input.hpp"

#include "input_context.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {

using vx::DecideInputSuppression;
using vx::InputContext;
using vx::InputSuppression;

/// 非模态上下文（自由活动 / 建造 / 修改；V0.11 / A6 起抑制由上下文派生）。
const std::array<InputContext, 4> kNonModal {
    InputContext::FreeRoam, InputContext::Build, InputContext::Modify, InputContext::ModifyDrag,
};

}  // namespace

// 无**模态面板**打开（任何非模态上下文）：玩法输入全部放行。
TEST(GameplayInput, NothingSuppressedInEveryNonModalContext) {
    for (const InputContext context : kNonModal) {
        const InputSuppression suppression = DecideInputSuppression(context);
        EXPECT_FALSE(suppression.keyboardGameplay);
        EXPECT_FALSE(suppression.cameraLook);
        EXPECT_FALSE(suppression.mouseAction);
    }
}

// **V0.9 回归**（缺陷：打开 F1 后无法移动 / 转视角）：F1 调试面板与常驻 HUD 都是**只读叠加层**，
// 都不进 `InputContextState` ⇒ `CurrentContext` 不是 `Modal` ⇒ 玩法输入**一律放行**。
// 旧实现把 ImGui 的 `WantCaptureMouse|Keyboard` 也计入抑制，而 ImGui 在这两个只读叠加层可见 / 被悬停 /
// 获得键盘焦点时会报告它们 ⇒ 键盘玩法被吞。本用例把"只读叠加层不产生抑制"这一契约钉住。
TEST(GameplayInput, ReadOnlyOverlaysNeverSuppressGameplayInput) {
    const InputSuppression suppression = DecideInputSuppression(InputContext::FreeRoam);
    EXPECT_FALSE(suppression.keyboardGameplay) << "开着 F1 / HUD 时仍必须能移动 / 跳跃 / 飞行";
    EXPECT_FALSE(suppression.cameraLook) << "开着 F1 / HUD 时仍必须能转视角";
    EXPECT_FALSE(suppression.mouseAction) << "开着 F1 / HUD 时仍必须能发射";
}

// 模态面板（系统面板 / 传送门菜单 / 物件选择器）：三类全量抑制（玩法输入绝不泄漏到世界）。
// **V0.11 / A6**：抑制由输入上下文派生 —— 三个模态面板当前**都**折叠为 `InputContext::Modal`。
TEST(GameplayInput, ModalContextSuppressesEverything) {
    const InputSuppression suppression = DecideInputSuppression(InputContext::Modal);
    EXPECT_TRUE(suppression.keyboardGameplay);
    EXPECT_TRUE(suppression.cameraLook) << "点面板按钮不得发射光球 / 不得转视角";
    EXPECT_TRUE(suppression.mouseAction);
}
