#include "platform/window.hpp"

#include <stdexcept>
#include <string>

namespace vx {
namespace {

/// 选择可用的 Shader 字节码格式。
/// V0.1 的构建流程只产出 SPIR-V，因此在 Windows 上优先落到 Vulkan 后端；
/// DXIL 一并声明，便于后续接入 dxc 后自动切到 D3D12。
[[nodiscard]] SDL_GPUShaderFormat pick_shader_format() noexcept {
#if defined(SDL_PLATFORM_WINDOWS)
    return SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL;
#elif defined(SDL_PLATFORM_APPLE)
    return SDL_GPU_SHADERFORMAT_METALLIB;
#else
    return SDL_GPU_SHADERFORMAT_SPIRV;
#endif
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

    // Mailbox 可减少排队延迟；不支持则保持默认（FIFO）
    if (SDL_WindowSupportsGPUPresentMode(m_device, m_window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        SDL_SetGPUSwapchainParameters(m_device, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_MAILBOX);
    }
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

bool Window::pump_events() noexcept {
    SDL_Event event {};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }
    }
    return true;
}

}  // namespace vx
