#include "platform/window.hpp"

#include "input/input_map.hpp"
#include "platform/settings.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace vx {
namespace {

/// 声明本机希望接受的 Shader 字节码格式。
/// SDL3_gpu 会据此挑选后端：Windows 上优先 Vulkan（SPIR-V），无可用 Vulkan 时落到 D3D12（DXIL）。
/// 构建期两种格式都会产出，故这里把两者一并声明（见 ADR 0002）。
[[nodiscard]] SDL_GPUShaderFormat pick_shader_format() noexcept {
#if defined(SDL_PLATFORM_WINDOWS)
    return SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL;
#elif defined(SDL_PLATFORM_APPLE)
    return SDL_GPU_SHADERFORMAT_METALLIB;
#else
    return SDL_GPU_SHADERFORMAT_SPIRV;
#endif
}

/// 选择交换链呈现模式（T17）：**唯一**的呈现模式决策点。
///   - `vsync = true` → `VSYNC`（换页等待，天然把帧率限在刷新率上）；
///   - `vsync = false` → 优先 `MAILBOX`（低排队延迟、不限帧，由睡眠限帧器接管），
///     不支持则 `IMMEDIATE`（立即呈现），再不行退回 `VSYNC`（保证总有可用模式）。
[[nodiscard]] SDL_GPUPresentMode pick_present_mode(SDL_GPUDevice* device, SDL_Window* window,
                                                   bool vsync) noexcept {
    if (vsync) {
        return SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string(what) + " 失败：" + SDL_GetError());
}

}  // namespace

Window::Window(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fail("SDL_Init");
    }

    m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
    if (m_window == nullptr) {
        fail("SDL_CreateWindow");
    }

    // debug_mode = true：让 SDL3_gpu 输出校验层信息，PoC 阶段务必打开
    m_device = SDL_CreateGPUDevice(pick_shader_format(), /*debug_mode=*/true, nullptr);
    if (m_device == nullptr) {
        fail("SDL_CreateGPUDevice");
    }

    if (!SDL_ClaimWindowForGPUDevice(m_device, m_window)) {
        fail("SDL_ClaimWindowForGPUDevice");
    }

    // 交换链呈现模式（T17）：默认垂直同步。上层会按"帧率上限"设置调用 `SetVSync`；
    // 此前固定 MAILBOX 会让帧率无限飙升（实测 2000+ FPS），正是 T17 要修的现象。
    m_presentMode = pick_present_mode(m_device, m_window, /*vsync=*/true);
    (void)SDL_SetGPUSwapchainParameters(m_device, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m_presentMode);
}

Window::~Window() {
    if (m_device != nullptr) {
        if (m_window != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
        }
        SDL_DestroyGPUDevice(m_device);
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

void Window::SetEventCallback(EventCallback callback, void* userData) noexcept {
    m_eventCallback = callback;
    m_eventUserData = userData;
}

bool Window::pump_events(InputMap& input) noexcept {
    SDL_Event event {};
    while (SDL_PollEvent(&event)) {
        // 先转发给已安装的事件回调（如 ImGui 后端），再翻译进 InputMap：
        // 两者互不干扰，平台层依然是唯一读事件队列的地方。
        if (m_eventCallback != nullptr) {
            m_eventCallback(m_eventUserData, event);
        }
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // 失焦（如 Alt+Tab）时自动释放相对鼠标模式：把光标交还系统，用户才不会"被锁在窗口里"。
                // 焦点恢复**不**自动重新捕获（见下面的 FOCUS_GAINED 分支）：隐藏的光标若自己回来会让人困惑，
                // 重新捕获必须由上层显式请求（本工程为"点击窗口"）。
                (void)SetRelativeMouseMode(false);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                // 有意留空：重新捕获由 `game/` 的显式点击驱动，平台层不越权自行锁定鼠标。
                break;
            case SDL_EVENT_KEY_DOWN:
                input.SetKeyDown(event.key.scancode, true);
                break;
            case SDL_EVENT_KEY_UP:
                input.SetKeyDown(event.key.scancode, false);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                input.AddMouseDelta(event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                input.SetMouseButtonDown(event.button.button, true);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                input.SetMouseButtonDown(event.button.button, false);
                break;
            default:
                break;
        }
    }
    return true;
}

bool Window::SetRelativeMouseMode(bool enabled) noexcept {
    if (m_window == nullptr) {
        return false;
    }
    // 幂等：状态已一致就不调用 SDL，避免每帧反复设置（挂起/恢复光标是有代价的）。
    if (SDL_GetWindowRelativeMouseMode(m_window) == enabled) {
        return enabled;
    }
    return SDL_SetWindowRelativeMouseMode(m_window, enabled);
}

bool Window::IsRelativeMouseMode() const noexcept {
    if (m_window == nullptr) {
        return false;
    }
    return SDL_GetWindowRelativeMouseMode(m_window);
}

bool Window::SetFullscreen(bool fullscreen) noexcept {
    if (m_window == nullptr) {
        return false;
    }
    if (IsFullscreen() == fullscreen) {
        return fullscreen;  // 幂等：状态一致就不调用 SDL（避免无谓的模式切换）
    }
    if (fullscreen) {
        // 显式置空全屏模式 → SDL 使用**桌面分辨率**铺满，即无边框全屏（borderless）。
        (void)SDL_SetWindowFullscreenMode(m_window, nullptr);
    }
    return SDL_SetWindowFullscreen(m_window, fullscreen);
}

bool Window::IsFullscreen() const noexcept {
    return m_window != nullptr && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool Window::SetWindowSize(int width, int height) noexcept {
    if (m_window == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    return SDL_SetWindowSize(m_window, width, height);
}

DisplaySize Window::WindowSize() const noexcept {
    DisplaySize size;
    if (m_window != nullptr) {
        (void)SDL_GetWindowSize(m_window, &size.width, &size.height);
    }
    return size;
}

std::vector<DisplaySize> Window::SupportedResolutions() const {
    std::vector<DisplaySize> sizes;
    if (m_window == nullptr) {
        return sizes;
    }

    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    int                count   = 0;
    SDL_DisplayMode**  modes   = SDL_GetFullscreenDisplayModes(display, &count);
    if (modes == nullptr) {
        return sizes;
    }

    for (int i = 0; i < count; ++i) {
        const SDL_DisplayMode* mode = modes[i];
        if (mode == nullptr) {
            continue;
        }
        const DisplaySize size { mode->w, mode->h };
        const bool        duplicate = std::any_of(sizes.begin(), sizes.end(), [&size](const DisplaySize& existing) {
            return existing.width == size.width && existing.height == size.height;
        });
        if (!duplicate) {
            sizes.push_back(size);
        }
    }
    SDL_free(modes);

    std::sort(sizes.begin(), sizes.end(), [](const DisplaySize& left, const DisplaySize& right) {
        if (left.width != right.width) {
            return left.width < right.width;
        }
        return left.height < right.height;
    });
    return sizes;
}

int Window::DisplayRefreshRate() const noexcept {
    if (m_window == nullptr) {
        return kFallbackRefreshRate;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    if (display == 0) {
        return kFallbackRefreshRate;  // 取不到显示器（如窗口尚未映射）
    }
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (desktop == nullptr || desktop->refresh_rate <= 0.0F) {
        return kFallbackRefreshRate;  // 刷新率未知 / 为 0 → 文档化回退
    }
    return static_cast<int>(desktop->refresh_rate + 0.5F);  // 四舍五入到整数 Hz
}

bool Window::SetVSync(bool enabled) noexcept {
    if (m_device == nullptr || m_window == nullptr) {
        return false;
    }
    const SDL_GPUPresentMode desired = pick_present_mode(m_device, m_window, enabled);
    if (desired == m_presentMode) {
        return true;  // 幂等：模式一致就不调用 SDL
    }
    if (!SDL_SetGPUSwapchainParameters(m_device, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, desired)) {
        return false;
    }
    m_presentMode = desired;
    return true;
}

const char* Window::PresentModeName() const noexcept {
    switch (m_presentMode) {
        case SDL_GPU_PRESENTMODE_VSYNC:
            return "vsync";
        case SDL_GPU_PRESENTMODE_MAILBOX:
            return "mailbox";
        case SDL_GPU_PRESENTMODE_IMMEDIATE:
            return "immediate";
        default:
            break;
    }
    return "unknown";
}

}  // namespace vx
