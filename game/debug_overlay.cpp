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
#include <cstdio>
#include <stdexcept>
#include <string>

namespace vx {
namespace {

/// 调试面板行标签列的固定宽度（像素）：数值列据此对齐，使各行数字成列。
constexpr float kLabelColumnWidth = 176.0F;

/// V0.5 E4：物件选择器**预览区**的固定尺寸（像素）。高度与两侧列表子窗口一致 ⇒ 三列视觉成一行。
constexpr float kPalettePreviewWidth  = 200.0F;
constexpr float kPalettePreviewHeight = 240.0F;
/// 预览区圆角（与面板其余控件的圆角一致）。
constexpr float kPalettePreviewRounding = 4.0F;
/// 预览区拖动灵敏度（弧度 / 像素）：整块预览宽度拖满 ≈ 2π 的一半，手感适中。
constexpr float kPalettePreviewDragRadiansPerPixel = 0.01F;

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
    // 只要还有任一 ImGui 窗口可见就必须起帧（系统面板打开时调试面板可能隐藏；加载画面 / 常驻 HUD 同样要出帧）。
    m_frameActive =
        m_visible || m_systemPanel.IsOpen() || m_portalMenuOpen || m_objectPaletteOpen || m_loadingActive ||
        m_hudVisible;
    if (!m_frameActive) {
        return;
    }

    // 相对鼠标模式下 SDL 给的绝对坐标无意义：对本帧 ImGui 置 NoMouse（ImGui 官方建议），
    // 避免按绝对坐标把某个面板算成"被悬停"（V0.9 起它**不再**参与玩法输入抑制，见 `gameplay_input.hpp`）。
    ImGuiIO& io = ImGui::GetIO();
    if (m_gameplayMouseCaptured) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
}

void DebugOverlay::SetLoadingStatus(UiLabel stage, float progress) noexcept {
    m_loadingActive   = true;
    m_loadingStage    = stage;
    m_loadingProgress = std::clamp(progress, 0.0F, 1.0F);
}

void DebugOverlay::BuildLoadingUI() {
    if (!m_frameActive || !m_loadingActive) {
        return;
    }
    const bool cjk = m_cjkFontLoaded;

    // 居中、不可移动、自动尺寸：加载期玩家无需与它交互，只要"看得见进度在走"。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowBgAlpha(0.92F);
    ImGui::Begin(UiText(UiLabel::LoadingTitle, cjk), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextUnformatted(UiText(m_loadingStage, cjk));

    char percent[16] = {};
    std::snprintf(percent, sizeof(percent), UiText(UiLabel::LoadingProgressFormat, cjk),
                  static_cast<double>(m_loadingProgress) * 100.0);
    ImGui::ProgressBar(m_loadingProgress, ImVec2(360.0F, 0.0F), percent);

    ImGui::TextUnformatted(UiText(UiLabel::LoadingHint, cjk));
    ImGui::End();
}

void DebugOverlay::BuildHud(const DebugStats& stats) {
    const bool cjk = m_cjkFontLoaded;

    // 左上角常驻、只读：不接管鼠标 / 键盘（`NoInputs`），不可移动 / 缩放 / 折叠，不落盘布局。
    ImGui::SetNextWindowPos(ImVec2(8.0F, 8.0F), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55F);
    ImGui::Begin(UiText(UiLabel::HudCoordinates, cjk), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text(UiText(UiLabel::HudCoordinatesFormat, cjk), stats.characterPosition.x, stats.characterPosition.y,
                stats.characterPosition.z);
    ImGui::Text(UiText(UiLabel::HudCellFormat, cjk), static_cast<int>(std::floor(stats.characterPosition.x)),
                static_cast<int>(std::floor(stats.characterPosition.y)),
                static_cast<int>(std::floor(stats.characterPosition.z)));

    // V3/V9：走近传送门时的交互提示（一行；按 `E` **打开菜单**）。
    // 显示名已在 game 层按字体解析好（无 CJK 字体 ⇒ 退化为纯 ASCII 的 world id）⇒ 不会缺字。
    if (!stats.nearbyPortalPromptName.empty()) {
        ImGui::Separator();
        ImGui::Text(UiText(UiLabel::PortalPromptFormat, cjk), stats.nearbyPortalPromptName.c_str());
    }

    // V0.5 E2：坐标拾取辅助提示（一行）。当前类型 id 是纯 ASCII ⇒ 无 CJK 字体时也不缺字。
    if (!stats.placementTypeId.empty()) {
        ImGui::Separator();
        ImGui::Text(UiText(UiLabel::PlacementHintFormat, cjk), stats.placementTypeId.c_str());
    }
    // 最近一次拾取反馈（按 F2 后出现）—— 让按键在游戏内**可见**，不依赖控制台（空串则不占一行）。
    if (!stats.lastPickFeedback.empty()) {
        ImGui::Text(UiText(UiLabel::PlacementPickedFormat, cjk), stats.lastPickFeedback.c_str());
    }
    // V0.10 / S9：**未保存改动数**（`> 0` 才显示）—— 摆放 / 删除后若尚未写盘，退出前也能一眼看到。
    if (stats.editLayerUnsaved > 0) {
        ImGui::Text(UiText(UiLabel::EditLayerUnsavedFormat, cjk), stats.editLayerUnsaved);
    }

    // V0.5 E3：摆放模式横幅 —— 模式内**显式**告知键位（模式内左键/Esc/E 让位，见 ADR 0032）。
    // V0.9 / ADR 0036：成套建筑摆放走**另一条**横幅（多出 落点模式 / 变暗值 两项）。
    if (stats.placementModeActive && !stats.placementTypeId.empty()) {
        ImGui::Separator();
        if (stats.placementBuildingMode) {
            ImGui::Text(UiText(UiLabel::PlacementBuildingHintFormat, cjk), stats.placementTypeId.c_str(),
                        stats.placementLandingMode.c_str(), stats.placementDarkeningValue);
        } else {
            ImGui::Text(UiText(UiLabel::PlacementModeHintFormat, cjk), stats.placementTypeId.c_str());
        }
    }
    // V0.9：**选中态**横幅 —— 准星指向的已有建筑（`[` / `]` 改的就是它）。
    if (!stats.placementSelectedBuilding.empty()) {
        ImGui::Text(UiText(UiLabel::PlacementSelectedHintFormat, cjk), stats.placementSelectedBuilding.c_str(),
                    stats.placementSelectedDarkening);
    }

    // 记录实际高度：F1 面板据此把初始位置排在 HUD 下方（避免左上角重叠）。
    m_hudHeight = ImGui::GetWindowSize().y;
    ImGui::End();
}

void DebugOverlay::BuildPortalMenu() {
    if (!m_frameActive || !m_portalMenuOpen) {
        return;
    }
    const bool cjk = m_cjkFontLoaded;

    // 居中、固定、自动尺寸；不可移动 / 折叠 / 缩放，不落盘布局。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowBgAlpha(0.92F);
    ImGui::Begin(UiText(UiLabel::PortalMenuTitle, cjk), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    // 标题行：「门名 → 秘境名」。无 CJK 字体 ⇒ 秘境名退化为**纯 ASCII** 的 world id（绝不出缺字）。
    const std::string realmDisplay = cjk ? m_portalMenu.realmName : m_portalMenu.targetWorldId;
    char              title[256]    = {};
    std::snprintf(title, sizeof(title), UiText(UiLabel::PortalMenuTitleFormat, cjk),
                  m_portalMenu.portalName.c_str(), realmDisplay.c_str());
    ImGui::TextUnformatted(title);

    if (m_portalMenu.canReset) {
        // `generation` = **已重置次数**（0 = 尚未重置）⇒ 显示为 1 起的"第 N 次生成"（首次 = 第 1 次）。
        char generation[64] = {};
        std::snprintf(generation, sizeof(generation), UiText(UiLabel::PortalMenuGenerationFormat, cjk),
                      m_portalMenu.generation + 1U);
        ImGui::TextUnformatted(generation);
    }
    ImGui::TextUnformatted(UiText(UiLabel::PortalMenuSessionHint, cjk));
    ImGui::Separator();

    // 三个动作：进入 / 重置（仅肉鸽秘境）/ 取消。点任一即**关菜单并产生一次请求**（由 game 层取走执行）。
    if (ImGui::Button(UiText(UiLabel::PortalMenuEnter, cjk))) {
        m_portalMenuRequest = PortalMenuRequest { PortalAction::Enter, m_portalMenu.targetWorldId };
        m_portalMenuOpen    = false;
    }
    if (m_portalMenu.canReset) {
        ImGui::SameLine();
        if (ImGui::Button(UiText(UiLabel::PortalMenuReset, cjk))) {
            m_portalMenuRequest = PortalMenuRequest { PortalAction::Reset, m_portalMenu.targetWorldId };
            m_portalMenuOpen    = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(UiText(UiLabel::PortalMenuCancel, cjk))) {
        m_portalMenuRequest = PortalMenuRequest { PortalAction::Cancel, m_portalMenu.targetWorldId };
        m_portalMenuOpen    = false;
    }
    ImGui::End();
}

void DebugOverlay::BuildObjectPalette() {
    if (!m_frameActive || !m_objectPaletteOpen) {
        return;
    }
    const bool cjk = m_cjkFontLoaded;

    PaletteModel& palette = m_palette;
    // 规整一次（列表可能在打开后变化；纯函数保证不越界；与 `ClampPaletteState` 同口径）。
    if (!palette.categoryNames.empty()) {
        if (palette.state.categoryIndex >= palette.categoryNames.size()) {
            palette.state.categoryIndex = 0U;
        }
        const std::size_t typeCount = palette.typeIdsByCategory[palette.state.categoryIndex].size();
        if (typeCount == 0U) {
            palette.state.typeIndex = 0U;
        } else if (palette.state.typeIndex >= typeCount) {
            palette.state.typeIndex = typeCount - 1U;
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowBgAlpha(0.92F);
    ImGui::Begin(UiText(UiLabel::ObjectPaletteTitle, cjk), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    // 左列：仓库（一级）
    ImGui::BeginGroup();
    ImGui::TextUnformatted(UiText(UiLabel::ObjectPaletteCategoryHeader, cjk));
    ImGui::BeginChild("##palette_categories", ImVec2(180.0F, 240.0F), ImGuiChildFlags_None);
    for (std::size_t index = 0; index < palette.categoryNames.size(); ++index) {
        const bool selected = (index == palette.state.categoryIndex);
        if (ImGui::Selectable(palette.categoryNames[index].c_str(), selected)) {
            palette.state.categoryIndex = index;
            palette.state.typeIndex     = 0U;  // 换仓库 ⇒ 二级归零
        }
    }
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    // 右列：模型（二级）
    ImGui::BeginGroup();
    ImGui::TextUnformatted(UiText(UiLabel::ObjectPaletteTypeHeader, cjk));
    ImGui::BeginChild("##palette_types", ImVec2(240.0F, 240.0F), ImGuiChildFlags_None);
    if (palette.state.categoryIndex < palette.typeIdsByCategory.size()) {
        const std::vector<std::string>& typeIds = palette.typeIdsByCategory[palette.state.categoryIndex];
        for (std::size_t index = 0; index < typeIds.size(); ++index) {
            const bool selected = (index == palette.state.typeIndex);
            // V0.11：**双击**模型名 ⇒ 直接进入摆放模式（与「进入摆放」按钮等效；所有者 2026-10-07 要求）。
            if (ImGui::Selectable(typeIds[index].c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                palette.state.typeIndex = index;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_paletteRequest    = PaletteRequest { PaletteRequest::Action::EnterPlacement, typeIds[index] };
                    m_objectPaletteOpen = false;  // 关面板 → 进摆放模式（捕获由 game 层恢复）
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    // 右列：**预览**（V0.5 E4）—— CPU 正交投影 + 朗伯明暗的小图（与最终摆放**同一份**几何）。
    // 几何由 game 层算好放进 `palette.previewTriangles`（面板只画）；未拖动时由 game 层给自动旋转角。
    ImGui::BeginGroup();
    ImGui::TextUnformatted(UiText(UiLabel::ObjectPalettePreviewHeader, cjk));
    {
        const ImVec2 previewSize(kPalettePreviewWidth, kPalettePreviewHeight);
        const ImVec2 previewOrigin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##palette_preview", previewSize);
        if (ImGui::IsItemActive()) {
            // V0.11：**上下左右都能转**（左右 = yaw、上下 = pitch）；拖动期间**暂停自动旋转**
            // （game 层据 `previewDragging` 不再推进自动角）。
            constexpr float kPi    = 3.14159265358979323846F;
            constexpr float kTwoPi = 6.28318530717958647692F;
            palette.previewYawRadians += ImGui::GetIO().MouseDelta.x * kPalettePreviewDragRadiansPerPixel;
            // 回绕到 [−π, π] ⇒ 长时间拖动不会让角度无界增长（`cos/sin` 在极大角度上会丢精度）。
            if (palette.previewYawRadians > kPi) {
                palette.previewYawRadians -= kTwoPi;
            } else if (palette.previewYawRadians < -kPi) {
                palette.previewYawRadians += kTwoPi;
            }
            // 俯仰**限幅**（±≈83°）⇒ 不会翻过头。
            constexpr float kMaxPitch   = 1.45F;
            palette.previewPitchRadians = std::clamp(
                palette.previewPitchRadians + ImGui::GetIO().MouseDelta.y * kPalettePreviewDragRadiansPerPixel,
                -kMaxPitch, kMaxPitch);
            palette.previewDragging = true;
        } else if (palette.previewDragging) {
            // V0.11：**松开 ⇒ 回到初始角度**（拖动偏移归零）并**恢复自动旋转**（所有者 2026-10-07 要求）。
            palette.previewYawRadians   = 0.0F;
            palette.previewPitchRadians = 0.0F;
            palette.previewDragging     = false;
        }
        ImGui::TextDisabled("%s", UiText(UiLabel::ObjectPalettePreviewHint, cjk));

        const ImVec2  previewEnd(previewOrigin.x + previewSize.x, previewOrigin.y + previewSize.y);
        ImDrawList*   drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewOrigin, previewEnd, IM_COL32(18, 20, 24, 235), kPalettePreviewRounding);
        if (palette.previewTriangles.empty()) {
            const char*  hint     = UiText(UiLabel::ObjectPalettePreviewEmpty, cjk);
            const ImVec2 hintSize = ImGui::CalcTextSize(hint);
            drawList->AddText(ImVec2(previewOrigin.x + (previewSize.x - hintSize.x) * 0.5F,
                                     previewOrigin.y + (previewSize.y - hintSize.y) * 0.5F),
                              IM_COL32(150, 155, 165, 255), hint);
        } else {
            const auto toScreen = [&](const glm::vec2& point) {
                return ImVec2(previewOrigin.x + point.x * previewSize.x, previewOrigin.y + point.y * previewSize.y);
            };
            for (const vx::PreviewTriangle& triangle : palette.previewTriangles) {
                const float  shade = std::clamp(triangle.shade, 0.0F, 1.0F);
                const int    level = static_cast<int>(shade * 255.0F + 0.5F);
                // 轻微冷调（B 略降）⇒ 与暗色主题协调；明暗由 `shade` 单独承担。
                const ImU32  color = IM_COL32(level, level, static_cast<int>(static_cast<float>(level) * 0.94F), 255);
                drawList->AddTriangleFilled(toScreen(triangle.a), toScreen(triangle.b), toScreen(triangle.c), color);
            }
        }
        drawList->AddRect(previewOrigin, previewEnd, IM_COL32(90, 95, 105, 255), kPalettePreviewRounding);
    }
    ImGui::EndGroup();

    // 当前选中的类型 id（按钮的"名义承诺"依据；空 = 二级列表为空 ⇒ 禁「进入」）。
    std::string selectedTypeId;
    if (palette.state.categoryIndex < palette.typeIdsByCategory.size()) {
        const std::vector<std::string>& typeIds = palette.typeIdsByCategory[palette.state.categoryIndex];
        if (palette.state.typeIndex < typeIds.size()) {
            selectedTypeId = typeIds[palette.state.typeIndex];
        }
    }

    ImGui::Separator();
    // V0.11（SKILL《运行期可修改优先》）：**摆放控制** —— 状态 + 键位显示。
    // 放在**其所属界面**（摆放功能的物件选择器），**不**塞进 F1 调试面板（见 SKILL「界面归属」）。
    // 本面板只读：**改**在运行期按键（`Z` / `X` / `B`），符合"界面上不存在点了没反应的无效控件"。
    ImGui::SeparatorText(UiText(UiLabel::SectionPlacementControls, cjk));
    ImGui::Text(UiText(UiLabel::PlacementRotateHoldFormat, cjk),
                UiText(palette.rotateHoldEnabled ? UiLabel::ValueYes : UiLabel::ValueNo, cjk));
    ImGui::Text(UiText(UiLabel::PlacementNeighborSnapFormat, cjk),
                UiText(palette.neighborSnapEnabled ? UiLabel::ValueYes : UiLabel::ValueNo, cjk));
    ImGui::Text(UiText(UiLabel::PlacementGridSnapFormat, cjk),
                UiText(palette.gridSnapEnabled ? UiLabel::ValueYes : UiLabel::ValueNo, cjk));
    // V0.10 / S9：**未保存改动数**（`0` = 不占一行）—— 让"还没写盘"在选择器里**可见**，
    // 与下方「保存全部到可编辑层」按钮构成明确的"存什么 / 还有多少没存"。
    if (palette.unsavedChanges > 0) {
        ImGui::Text(UiText(UiLabel::ObjectPaletteUnsavedFormat, cjk), palette.unsavedChanges);
    }
    ImGui::BeginDisabled(selectedTypeId.empty());
    if (ImGui::Button(UiText(UiLabel::ObjectPaletteEnter, cjk))) {
        m_paletteRequest    = PaletteRequest { PaletteRequest::Action::EnterPlacement, selectedTypeId };
        m_objectPaletteOpen = false;  // 关面板 → 进摆放模式（捕获由 game 层恢复）
    }
    ImGui::EndDisabled();
    // V0.11 / I4 修订：**修改模式**按钮（不需先选类型 —— 它是"选中已有物件并拖动"的入口）。
    ImGui::SameLine();
    if (ImGui::Button(UiText(UiLabel::ObjectPaletteModify, cjk))) {
        m_paletteRequest    = PaletteRequest { PaletteRequest::Action::EnterModify, std::string {} };
        m_objectPaletteOpen = false;  // 关面板 → 进修改模式（捕获由 game 层恢复）
    }
    ImGui::SameLine();
    // V0.11 / A8：`F3` 让位给「修改模式」⇒「**重复上次**」改由本按钮触发（能力不删、只换入口）。
    if (ImGui::Button(UiText(UiLabel::ObjectPaletteRepeatLast, cjk))) {
        m_paletteRequest = PaletteRequest { PaletteRequest::Action::RepeatLast, std::string {} };
    }
    ImGui::SameLine();
    if (ImGui::Button(UiText(UiLabel::ObjectPaletteSave, cjk))) {
        m_paletteRequest = PaletteRequest { PaletteRequest::Action::Save, std::string {} };
    }
    ImGui::SameLine();
    if (ImGui::Button(UiText(UiLabel::ObjectPaletteCancel, cjk))) {
        m_paletteRequest    = PaletteRequest { PaletteRequest::Action::Cancel, std::string {} };
        m_objectPaletteOpen = false;
    }

    ImGui::End();
}

void DebugOverlay::BuildUI(const DebugStats& stats, SystemPanelContext& panelContext) {
    if (!m_frameActive) {
        return;
    }

    // T16：标签语言由构造期字体解析结果决定，两个面板共用同一个开关值。
    const bool cjk = m_cjkFontLoaded;

    m_systemPanel.Build(panelContext, cjk);

    // V9：传送门菜单（居中模态）。放在 `!m_visible` 早退**之前** —— 它不依赖 F1 面板是否显示。
    BuildPortalMenu();

    // V0.5 E3：物件选择器（居中模态二级列表）。同样放在 `!m_visible` 早退**之前**。
    BuildObjectPalette();

    // V0.11 / I4 修订：**修改模式的模式指示器**（只读叠加；不参与输入抑制、不影响玩法）。
    // 为什么要它（所有者 2026-10-08）：摆放与"修改（选中并拖动）"是**两种模式**，屏幕上需要一眼看清当前在哪一种。
    // V0.11 / A8h（所有者 2026-10-09）：① **文案从屏幕正中移到右上角**（原来压住操作）；
    //   ② 文案**加背景色 + 同色边框**（亮底 / 暗底都看得清）。
    // V0.11 / A8i（所有者 2026-10-09）：③ **去掉屏幕中心图标** —— 修改模式是**自由光标**（看到哪就点到哪），
    //   屏幕正中那枚十字已无实际含义（不是准星、也不指示拾取点）⇒ 只保留右上角的模式文案。
    if (m_modifyActive) {
        ImDrawList*    foreground = ImGui::GetForegroundDrawList();
        const ImGuiIO& io         = ImGui::GetIO();
        const ImU32    glow = m_modifyHasSelection ? IM_COL32(120, 230, 120, 235) : IM_COL32(245, 245, 245, 235);
        const char*  text = UiText(m_modifyHasSelection ? UiLabel::ModifySelectedIndicator : UiLabel::ModifyModeIndicator, cjk);
        const ImVec2 size = ImGui::CalcTextSize(text);
        const float  padX = 10.0F;
        const float  padY = 6.0F;
        const ImVec2 textPos(io.DisplaySize.x - size.x - padX - 12.0F, 12.0F + padY);
        const ImVec2 boxMin(textPos.x - padX, textPos.y - padY);
        const ImVec2 boxMax(textPos.x + size.x + padX, textPos.y + size.y + padY);
        foreground->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 175), 6.0F);
        foreground->AddRect(boxMin, boxMax, glow, 6.0F, 0, 1.5F);
        foreground->AddText(textPos, glow, text);
    }

    // 常驻坐标 HUD（屏幕左上角，只读）：与 F1 面板相互独立，不需要开面板就能看到当前位置。
    if (m_hudVisible) {
        BuildHud(stats);
    }

    if (!m_visible) {
        return;
    }

    PushFrameTime(stats.frameSeconds);

    const double frameMs = stats.frameSeconds * 1000.0;
    const double p50Ms   = Percentile(0.50) * 1000.0;
    const double p95Ms   = Percentile(0.95) * 1000.0;
    const double p99Ms   = Percentile(0.99) * 1000.0;  // T38：尾部（hitch）判据，见 references/performance-and-hitches.md
    const double fps     = (stats.frameSeconds > 0.0) ? (1.0 / stats.frameSeconds) : 0.0;

    // 让 F1 面板初始位置落在常驻 HUD 下方，避免两者在左上角重叠（之后仍可由用户拖动）。
    // `m_hudHeight` 首帧可能尚未测出 ⇒ 用下界兜底，保证至少不压住 HUD。
    const float hudBottom = 8.0F + std::max(m_hudHeight, 46.0F) + 4.0F;
    ImGui::SetNextWindowPos(ImVec2(8.0F, m_hudVisible ? hudBottom : 8.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.88F);
    ImGui::Begin(UiText(UiLabel::DebugPanelTitle, cjk), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    // T85：测试模式横幅（**置顶、只读**）——由启动参数决定，全运行期不变，供所有者一眼判断
    // "该不该动键鼠 / 本次要人工确认什么"。动态文本不经标签缝，故在无 CJK 字体时**不渲染非 ASCII 项**
    // （沿用"面板上绝不出现缺字 `?`"的口径，见 ui_text.hpp）。
    ImGui::SeparatorText(UiText(UiLabel::SectionTestMode, cjk));
    if (m_testMode.mode == TestMode::Auto) {
        ImGui::TextColored(ImVec4(1.0F, 0.78F, 0.25F, 1.0F), "%s", UiText(UiLabel::TestModeAuto, cjk));
    } else {
        ImGui::TextColored(ImVec4(0.45F, 0.85F, 1.0F, 1.0F), "%s", UiText(UiLabel::TestModeManual, cjk));
        if (m_testMode.manualItems.empty()) {
            ImGui::TextUnformatted(UiText(UiLabel::TestModeNoItems, cjk));
        } else {
            for (const std::string& item : m_testMode.manualItems) {
                if (cjk || IsAsciiOnly(item.c_str())) {
                    ImGui::BulletText("%s", item.c_str());
                } else {
                    ImGui::BulletText("%s", UiText(UiLabel::TestModeItemNonAscii, cjk));
                    break;
                }
            }
        }
    }

    ImGui::SeparatorText(UiText(UiLabel::SectionTiming, cjk));
    StatRow(UiText(UiLabel::FrameTime, cjk), UiText(UiLabel::FrameTimeFormat, cjk), frameMs, p50Ms, p95Ms, p99Ms);
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
            stats.carvedBlockCount, stats.volumePendingActions, stats.volumeKeptDirtyCount);
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
    StatRow(UiText(UiLabel::MeshVram, cjk), UiText(UiLabel::TextureVramFormat, cjk),
            static_cast<double>(stats.meshBytes) / (1024.0 * 1024.0));

    // T24：CPU 帧时间分解（毫秒，显示到 0.01 ms）——由 main 用单调计时分别测量。
    ImGui::SeparatorText(UiText(UiLabel::SectionCpuFrameTime, cjk));
    StatRow(UiText(UiLabel::CpuLogicStep, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuLogicMs);
    StatRow(UiText(UiLabel::CpuUiBuild, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuUiMs);
    StatRow(UiText(UiLabel::CpuRenderSubmit, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.cpuRenderMs);
    // T38：单独列出"等交换链"耗时 —— 它混在渲染提交里会把"在空等 GPU"误判成"CPU 忙"。
    StatRow(UiText(UiLabel::SwapchainWait, cjk), UiText(UiLabel::MillisecondsFormat, cjk), stats.swapchainWaitMs);

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
