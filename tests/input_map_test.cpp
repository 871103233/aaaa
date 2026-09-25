#include "input/input_map.hpp"

#include <gtest/gtest.h>

namespace {

using vx::ActionId;
using vx::InputMap;
using vx::MouseAxis;

}  // namespace

// 未绑定的动作恒为未激活，消费也不会误报 true。
TEST(InputMap, UnboundActionIsAlwaysInactive) {
    InputMap input;

    EXPECT_FALSE(input.Held(ActionId::Jump));
    EXPECT_FALSE(input.Pressed(ActionId::Jump));
    EXPECT_FALSE(input.ConsumePressed(ActionId::Jump));
    EXPECT_FLOAT_EQ(input.State(ActionId::Jump).value, 0.0F);
}

// 持续按下：键按下后跨帧保持，抬起后消失。
TEST(InputMap, HeldTracksKeyAcrossFrames) {
    InputMap input;
    input.BindKey(ActionId::Jump, SDL_SCANCODE_SPACE);

    EXPECT_FALSE(input.Held(ActionId::Jump));

    input.SetKeyDown(SDL_SCANCODE_SPACE, true);
    input.BeginFrame();
    EXPECT_TRUE(input.Held(ActionId::Jump));

    input.BeginFrame();  // 键状态未变化
    EXPECT_TRUE(input.Held(ActionId::Jump));

    input.SetKeyDown(SDL_SCANCODE_SPACE, false);
    input.BeginFrame();
    EXPECT_FALSE(input.Held(ActionId::Jump));
}

// 契约 2：pressed 是"本帧新按下"的边沿；BeginFrame 会清除上一帧的 pressed。
TEST(InputMap, PressedIsEdgeAndClearedByNextBeginFrame) {
    InputMap input;
    input.BindKey(ActionId::Attack, SDL_SCANCODE_J);

    input.SetKeyDown(SDL_SCANCODE_J, true);
    input.BeginFrame();
    EXPECT_TRUE(input.Pressed(ActionId::Attack));  // 本帧新按下

    input.BeginFrame();
    EXPECT_FALSE(input.Pressed(ActionId::Attack)) << "上一帧的 pressed 必须已被清除";
    EXPECT_TRUE(input.Held(ActionId::Attack)) << "键仍按住时 held 应为真";

    // 抬起 → 再按下：产生新的边沿
    input.SetKeyDown(SDL_SCANCODE_J, false);
    input.BeginFrame();
    EXPECT_FALSE(input.Pressed(ActionId::Attack));

    input.SetKeyDown(SDL_SCANCODE_J, true);
    input.BeginFrame();
    EXPECT_TRUE(input.Pressed(ActionId::Attack));
}

// 契约 3：同一帧内 pressed 只被消费一次——多个逻辑步不会重复消费同一次点击。
TEST(InputMap, PressedIsConsumedExactlyOncePerFrame) {
    InputMap input;
    input.BindKey(ActionId::Attack, SDL_SCANCODE_J);

    input.SetKeyDown(SDL_SCANCODE_J, true);
    input.BeginFrame();

    int consumed = 0;
    for (int step = 0; step < 5; ++step) {  // 模拟同一帧内的多个固定逻辑步
        if (input.ConsumePressed(ActionId::Attack)) {
            ++consumed;
        }
    }
    EXPECT_EQ(consumed, 1);
    EXPECT_FALSE(input.Pressed(ActionId::Attack)) << "消费后同帧再查必须为 false";

    // 下一帧没有新的边沿，消费仍为 false
    input.BeginFrame();
    EXPECT_FALSE(input.ConsumePressed(ActionId::Attack));
}

// 契约 3（模拟动作）：鼠标位移只在本帧被消费一次，且不会跨帧重复。
TEST(InputMap, MouseDeltaIsSubmittedOnceAndConsumedOnce) {
    InputMap input;
    input.BindMouseAxis(ActionId::LookX, MouseAxis::X);

    input.AddMouseDelta(3.5F, -1.5F);
    input.BeginFrame();
    EXPECT_FLOAT_EQ(input.State(ActionId::LookX).value, 3.5F);

    EXPECT_FLOAT_EQ(input.ConsumeValue(ActionId::LookX), 3.5F);
    EXPECT_FLOAT_EQ(input.State(ActionId::LookX).value, 0.0F) << "同一帧第二次读必须为 0";

    // 下一帧没有新的位移事件 → 归零，而不是重复上一帧的位移
    input.BeginFrame();
    EXPECT_FLOAT_EQ(input.State(ActionId::LookX).value, 0.0F);
}

// 同一动作可绑定多个键：任一按下即成立，且另一键新按下仍算本帧边沿。
TEST(InputMap, MultipleKeysShareOneAction) {
    InputMap input;
    input.BindKey(ActionId::Sprint, SDL_SCANCODE_LSHIFT);
    input.BindKey(ActionId::Sprint, SDL_SCANCODE_RSHIFT);

    input.SetKeyDown(SDL_SCANCODE_LSHIFT, true);
    input.BeginFrame();
    EXPECT_TRUE(input.Held(ActionId::Sprint));

    input.SetKeyDown(SDL_SCANCODE_RSHIFT, true);
    input.BeginFrame();
    EXPECT_TRUE(input.Held(ActionId::Sprint));
    EXPECT_TRUE(input.Pressed(ActionId::Sprint)) << "另一个键新按下仍是本帧边沿";

    input.SetKeyDown(SDL_SCANCODE_LSHIFT, false);
    input.BeginFrame();
    EXPECT_TRUE(input.Held(ActionId::Sprint)) << "右 Shift 仍按住";
}
