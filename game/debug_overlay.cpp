#include "debug_overlay.hpp"

#include "core/fixed_step.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace vx {

DebugOverlay::DebugOverlay(SDL_GPUDevice* device, SDL_Window* window) {
    IMGUI_CHECKVERSION();
    m_context = ImGui::CreateContext();
    if (m_context == nullptr) {
        throw std::runtime_error("ImGui::CreateContext 失败");
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 不落盘 imgui.ini，避免运行期写文件
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
        ImGui::DestroyContext(m_context);
        m_context = nullptr;
        throw std::runtime_error(std::string("ImGui_ImplSDL3_InitForSDLGPU 失败：") + SDL_GetError());
    }

    ImGui_ImplSDLGPU3_InitInfo initInfo;
    initInfo.Device               = device;
    initInfo.ColorTargetFormat    = SDL_GetGPUSwapchainTextureFormat(device, window);
    initInfo.MSAASamples          = SDL_GPU_SAMPLECOUNT_1;
    initInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    initInfo.PresentMode          = SDL_GPU_PRESENTMODE_VSYNC;  // 仅多视口模式使用，这里取默认

    if (!ImGui_ImplSDLGPU3_Init(&initInfo)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_context);
        m_context = nullptr;
        throw std::runtime_error(std::string("ImGui_ImplSDLGPU3_Init 失败：") + SDL_GetError());
    }

    VX_LOG_INFO("ImGui 调试面板已初始化（SDL3 + SDL3_gpu 后端）");
}

DebugOverlay::~DebugOverlay() {
    if (m_context == nullptr) {
        return;
    }
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_context);
    m_context = nullptr;
}

void DebugOverlay::BeginFrame() {
    if (!m_visible) {
        return;
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
}

void DebugOverlay::BuildUI(const DebugStats& stats) {
    if (!m_visible) {
        return;
    }

    PushFrameTime(stats.frameSeconds);

    const double frameMs = stats.frameSeconds * 1000.0;
    const double p50Ms   = Percentile(0.50) * 1000.0;
    const double p95Ms   = Percentile(0.95) * 1000.0;
    const double fps     = (stats.frameSeconds > 0.0) ? (1.0 / stats.frameSeconds) : 0.0;

    ImGui::SetNextWindowBgAlpha(0.85F);
    ImGui::Begin("V0.1 调试面板", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("时间步");
    ImGui::Text("帧时间：%.2f ms（P50 %.2f / P95 %.2f ms）", frameMs, p50Ms, p95Ms);
    ImGui::Text("FPS：%.1f", fps);
    ImGui::Text("本帧固定步：%d（dt = %.5f s）", stats.stepsThisFrame, kFixedDt);

    ImGui::Separator();
    ImGui::TextUnformatted("角色");
    ImGui::Text("位置（世界格）：(%.2f, %.2f, %.2f)", stats.characterPosition.x, stats.characterPosition.y,
                stats.characterPosition.z);
    ImGui::Text("所在格：(%d, %d, %d)", static_cast<int>(std::floor(stats.characterPosition.x)),
                static_cast<int>(std::floor(stats.characterPosition.y)),
                static_cast<int>(std::floor(stats.characterPosition.z)));
    ImGui::Text("着地：%s  物理：%s", stats.characterOnGround ? "是" : "否", stats.physicsReady ? "就绪" : "未就绪");

    ImGui::Separator();
    ImGui::TextUnformatted("相机 / 笔刷");
    ImGui::Text("yaw %.1f°  pitch %.1f°  距离 %.2f 格", stats.cameraYaw * 57.2957795F, stats.cameraPitch * 57.2957795F,
                stats.cameraDistance);
    ImGui::Text("笔刷半径：%.1f 格", stats.brushRadius);

    ImGui::Separator();
    ImGui::TextUnformatted("世界 / 物理");
    ImGui::Text("已加载 tile：%zu", stats.loadedTileCount);
    ImGui::Text("上次弄脏 tile：%zu", stats.lastDirtyTileCount);
    ImGui::Text("地表碰撞体 tile：%zu", stats.tileBodyCount);

    ImGui::End();
}

void DebugOverlay::EndFrame() {
    if (!m_visible) {
        return;
    }
    ImGui::Render();
}

void DebugOverlay::DrawOverlay(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* swapchain, std::uint32_t,
                               std::uint32_t) {
    if (!m_visible) {
        return;
    }
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->CmdListsCount == 0) {
        return;
    }

    // 必须先把顶点 / 索引数据上传到该命令缓冲，再开渲染通道。
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);

    SDL_GPUColorTargetInfo colorTarget {};
    colorTarget.texture  = swapchain;
    colorTarget.load_op  = SDL_GPU_LOADOP_LOAD;  // 保留 3D 通道结果，叠加绘制
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
    if (pass != nullptr) {
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, pass);
        SDL_EndGPURenderPass(pass);
    }
}

void DebugOverlay::PushFrameTime(double seconds) noexcept {
    m_history[m_historyHead] = static_cast<float>(seconds);
    m_historyHead            = (m_historyHead + 1) % kHistorySize;
    if (m_historyCount < kHistorySize) {
        ++m_historyCount;
    }
}

double DebugOverlay::Percentile(double percentile) const noexcept {
    if (m_historyCount == 0) {
        return 0.0;
    }

    // 固定容量副本，零堆分配。
    std::array<float, kHistorySize> sorted {};
    for (std::size_t i = 0; i < m_historyCount; ++i) {
        sorted[i] = m_history[i];
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(m_historyCount));

    std::size_t index = static_cast<std::size_t>(percentile * static_cast<double>(m_historyCount - 1) + 0.5);
    if (index >= m_historyCount) {
        index = m_historyCount - 1;
    }
    return static_cast<double>(sorted[index]);
}

}  // namespace vx
