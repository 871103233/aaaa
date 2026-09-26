#include "input/input_map.hpp"

namespace vx {

void InputMap::BindKey(ActionId action, SDL_Scancode key) {
    m_bindings[Index(action)].keys.push_back(key);
}

void InputMap::BindMouseAxis(ActionId action, MouseAxis axis) {
    Binding& binding     = m_bindings[Index(action)];
    binding.hasMouseAxis = true;
    binding.mouseAxis    = axis;
}

void InputMap::BindMouseButton(ActionId action, std::uint8_t button) {
    m_bindings[Index(action)].mouseButtons.push_back(button);
}

void InputMap::SetKeyDown(SDL_Scancode key, bool isDown) noexcept {
    m_keyDownNow[static_cast<std::size_t>(key)] = isDown;
}

void InputMap::SetMouseButtonDown(std::uint8_t button, bool isDown) noexcept {
    if (static_cast<std::size_t>(button) >= kMouseButtonCount) {
        return;  // 0 号与越界按键号：忽略，避免越界写
    }
    m_mouseDownNow[static_cast<std::size_t>(button)] = isDown;
}

void InputMap::AddMouseDelta(float deltaX, float deltaY) noexcept {
    m_pendingDeltaX += deltaX;
    m_pendingDeltaY += deltaY;
}

void InputMap::BeginFrame() noexcept {
    // 提交事件阶段累积的鼠标位移，并清空累积器：下一帧不会再重复上一帧的位移。
    m_mouseDeltaX   = m_pendingDeltaX;
    m_mouseDeltaY   = m_pendingDeltaY;
    m_pendingDeltaX = 0.0F;
    m_pendingDeltaY = 0.0F;

    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Binding& binding = m_bindings[i];
        ActionState    state {};

        for (const SDL_Scancode key : binding.keys) {
            const std::size_t index = static_cast<std::size_t>(key);
            if (!m_keyDownNow[index]) {
                continue;
            }
            state.held = true;
            if (!m_keyDownPrev[index]) {
                state.pressed = true;  // 抬起 → 按下 的边沿，每帧最多一次
            }
        }

        for (const std::uint8_t button : binding.mouseButtons) {
            const std::size_t index = static_cast<std::size_t>(button);
            if (index >= kMouseButtonCount || !m_mouseDownNow[index]) {
                continue;
            }
            state.held = true;
            if (!m_mouseDownPrev[index]) {
                state.pressed = true;  // 鼠标按键同样按"抬起 → 按下"产生边沿
            }
        }

        if (binding.hasMouseAxis) {
            state.value = (binding.mouseAxis == MouseAxis::X) ? m_mouseDeltaX : m_mouseDeltaY;
        } else if (state.held) {
            state.value = 1.0F;
        }

        m_states[i] = state;
    }

    // 本帧快照成为下一帧的"上一帧"：pressed 边沿因此不会跨帧残留。
    m_keyDownPrev   = m_keyDownNow;
    m_mouseDownPrev = m_mouseDownNow;
}

ActionState InputMap::State(ActionId action) const noexcept {
    return m_states[Index(action)];
}

bool InputMap::Held(ActionId action) const noexcept {
    return m_states[Index(action)].held;
}

bool InputMap::Pressed(ActionId action) const noexcept {
    return m_states[Index(action)].pressed;
}

bool InputMap::ConsumePressed(ActionId action) noexcept {
    ActionState& state   = m_states[Index(action)];
    const bool   pressed = state.pressed;
    state.pressed        = false;  // 同帧内第二次消费必然为 false
    return pressed;
}

float InputMap::ConsumeValue(ActionId action) noexcept {
    ActionState& state = m_states[Index(action)];
    const float  value = state.value;
    state.value        = 0.0F;
    return value;
}

}  // namespace vx
