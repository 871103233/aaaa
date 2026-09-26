#pragma once

#include "input/action_state.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// 鼠标轴：用于把鼠标位移绑定到**模拟动作**。
enum class MouseAxis : std::uint8_t {
    X = 0,
    Y = 1,
};

/// 动作映射 + 每帧输入采样。
///
/// 契约（上层据此编程，测试逐条断言）：
///   1. **平台层填充，上层只读**：平台层在事件阶段调用 `SetKeyDown` / `SetMouseButtonDown` /
///      `AddMouseDelta`；`world/` 与 `game/` **绝不**触摸 SDL 事件队列（SKILL 红线）。
///   2. **每帧采样一次**：每帧在**固定步循环之外**调用一次 `BeginFrame()`。
///      `BeginFrame()` 提交本帧状态，并**清除上一帧的 pressed 边沿**——
///      因此 `pressed` 只反映"本帧新按下"，绝不跨帧残留（键一直按住也不会每帧触发）。
///   3. **同帧只消费一次**：`ConsumePressed()` 返回该动作本帧的边沿，并把该边沿清掉；
///      同一帧内第二次调用返回 `false`。这样同一帧里的多个固定逻辑步
///      **不会重复消费同一次点击**。`ConsumeValue()` 对模拟动作同理（消费后本帧余下查询得 0）。
///   4. 恒等查询：`State()` / `Held()` / `Pressed()` 是幂等的只读查询，不改变任何状态。
///
/// 前置条件：`action < ActionId::Count`。
/// 线程约定：只在拥有输入的那个线程（主线程）使用。
class InputMap {
public:
    InputMap() = default;

    InputMap(const InputMap&) = delete;
    InputMap& operator=(const InputMap&) = delete;

    /// 把一个 SDL scancode 绑定到数字动作。同一动作可绑定多个键（任一按下即成立）。
    void BindKey(ActionId action, SDL_Scancode key);

    /// 把一个鼠标轴绑定到模拟动作。
    void BindMouseAxis(ActionId action, MouseAxis axis);

    /// 把一个鼠标按键绑定到数字动作（如主 / 副笔刷）。
    /// 前置条件：`button` 为 SDL 的鼠标按键号（`SDL_BUTTON_LEFT` 等，从 1 起）。
    void BindMouseButton(ActionId action, std::uint8_t button);

    /// 平台层：记录某个按键当前的按下状态（由键盘事件驱动）。
    void SetKeyDown(SDL_Scancode key, bool isDown) noexcept;

    /// 平台层：记录某个鼠标按键当前的按下状态（由鼠标按键事件驱动）。
    /// 越界按键号被忽略（保持 `noexcept`，不做分配）。
    void SetMouseButtonDown(std::uint8_t button, bool isDown) noexcept;

    /// 平台层：累积本帧鼠标相对位移（由鼠标移动事件驱动）。
    void AddMouseDelta(float deltaX, float deltaY) noexcept;

    /// 每帧一次（固定步循环之外）：提交本帧原始输入，重算全部动作状态并清除上一帧的 pressed。
    void BeginFrame() noexcept;

    /// 只读查询：动作在本帧的状态快照。
    [[nodiscard]] ActionState State(ActionId action) const noexcept;

    /// 只读查询：本帧是否持续按下。
    [[nodiscard]] bool Held(ActionId action) const noexcept;

    /// 只读查询：本帧是否有"新按下"的边沿（不受消费影响，消费请用 `ConsumePressed`）。
    [[nodiscard]] bool Pressed(ActionId action) const noexcept;

    /// 消费本帧的 pressed 边沿：首次调用返回 `true` 并清除它，同帧再次调用返回 `false`。
    [[nodiscard]] bool ConsumePressed(ActionId action) noexcept;

    /// 消费本帧的模拟量：返回当前值并把它清零，使同帧后续调用得到 0。
    [[nodiscard]] float ConsumeValue(ActionId action) noexcept;

private:
    /// 单个动作的绑定表。
    struct Binding {
        std::vector<SDL_Scancode>  keys;
        std::vector<std::uint8_t>  mouseButtons;
        bool                       hasMouseAxis = false;
        MouseAxis                  mouseAxis    = MouseAxis::X;
    };

    [[nodiscard]] static std::size_t Index(ActionId action) noexcept {
        return static_cast<std::size_t>(action);
    }

    /// 鼠标按键状态数组长度：SDL 鼠标按键号从 1 起，0 号不用，最大到 `SDL_BUTTON_X2`。
    static constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(SDL_BUTTON_X2) + 1;

    std::array<Binding, kActionCount>     m_bindings {};
    std::array<ActionState, kActionCount> m_states {};
    std::array<bool, static_cast<std::size_t>(SDL_SCANCODE_COUNT)> m_keyDownNow {};
    std::array<bool, static_cast<std::size_t>(SDL_SCANCODE_COUNT)> m_keyDownPrev {};
    std::array<bool, kMouseButtonCount>                            m_mouseDownNow {};
    std::array<bool, kMouseButtonCount>                            m_mouseDownPrev {};

    float m_mouseDeltaX = 0.0F;  ///< 本帧已提交的水平位移
    float m_mouseDeltaY = 0.0F;  ///< 本帧已提交的垂直位移
    float m_pendingDeltaX = 0.0F;  ///< 事件阶段累积、待 `BeginFrame` 提交
    float m_pendingDeltaY = 0.0F;
};

}  // namespace vx
