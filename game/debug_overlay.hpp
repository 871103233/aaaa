#pragma once

#include "object_palette.hpp"
#include "object_preview.hpp"
#include "portal_menu.hpp"
#include "render/mesh_renderer.hpp"
#include "system_panel.hpp"
#include "test_mode.hpp"
#include "ui_text.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

struct ImGuiContext;

namespace vx {

/// 物件选择器的显示数据（V0.5 E3；[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md)）。
///
/// **只存字符串 id，不存 `ObjectType*`** ⇒ 面板与世界切换 / 类型表重建**无生命周期耦合**（避免悬垂指针）。
struct PaletteModel {
    std::vector<std::string>              categoryNames;        ///< 一级：仓库（类别）
    std::vector<std::vector<std::string>> typeIdsByCategory;    ///< 二级：每个类别下的类型 id
    PaletteState                          state;                ///< 当前选择

    /// V0.5 E4：**当前选中类型的预览小图**（屏幕空间归一化三角形；由 game 层用**同一份**
    /// `buildLocalMesh` 几何算出，见 `game/object_preview.hpp`）。面板**只读**并画出来
    /// ⇒ 与 `MeshData` / `ObjectType*` 的**生命周期解耦**（同 `typeIdsByCategory` 的口径）。
    /// 空 = 该类型没有可预览的几何（面板显示 `ObjectPalettePreviewEmpty`）。
    std::vector<PreviewTriangle> previewTriangles;

    /// V0.5 E4：预览的**用户拖动朝向偏移**（弧度）。面板在拖动时累加；game 层把它叠加到自动旋转角上。
    /// 归零 = 只看自动旋转。
    float previewYawRadians = 0.0F;
};

/// 选择器上"用户做了什么"（每帧最多取走一次；见 `TakePaletteRequest`）。
struct PaletteRequest {
    enum class Action {
        None,
        EnterPlacement,  ///< 进入摆放模式（`typeId` = 当前选中类型）
        Save,            ///< 保存到可编辑层
        Cancel,          ///< 关闭面板，不改变状态
    };

    Action      action = Action::None;
    std::string typeId;  ///< `EnterPlacement` 时的选中类型 id
};

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
    std::size_t volumeBlockCount   = 0;       ///< **当前常驻**的可挖体积块数（T61：随玩家窗口变化）
    std::size_t carvedBlockCount   = 0;       ///< 其中被挖过 / 塌落改过的体积块数（T8 / T27 / T29）
    std::size_t volumeBodyCount    = 0;       ///< 可挖体积的三角网碰撞体数（T28）
    std::size_t volumePendingActions = 0;     ///< T61：常驻调度待办动作数（建 + 卸 + 淘汰）
    std::size_t volumeKeptDirtyCount = 0;     ///< T61：离开窗口但因**已被改动**而继续常驻的块数（ADR 0020 决策五）
    std::size_t collapseMovedVoxels = 0;      ///< 累计塌落移动的实心体素数（T29）
    bool        physicsReady       = false;   ///< 角色物理是否已就绪

    // ---- 渲染开销与 CPU 帧时间分解（T24；取自 engine/render 的通用统计）----
    std::uint32_t drawCalls     = 0;    ///< 最近一帧实际执行的 Draw Call 数
    std::uint64_t triangleCount = 0;    ///< 最近一帧实际绘制的三角形数
    std::uint64_t vertexCount   = 0;    ///< 最近一帧实际绘制的顶点数
    std::uint64_t textureBytes  = 0;    ///< 当前纹理显存字节总量（含 mip 链）
    std::uint64_t meshBytes     = 0;    ///< 当前**网格缓冲**显存字节总量（顶点 + 索引 + 骨骼；V0.7 H0）
    double        cpuLogicMs    = 0.0;  ///< 最近一帧逻辑步耗时（固定步循环：物理 + 相机，毫秒）
    double        cpuUiMs       = 0.0;  ///< 最近一帧 UI 构建耗时（毫秒）
    double        cpuRenderMs   = 0.0;  ///< 最近一帧渲染提交耗时（`RenderFrame` 及其内部上传，毫秒）
    double        swapchainWaitMs = 0.0;  ///< 最近一帧**等待交换链纹理**的毫秒数（T38；取自 `RenderStats`）
    /// V3/V9：角色附近（提示半径内）传送门的**提示显示名**（已按字体解析好：有 CJK 字体 ⇒ 门名，
    /// 否则 ⇒ **纯 ASCII** 的目标世界 id）；空串 = 附近没有门（HUD 不显示提示）。见 `ui_text.hpp` 的"绝不缺字"口径。
    std::string   nearbyPortalPromptName;

    /// V0.5 E2：坐标拾取辅助当前输出的**物件类型 id**（空串 = 未启用 / 类型表为空 ⇒ HUD 不显示该行）。
    ///
    /// 类型 id 是**纯 ASCII**（配置里的 id），因此无 CJK 字体时该行也不会缺字（标签经 `ui_text` 标签缝取）。
    std::string   placementTypeId;

    /// V0.5 E2：坐标拾取辅助的**最近一次反馈**（`"(12.3, 8.1) h 64.2"` / `"no ground hit"` / `"no type"`；
    /// 空串 = 还没按过 ⇒ HUD 不显示该行）。**内容恒为纯 ASCII** ⇒ 无 CJK 字体时也不缺字。
    std::string   lastPickFeedback;

    /// V0.5 E3：是否处于**摆放模式**（true ⇒ HUD 显示摆放模式横幅 `PlacementModeHintFormat`）。
    bool          placementModeActive = false;

    // ---- V0.9（[ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md)）：成套建筑摆放 ----
    /// 摆放模式是否在摆**成套建筑**（true ⇒ HUD 用 `PlacementBuildingHintFormat`）。
    bool          placementBuildingMode = false;
    /// 当前落点模式的**配置 token**（纯 ASCII：`sink` / `flat_only`；非建筑模式为空串）。
    std::string   placementLandingMode;
    /// 建筑摆放模式下的**待放室内变暗值**（`[` / `]` 调整）。
    float         placementDarkeningValue = 0.0F;
    /// 准星指向的**已有建筑** id（纯 ASCII；空串 = 没指向建筑）—— **选中态**调参的显示。
    std::string   placementSelectedBuilding;
    /// 被指向建筑当前的**有效**室内变暗值（`-1`（用全局值）时显示全局值）。
    float         placementSelectedDarkening = 0.0F;
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

    // ---- V9：传送门交互菜单（走近门按 `E` 打开；与 ESC 面板**同一套**捕获 / 抑制口径）----

    /// 打开菜单（数据由 game 层组装；见 `BuildPortalMenuModel`）。已打开时按新数据覆盖。
    void OpenPortalMenu(PortalMenuModel model) {
        m_portalMenu        = std::move(model);
        m_portalMenuOpen    = true;
        m_portalMenuRequest = PortalMenuRequest {};  // 新开一次 ⇒ 清掉上次未取走的动作
    }

    /// 关闭菜单（`Esc` / 取消 / 选择后由 game 层调用）。**不产生动作**。
    void ClosePortalMenu() noexcept {
        m_portalMenuOpen    = false;
        m_portalMenuRequest = PortalMenuRequest {};
    }

    [[nodiscard]] bool PortalMenuOpen() const noexcept { return m_portalMenuOpen; }

    /// **取走**一次选择（动作 + 目标世界）；取走后清零 ⇒ **只生效一次**。
    [[nodiscard]] PortalMenuRequest TakePortalMenuRequest() noexcept {
        PortalMenuRequest taken = std::move(m_portalMenuRequest);
        m_portalMenuRequest     = PortalMenuRequest {};
        return taken;
    }

    /// 任意面板（系统面板 / 传送门菜单 / 物件选择器）是否打开 —— 供 main 的捕获 / 输入抑制决策统一使用。
    [[nodiscard]] bool AnyBlockingPanelOpen() const noexcept {
        return m_systemPanel.IsOpen() || m_portalMenuOpen || m_objectPaletteOpen;
    }

    // ---- V0.5 E3：物件选择器（`F2`；[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md)）----
    /// 打开选择器（数据由 game 层组装；只存 id 字符串 ⇒ 无生命周期耦合）。已打开时按新数据覆盖。
    void OpenObjectPalette(PaletteModel model) {
        m_palette           = std::move(model);
        m_objectPaletteOpen = true;
        m_paletteRequest    = PaletteRequest {};  // 新开一次 ⇒ 清掉上次未取走的动作
    }

    /// 关闭选择器（`F2` / `Esc` / 取消 / 选择后由 game 层调用）。**不产生动作**。
    void CloseObjectPalette() noexcept {
        m_objectPaletteOpen = false;
        m_paletteRequest    = PaletteRequest {};
    }

    [[nodiscard]] bool ObjectPaletteOpen() const noexcept { return m_objectPaletteOpen; }

    /// 就地访问面板数据（`main` 读回"当前选中类型"；面板构建时也会改写 `state`）。
    [[nodiscard]] PaletteModel&       MutablePalette() noexcept { return m_palette; }
    [[nodiscard]] const PaletteModel& Palette() const noexcept { return m_palette; }

    /// **取走**一次选择器动作；取走后清零 ⇒ **只生效一次**。
    [[nodiscard]] PaletteRequest TakePaletteRequest() noexcept {
        PaletteRequest taken = std::move(m_paletteRequest);
        m_paletteRequest     = PaletteRequest {};
        return taken;
    }

    /// 设置**测试模式**（T85）：驱动调试面板顶部的只读横幅（自动测试 / 人工测试 + 人工验收项）。
    /// 全运行期不变，启动时设置一次。
    void SetTestMode(TestModeInfo mode) { m_testMode = std::move(mode); }

    /// 通知 ImGui 玩法当前是否处于**相对鼠标（捕获）**状态。
    ///
    /// 相对模式下 SDL 报告的鼠标绝对坐标无意义；按 ImGui 官方建议在捕获期间置
    /// `ImGuiConfigFlags_NoMouse`，让 ImGui 不去按绝对坐标算 `HoveredWindow`（否则光标停在
    /// 面板上时会给控件加悬停高亮）。**注意（V0.9）**：玩法输入抑制**不再**依赖 ImGui 的
    /// `WantCapture*`（见 `gameplay_input.hpp`），因此本标志只影响 ImGui 自身的悬停判定。
    void SetGameplayMouseCaptured(bool captured) noexcept { m_gameplayMouseCaptured = captured; }

    /// 设置**加载画面**的状态：阶段标签 + 总进度（`progress ∈ [0,1]`）。
    ///
    /// 置位后 `BeginFrame` 照常起帧，`BuildLoadingUI` 会绘制一个居中窗口显示当前阶段与进度条。
    /// 这是 SKILL「不冻结画面」要求的**可见进度反馈**——没有进度反馈的等待会被玩家判定为"卡死"。
    /// 无堆分配（阶段是枚举、进度是浮点），可逐帧调用。
    void SetLoadingStatus(UiLabel stage, float progress) noexcept;
    void ClearLoadingStatus() noexcept { m_loadingActive = false; }
    [[nodiscard]] bool LoadingActive() const noexcept { return m_loadingActive; }

    /// 构建加载画面。前置条件：已调用 `BeginFrame`；未置位加载状态时为无操作。
    void BuildLoadingUI();

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

    /// 构建**常驻坐标 HUD**（屏幕左上角，只读、不接管输入）。前置条件：已调用 `BeginFrame`。
    void BuildHud(const DebugStats& stats);

    /// 构建**传送门交互菜单**（V9；居中模态窗口）。前置条件：已调用 `BeginFrame`；未打开时无操作。
    void BuildPortalMenu();

    /// 构建**物件选择器**（V0.5 E3；居中模态窗口，二级列表）。前置条件：已调用 `BeginFrame`；未打开时无操作。
    void BuildObjectPalette();

    ImGuiContext* m_context = nullptr;
    bool          m_visible = true;

    /// 常驻坐标 HUD 是否显示（默认显示；与 F1 面板相互独立）。
    bool m_hudVisible = true;
    /// 上一帧 HUD 的实际像素高度（供 F1 面板排到其下方，避免左上角重叠）。
    float m_hudHeight = 0.0F;

    /// 构造期解析到的字体语言（T16）：是否加载到 CJK 字体，决定标签中 / 英。
    bool m_cjkFontLoaded = false;

    SystemPanel m_systemPanel;

    // V9：传送门交互菜单 —— 显示数据 + 是否打开 + 本帧待取走的"选择"。
    PortalMenuModel   m_portalMenu;
    bool              m_portalMenuOpen = false;
    PortalMenuRequest m_portalMenuRequest;

    // V0.5 E3：物件选择器 —— 显示数据（只存 id）+ 是否打开 + 本帧待取走的"动作"。
    PaletteModel   m_palette;
    bool           m_objectPaletteOpen = false;
    PaletteRequest m_paletteRequest;

    /// 测试模式（T85）：由 `SetTestMode` 在启动时设置一次，面板顶部据此显示只读横幅。
    TestModeInfo m_testMode;

    /// 本帧是否已调用 `ImGui::NewFrame`（调试面板或系统面板可见时为 true）。
    bool m_frameActive = false;
    /// 玩法是否处于相对鼠标（捕获）状态；为 true 时对本帧 ImGui 置 `NoMouse`。
    bool m_gameplayMouseCaptured = false;

    /// 加载画面的状态（见 `SetLoadingStatus`）：是否置位、当前阶段、总进度。
    bool    m_loadingActive   = false;
    UiLabel m_loadingStage    = UiLabel::LoadingTitle;
    float   m_loadingProgress = 0.0F;

    std::array<float, kHistorySize> m_history {};
    std::size_t                     m_historyCount = 0;
    std::size_t                     m_historyHead  = 0;  ///< 下一个写入位置
};

}  // namespace vx
