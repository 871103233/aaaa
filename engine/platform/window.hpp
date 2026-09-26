#pragma once

#include <SDL3/SDL.h>

namespace vx {

class InputMap;

/// SDL3 窗口 + SDL3_gpu 设备的所有者。
///
/// 线程约定：图形上下文只能在创建它的线程使用，因此本类以及所有
/// GPU 资源操作都必须在「渲染线程」上完成（见技能规范 §4.9 线程角色表）。
class Window {
public:
    Window(const char* title, int width, int height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] SDL_Window*    handle() const noexcept { return m_window; }
    [[nodiscard]] SDL_GPUDevice* device() const noexcept { return m_device; }

    /// 处理本帧事件并把键盘 / 鼠标输入喂给 `input`；返回 false 表示收到退出请求。
    ///
    /// 这是工程内**唯一**从 SDL 事件队列取事件的位置：上层（`world/` / `game/`）
    /// 只消费 `InputMap` 的动作，不接触 SDL 事件（见 `input/input_map.hpp` 契约 1）。
    [[nodiscard]] bool pump_events(InputMap& input) noexcept;

private:
    SDL_Window*    m_window = nullptr;
    SDL_GPUDevice* m_device = nullptr;
};

}  // namespace vx
