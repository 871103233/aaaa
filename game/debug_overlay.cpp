#include "debug_overlay.hpp"

#include "core/fixed_step.hpp"
#include "core/log.hpp"
#include "ui_font.hpp"
#include "ui_text.hpp"
#include "ui_theme.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace vx {
namespace {

/// 调试面板行标签列的固定宽度（像素）：数值列据此对齐，使各行数字成列。
constexpr float kLabelColumnWidth = 176.0F;

/// 一行"标签 : 数值（格式化）"：标签列定宽，数值列对齐到同一 x。
///
/// 标签与格式串一律经 [`UiText`](ui_text.hpp) 取得，本函数不接收字符串字面量。
void StatRow(const char* label, const char* format, ...) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelColumnWidth);

    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

/// 一行"标签 : 数值（原样字符串）"：用于取值本身已是完整文本的行（如鼠标捕获状态）。
void ValueRow(const char* label, const char* value) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelColumnWidth);
    ImGui::TextUnformatted(value);
}

}  // namespace

DebugOverlay::DebugOverlay(SDL_GPUDevice* device, SDL_Window* window) {
    IMGUI_CHECKVERSION();
    m_context = ImGui::CreateContext();
    if (m_context == nullptr) {
        throw std::runtime_error("ImGui::CreateContext 失败");
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 不落盘 imgui.ini，避免运行期写文件

    // T16：统一主题（唯一的样式改动入口）与字体解析。字体解析失败不报错，
    // 只把标签语言整表回退为纯 ASCII 英文（保证任何机器上都不出现缺字 `?`）。
    ApplyUiTheme();
    m_cjkFontLoaded = ApplyUiFont(io);

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

    VX_LOG_INFO("ImGui 面板已初始化（SDL3 + SDL3_gpu 后端；统一暗色主题；标签语言：%s）",
                m_cjkFontLoaded ? "中文（已加载 CJK 字体）" : "英文（未找到 CJK 字体，纯 ASCII 回退）");
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

void DebugOverlay::OnSdlEvent(void* userData, const SDL_Event& event) {
    // userData 未使用：ImGui 后端是全局单例，但保留参数以匹配 `Window::EventCallback` 签名。
    (void)userData;
    (void)ImGui_ImplSDL3_ProcessEvent(&event);
}

void DebugOverlay::BeginFrame() {
    // 只要还有任一 ImGui 窗口可见就必须起帧（系统面板打开时调试面板可能隐藏）。
    m_frameActive = m_visible || m_systemPanel.IsOpen();
    if (!m_frameActive) {
        m_wantCaptureMouse    = false;
        m_wantCaptureKeyboard = false;
        return;
    }

    // 相对鼠标模式下 SDL 给的绝对坐标无意义：对本帧 ImGui 置 NoMouse，避免把 UI 误判为被悬停。
    ImGuiIO& io = ImGui::GetIO();
    if (m_gameplayMouseCaptured) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();

    // 在构建本帧窗口**之前**采样捕获标志：ImGui 在此已按上一帧的布局算好 hover，
    // 恒为"最近一帧 UI 是否想接管输入"，足够用于抑制本帧玩法输入（面板打开本身另会全量抑制）。
    m_wantCaptureMouse    = io.WantCaptureMouse;
    m_wantCaptureKeyboard = io.WantCaptureKeyboard;
}

void DebugOverlay::BuildUI(const DebugStats& stats, SystemPanelContext& panelContext) {
    if (!m_frameActive) {
        return;
    }

    // T16：标签语言由构造期字体解析结果决定，两个面板共用同一个开关值。
    const bool cjk = m_cjkFontLoaded;

    m_systemPanel.Build(panelContext, cjk);

    if (!m_visible) {
        return;
    }

    PushFrameTime(stats.frameSeconds);

    const double frameMs = stats.frameSeconds * 1000.0;
    const double p50Ms   = Percentile(0.50) * 1000.0;
    const double p95Ms   = Percentile(0.95) * 1000.0;
    const double fps     = (stats.frameSeconds > 0.0) ? (1.0 / stats.frameSeconds) : 0.0;

    ImGui::SetNextWindowBgAlpha(0.88F);
    ImGui::Begin(UiText(UiLabel::DebugPanelTitle, cjk), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    ImGui::SeparatorText(UiText(UiLabel::SectionTiming, cjk));
    StatRow(UiText(UiLabel::FrameTime, cjk), UiText(UiLabel::FrameTimeFormat, cjk), frameMs, p50Ms, p95Ms);
    StatRow(UiText(UiLabel::Fps, cjk), UiText(UiLabel::FpsFormat, cjk), fps);
    StatRow(UiText(UiLabel::FrameRateCap, cjk), UiText(UiLabel::FrameRateCapFormat, cjk), stats.frameRateCap);
    StatRow(UiText(UiLabel::FixedSteps, cjk), UiText(UiLabel::FixedStepsFormat, cjk), stats.stepsThisFrame,
            kFixedDt);

    ImGui::SeparatorText(UiText(UiLabel::SectionCharacter, cjk));
    StatRow(UiText(UiLabel::Position, cjk), UiText(UiLabel::PositionFormat, cjk), stats.characterPosition.x,
            stats.characterPosition.y, stats.characterPosition.z);
    StatRow(UiText(UiLabel::Cell, cjk), UiText(UiLabel::CellFormat, cjk),
            static_cast<int>(std::floor(stats.characterPosition.x)),
            static_cast<int>(std::floor(stats.characterPosition.y)),
            static_cast<int>(std::floor(stats.characterPosition.z)));
    StatRow(UiText(UiLabel::State, cjk), UiText(UiLabel::StateFormat, cjk),
            UiText(stats.characterOnGround ? UiLabel::ValueYes : UiLabel::ValueNo, cjk),
            UiText(stats.physicsReady ? UiLabel::ValueReady : UiLabel::ValueNotReady, cjk),
            UiText(stats.characterFlying ? UiLabel::ValueYes : UiLabel::ValueNo, cjk));

    ImGui::SeparatorText(UiText(UiLabel::SectionCameraOrb, cjk));
    StatRow(UiText(UiLabel::CameraOrientation, cjk), UiText(UiLabel::CameraOrientationFormat, cjk),
            stats.cameraYaw * 57.2957795F, stats.cameraPitch * 57.2957795F, stats.cameraDistance);
    ValueRow(UiText(UiLabel::MouseCapture, cjk),
             UiText(stats.mouseCaptured ? UiLabel::MouseCaptureOn : UiLabel::MouseCaptureOff, cjk));
    StatRow(UiText(UiLabel::ExplosionRadius, cjk), UiText(UiLabel::ExplosionRadiusFormat, cjk),
            stats.explosionRadius);
    StatRow(UiText(UiLabel::Orbs, cjk), UiText(UiLabel::OrbCountFormat, cjk), stats.orbActiveCount,
            stats.orbCapacity);

    ImGui::SeparatorText(UiText(UiLabel::SectionWorldPhysics, cjk));
    StatRow(UiText(UiLabel::LoadedTiles, cjk), UiText(UiLabel::CountFormat, cjk), stats.loadedTileCount);
    StatRow(UiText(UiLabel::DirtyTiles, cjk), UiText(UiLabel::CountFormat, cjk), stats.lastDirtyTileCount);
    StatRow(UiText(UiLabel::TileBodies, cjk), UiText(UiLabel::CountFormat, cjk), stats.tileBodyCount);
    StatRow(UiText(UiLabel::VolumeBlocks, cjk), UiText(UiLabel::VolumeBlocksFormat, cjk), stats.volumeBlockCount,
            stats.carvedBlockCount);
    StatRow(UiText(UiLabel::VolumeBodies, cjk), UiText(UiLabel::CountFormat, cjk), stats.volumeBodyCount);
    StatRow(UiText(UiLabel::CollapseMoved, cjk), UiText(UiLabel::CountFormat, cjk), stats.collapseMovedVoxels);

    // T24：渲染开销（实测自 MeshRenderer）+ 纹理显存记账（ADR 0010）；纯展示，不回写任何状态。
    ImGui::SeparatorText(UiText(UiLabel::SectionRenderCost, cjk));
    StatRow(UiText(UiLabel::DrawCalls, cjk), UiText(UiLabel::CountFormat, cjk),
            static_cast<std::size_t>(stats.drawCalls));
    StatRow(UiText(UiLabel::Triangles, cjk), UiText(UiLabel::CountFormat, cjk),
            static_cast<std::size_t>(stats.triangleCount));
    StatRow(UiText(UiLabel::Vertices, cjk), UiText(UiLabel::CountFormat, cjk),
            static_cast<std::size_t>(stats.vertexCount));
    StatRow(UiText(UiLabel::TextureVram, cjk), UiText(UiLabel::TextureVramFormat, cjk),
            static_cast<double>(stats.textureBytes) / (1024.0 * 1024.0));

    // T24：CPU 帧时间分解（毫秒，显示到 0.01 ms）——由 main 用单调计时分别测量。
    ImGui::SeparatorText(UiText(UiLabel::SectionCpuFrameTime, cjk));
    StatRow(UiText(UiLabel::CpuLogicStep, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuLogicMs);
    StatRow(UiText(UiLabel::CpuUiBuild, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuUiMs);
    StatRow(UiText(UiLabel::CpuRenderSubmit, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuRenderMs);

    // T24：各 pass GPU 时间。SDL3_gpu **没有时间戳查询 API**，故如实标注"不可用"——绝不编造数字。
    ImGui::SeparatorText(UiText(UiLabel::SectionGpuPassTime, cjk));
    ValueRow(UiText(UiLabel::GpuPassTime, cjk), UiText(UiLabel::GpuTimeUnavailable, cjk));

    ImGui::End();
}

void DebugOverlay::EndFrame() {
    if (!m_frameActive) {
        return;
    }
    ImGui::Render();
}

void DebugOverlay::DrawOverlay(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* swapchain, std::uint32_t,
                               std::uint32_t) {
    if (!m_frameActive) {
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
