#pragma once

#include <SDL3/SDL.h>

#include <vector>

namespace vx {

class InputMap;

/// 窗口 / 显示器的像素尺寸。
struct DisplaySize {
    int width  = 0;
    int height = 0;
};

/// SDL3 窗口 + SDL3_gpu 设备的所有者。
///
/// 线程约定：图形上下文只能在创建它的线程使用，因此本类以及所有
/// GPU 资源操作都必须在「渲染线程」上完成（见技能规范 §4.9 线程角色表）。
class Window {
public:
    /// 事件转发回调：每个 SDL 事件调用一次。`userData` 为安装时给定的上下文。
    ///
    /// 这是给上层（如 ImGui 后端）接入事件处理的通用钩子：**平台层仍是唯一读事件队列的地方**，
    /// 上层通过注册回调消费事件，而不是自己去轮询 SDL（SKILL §2）。
    using EventCallback = void (*)(void* userData, const SDL_Event& event);

    Window(const char* title, int width, int height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] SDL_Window*    handle() const noexcept { return m_window; }
    [[nodiscard]] SDL_GPUDevice* device() const noexcept { return m_device; }

    /// 安装事件转发回调（全生命周期**只安装一次**，每帧不再改动）。传 `nullptr` 可卸载。
    ///
    /// 回调在 `pump_events` 内、对**每个** SDL 事件同步调用一次，**不做任何分配**。
    /// 前置条件：`callback` 非空时必须能接受引擎运行期内的任意 SDL 事件。
    void SetEventCallback(EventCallback callback, void* userData) noexcept;

    /// 处理本帧事件并把键盘 / 鼠标输入喂给 `input`；返回 false 表示收到退出请求。
    ///
    /// 这是工程内**唯一**从 SDL 事件队列取事件的位置：上层（`world/` / `game/`）
    /// 只消费 `InputMap` 的动作，或经 `SetEventCallback` 注册的回调，不接触 SDL 事件
    /// （见 `input/input_map.hpp` 契约 1）。
    [[nodiscard]] bool pump_events(InputMap& input) noexcept;

    /// 启用 / 关闭**相对鼠标模式**（光标隐藏、鼠标位移改为相对量，不再受屏幕边界限制）。
    ///
    /// 这是上层（`game/`）控制"鼠标锁定"的**唯一**入口，上层不直接调用 SDL（SKILL §2）。
    /// 幂等：与当前状态一致的请求不会再次调用 SDL，因此可安全地每帧调用而不刷 API。
    /// 返回值：调用后窗口是否处于 `enabled` 状态（SDL 可能因窗口未聚焦等原因拒绝启用）。
    [[nodiscard]] bool SetRelativeMouseMode(bool enabled) noexcept;

    /// 当前窗口是否处于相对鼠标模式（直接向 SDL 查询，不做缓存，故失焦自动释放也能被上层观察到）。
    [[nodiscard]] bool IsRelativeMouseMode() const noexcept;

    /// 切换窗口 / 全屏（T15）。
    ///
    /// 采用业界默认的**桌面无边框全屏**（borderless，等价于 `SDL_SetWindowFullscreenMode(window, NULL)`），
    /// 不做分辨率切换与显示模式枚举选择，避免切换黑屏与模式匹配失败。
    /// 幂等：目标状态与当前一致时不调用 SDL。返回值：调用后是否处于 `fullscreen` 状态。
    [[nodiscard]] bool SetFullscreen(bool fullscreen) noexcept;

    /// 当前窗口是否处于全屏（直接向 SDL 查询窗口标志，不做缓存）。
    [[nodiscard]] bool IsFullscreen() const noexcept;

    /// 设置窗口**客户区**尺寸（窗口模式下由分辨率控件调用）。返回是否成功。
    [[nodiscard]] bool SetWindowSize(int width, int height) noexcept;

    /// 当前窗口客户区尺寸。
    [[nodiscard]] DisplaySize WindowSize() const noexcept;

    /// 当前窗口所在显示器支持的**全屏分辨率档位**（按尺寸去重，并按宽 / 高升序排列）。
    ///
    /// 取自 `SDL_GetFullscreenDisplayModes`，供系统面板的分辨率列表使用（T15）。
    [[nodiscard]] std::vector<DisplaySize> SupportedResolutions() const;

    /// 查询**当前窗口所在显示器**的刷新率（Hz，T17）。
    ///
    /// 实现：`SDL_GetDisplayForWindow` → `SDL_GetDesktopDisplayMode` → `SDL_DisplayMode::refresh_rate`。
    /// **回退**：窗口未创建 / 取不到显示器 / 桌面模式为空 / 刷新率非正时，返回
    /// [`kFallbackRefreshRate`](settings.hpp)（当前 60 Hz）。因此返回值恒 `> 0`，
    /// 上层可直接用作帧率上限的上界，无需再判空。
    [[nodiscard]] int DisplayRefreshRate() const noexcept;

    /// 切换交换链**呈现模式**（T17）：`enabled = true` 用垂直同步（VSYNC）；
    /// `false` 优先用 MAILBOX，不支持则 IMMEDIATE，再不行退回 VSYNC。
    ///
    /// 这是设置呈现模式的**唯一**代码路径（构造期也经它初始化），因此可在运行期安全切换。
    /// 幂等：目标模式与当前一致时不调用 SDL。返回调用后是否处于目标模式。
    [[nodiscard]] bool SetVSync(bool enabled) noexcept;

    /// 当前呈现模式名称（`"vsync"` / `"mailbox"` / `"immediate"`），用于日志与观测（T17）。
    [[nodiscard]] const char* PresentModeName() const noexcept;

private:
    SDL_Window*         m_window        = nullptr;
    SDL_GPUDevice*      m_device        = nullptr;
    EventCallback       m_eventCallback = nullptr;
    void*               m_eventUserData = nullptr;
    SDL_GPUPresentMode  m_presentMode   = SDL_GPU_PRESENTMODE_VSYNC;  ///< 当前交换链呈现模式（T17）
};

}  // namespace vx
