#pragma once

#include "render/mesh_renderer.hpp"
#include "system_panel.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct ImGuiContext;

namespace vx {

/// 调试面板的统计快照：由 game 每帧填充，面板**只读**，不回写任何模拟状态（红线 11）。
struct DebugStats {
    double      frameSeconds      = 0.0;      ///< 上一帧真实时长（秒）
    int         stepsThisFrame     = 0;       ///< 本帧执行的固定逻辑步数
    int         frameRateCap       = 0;       ///< 当前帧率上限（Hz，T17）
    glm::dvec3  characterPosition { 0.0 };    ///< 角色脚底位置（世界格，`double`）
    bool        characterOnGround  = false;   ///< 角色是否着地
    bool        characterFlying    = false;   ///< 角色是否处于飞行模式（T12）
    float       cameraYaw          = 0.0F;    ///< 相机 yaw（弧度）
    float       cameraPitch        = 0.0F;    ///< 相机 pitch（弧度）
    float       cameraDistance     = 0.0F;    ///< 相机实际跟随距离（格）
    float       explosionRadius    = 0.0F;    ///< 当前弹丸的爆炸半径（格，T27）
    bool        mouseCaptured      = false;   ///< 鼠标是否处于相对模式（捕获，T14）
    std::size_t orbActiveCount     = 0;       ///< 活动光球数（T27）
    std::size_t orbCapacity        = 0;       ///< 光球池容量（= `projectiles.toml` 的 `max_active`）
    std::size_t loadedTileCount    = 0;       ///< 已加载（已网格化）的 tile 数
    std::size_t lastDirtyTileCount = 0;       ///< 最近一帧因爆炸而重网格的单元数（地表 tile 或体积块）
    std::size_t tileBodyCount      = 0;       ///< 已建立物理碰撞体的 tile 数
    std::size_t volumeBlockCount   = 0;       ///< 可挖体积块总数（T8）
    std::size_t carvedBlockCount   = 0;       ///< 其中被挖过 / 塌落改过的体积块数（T8 / T27 / T29）
    std::size_t volumeBodyCount    = 0;       ///< 可挖体积的三角网碰撞体数（T28）
    std::size_t collapseMovedVoxels = 0;      ///< 累计塌落移动的实心体素数（T29）
    bool        physicsReady       = false;   ///< 角色物理是否已就绪

    // ---- 渲染开销与 CPU 帧时间分解（T24；取自 engine/render 的通用统计）----
    std::uint32_t drawCalls     = 0;    ///< 最近一帧实际执行的 Draw Call 数
    std::uint64_t triangleCount = 0;    ///< 最近一帧实际绘制的三角形数
    std::uint64_t vertexCount   = 0;    ///< 最近一帧实际绘制的顶点数
    std::uint64_t textureBytes  = 0;    ///< 当前纹理显存字节总量（含 mip 链）
    double        cpuLogicMs    = 0.0;  ///< 最近一帧逻辑步耗时（固定步循环：物理 + 相机，毫秒）
    double        cpuUiMs       = 0.0;  ///< 最近一帧 UI 构建耗时（毫秒）
    double        cpuRenderMs   = 0.0;  ///< 最近一帧渲染提交耗时（`RenderFrame` 及其内部上传，毫秒）
};

/// 极简 ImGui 调试面板（T9）。基于 imgui 的 **SDL3 平台后端 + SDL3_gpu 渲染后端**。
///
/// 设计约束（方案 §9.3 / references/gameplay-v0.1.md §7）：
///   - 面板数据经 `DebugStats` **独立统计接口**传入，**不进世界层热路径**；
///   - 帧时间分位用**固定长度环形缓冲**计算，每帧**零堆分配**；
///   - 隐藏时整个 ImGui 帧被跳过（不构建 UI、不提交绘制），开销近似为零。
///
/// 生命周期：必须在 GPU 设备销毁**之前**析构（SDLGPU3 后端会释放设备对象）。
/// 线程约定：只在渲染线程（拥有 GPU 设备与窗口的线程）使用。
class DebugOverlay final : public IRenderOverlay {
public:
    /// 前置条件：`device` / `window` 有效且窗口已被该设备认领。
    DebugOverlay(SDL_GPUDevice* device, SDL_Window* window);
    ~DebugOverlay() override;

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    DebugOverlay(DebugOverlay&&) = delete;
    DebugOverlay& operator=(DebugOverlay&&) = delete;

    void Toggle() noexcept { m_visible = !m_visible; }
    [[nodiscard]] bool Visible() const noexcept { return m_visible; }

    /// 本机是否加载到 CJK 字体（T16）：true 时面板用中文标签，false 时整表回退纯 ASCII 英文。
    ///
    /// 取值在构造期由 [`ApplyUiFont`](ui_font.hpp) 决定，之后不再变化。
    [[nodiscard]] bool UsesCjkLabels() const noexcept { return m_cjkFontLoaded; }

    /// SDL 事件转发入口（T15）：安装到 `Window::SetEventCallback`，把每个事件交给 ImGui 后端。
    ///
    /// 这是让 ImGui 面板**可交互**的前提——此前只读正是因为事件从未转发。`userData` 为 `DebugOverlay*`。
    static void OnSdlEvent(void* userData, const SDL_Event& event);

    /// 系统面板（T15）开关 / 查询。
    void               ToggleSystemPanel() noexcept { m_systemPanel.Toggle(); }
    [[nodiscard]] bool SystemPanelOpen() const noexcept { return m_systemPanel.IsOpen(); }

    /// 本帧 ImGui 是否想接管鼠标 / 键盘（`io.WantCaptureMouse` / `WantCaptureKeyboard`）。
    ///
    /// 只在**本帧已开始 ImGui 帧**时有效；未开始（面板与调试面板都隐藏）时恒为 false。
    /// 供 `main` 的输入抑制纯函数使用（见 `gameplay_input.hpp`）。
    [[nodiscard]] bool WantsCaptureMouse() const noexcept { return m_wantCaptureMouse; }
    [[nodiscard]] bool WantsCaptureKeyboard() const noexcept { return m_wantCaptureKeyboard; }

    /// 通知 ImGui 玩法当前是否处于**相对鼠标（捕获）**状态。
    ///
    /// 相对模式下 SDL 报告的鼠标绝对坐标无意义，若照常喂给 ImGui，会把"悬停"算到 UI 上，
    /// 从而误抑制玩法视角。捕获期间据此置 `ImGuiConfigFlags_NoMouse`（面板打开时必已释放捕获，
    /// 因此不影响面板交互）。
    void SetGameplayMouseCaptured(bool captured) noexcept { m_gameplayMouseCaptured = captured; }

    /// 开始一帧 ImGui。当调试面板与系统面板都不可见时为无操作
    /// （因此 `BuildUI` / `EndFrame` / `DrawOverlay` 也一并空转，开销近似为零）。
    void BeginFrame();

    /// 记录本帧统计并构建面板内容（调试面板 + 系统面板）。前置条件：已调用 `BeginFrame`。
    void BuildUI(const DebugStats& stats, SystemPanelContext& panelContext);

    /// 结束 ImGui 帧（`ImGui::Render`）；未开始帧时为无操作。
    void EndFrame();

    /// IRenderOverlay：在同一命令缓冲、3D 通道之后绘制面板。
    void DrawOverlay(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* swapchain, std::uint32_t width,
                     std::uint32_t height) override;

private:
    /// 环形缓冲容量：60 FPS 下约 2 秒的帧时间样本。
    static constexpr std::size_t kHistorySize = 120;

    void PushFrameTime(double seconds) noexcept;

    /// 分位（`percentile` ∈ [0, 1]）；样本不足时返回最近一次帧时间。
    [[nodiscard]] double Percentile(double percentile) const noexcept;

    ImGuiContext* m_context = nullptr;
    bool          m_visible = true;

    /// 构造期解析到的字体语言（T16）：是否加载到 CJK 字体，决定标签中 / 英。
    bool m_cjkFontLoaded = false;

    SystemPanel m_systemPanel;

    /// 本帧是否已调用 `ImGui::NewFrame`（调试面板或系统面板可见时为 true）。
    bool m_frameActive = false;
    /// 本帧开始 ImGui 帧后采样的 ImGui 捕获标志（供输入抑制决策）。
    bool m_wantCaptureMouse    = false;
    bool m_wantCaptureKeyboard = false;
    /// 玩法是否处于相对鼠标（捕获）状态；为 true 时对本帧 ImGui 置 `NoMouse`。
    bool m_gameplayMouseCaptured = false;

    std::array<float, kHistorySize> m_history {};
    std::size_t                     m_historyCount = 0;
    std::size_t                     m_historyHead  = 0;  ///< 下一个写入位置
};

}  // namespace vx
