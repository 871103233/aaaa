#pragma once

#include "render/mesh_renderer.hpp"

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
    glm::dvec3  characterPosition { 0.0 };    ///< 角色脚底位置（世界格，`double`）
    bool        characterOnGround  = false;   ///< 角色是否着地
    float       cameraYaw          = 0.0F;    ///< 相机 yaw（弧度）
    float       cameraPitch        = 0.0F;    ///< 相机 pitch（弧度）
    float       cameraDistance     = 0.0F;    ///< 相机实际跟随距离（格）
    float       brushRadius        = 0.0F;    ///< 笔刷半径（格）
    std::size_t loadedTileCount    = 0;       ///< 已加载（已网格化）的 tile 数
    std::size_t lastDirtyTileCount = 0;       ///< 最近一次笔刷弄脏的 tile 数
    std::size_t tileBodyCount      = 0;       ///< 已建立物理碰撞体的 tile 数
    bool        physicsReady       = false;   ///< 角色物理是否已就绪
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

    /// 开始一帧 ImGui；隐藏时为无操作（因此 `BuildUI` / `EndFrame` / `DrawOverlay` 也一并空转）。
    void BeginFrame();

    /// 记录本帧统计并构建面板内容。前置条件：已在可见状态下调用 `BeginFrame`。
    void BuildUI(const DebugStats& stats);

    /// 结束 ImGui 帧（`ImGui::Render`）；隐藏时为无操作。
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

    std::array<float, kHistorySize> m_history {};
    std::size_t                     m_historyCount = 0;
    std::size_t                     m_historyHead  = 0;  ///< 下一个写入位置
};

}  // namespace vx
