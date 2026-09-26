#include "system_panel.hpp"

#include <imgui.h>

#include <cfloat>
#include <cstddef>
#include <cstdio>

namespace vx {
namespace {

/// 系统面板固定尺寸（像素）：打开时按此尺寸居中，退出区锚定在底部。
constexpr float kPanelWidth  = 460.0F;
constexpr float kPanelHeight = 540.0F;

/// 把尺寸格式化为下拉项文本（ASCII `x`，避免依赖字体的乘号字形）。
void FormatSize(char* buffer, std::size_t size, int width, int height) {
    std::snprintf(buffer, size, "%d x %d", width, height);
}

}  // namespace

void SystemPanel::Build(SystemPanelContext& context, bool cjkLabels) {
    if (!m_open) {
        return;
    }

    // 居中模态窗口：固定宽高，每帧重新居中以跟随窗口尺寸变化。
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5F, io.DisplaySize.y * 0.5F), ImGuiCond_Always,
                            ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97F);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(UiText(UiLabel::SystemPanelTitle, cjkLabels), nullptr, flags)) {
        const ImGuiStyle& style = ImGui::GetStyle();

        // ---- 显示 ----
        ImGui::SeparatorText(UiText(UiLabel::SectionDisplay, cjkLabels));
        ImGui::TextUnformatted(UiText(UiLabel::DisplayMode, cjkLabels));

        int mode = static_cast<int>(context.settings.displayMode);
        if (ImGui::RadioButton(UiText(UiLabel::ModeWindowed, cjkLabels), &mode,
                               static_cast<int>(DisplayMode::Windowed))) {
            context.settings.displayMode = DisplayMode::Windowed;
            context.displayModeChanged   = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(UiText(UiLabel::ModeFullscreen, cjkLabels), &mode,
                               static_cast<int>(DisplayMode::Fullscreen))) {
            context.settings.displayMode = DisplayMode::Fullscreen;
            context.displayModeChanged   = true;
        }

        // 分辨率：仅窗口模式下可用；全屏时由显示器决定，控件置灰。
        ImGui::TextUnformatted(UiText(UiLabel::Resolution, cjkLabels));
        const bool resolutionEditable = IsResolutionEditable(context.settings.displayMode);
        if (!resolutionEditable) {
            ImGui::BeginDisabled();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);

        char currentSize[32];
        FormatSize(currentSize, sizeof(currentSize), context.settings.windowWidth, context.settings.windowHeight);

        int currentIndex = -1;
        if (context.resolutions != nullptr) {
            for (std::size_t i = 0; i < context.resolutions->size(); ++i) {
                if ((*context.resolutions)[i].width == context.settings.windowWidth &&
                    (*context.resolutions)[i].height == context.settings.windowHeight) {
                    currentIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        if (ImGui::BeginCombo("##resolution", currentSize)) {
            if (context.resolutions != nullptr) {
                for (std::size_t i = 0; i < context.resolutions->size(); ++i) {
                    const DisplaySize size = (*context.resolutions)[i];
                    char              label[32];
                    FormatSize(label, sizeof(label), size.width, size.height);
                    const bool selected = (static_cast<int>(i) == currentIndex);
                    if (ImGui::Selectable(label, selected)) {
                        context.settings.windowWidth  = size.width;
                        context.settings.windowHeight = size.height;
                        context.resolutionChanged     = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (!resolutionEditable) {
            ImGui::EndDisabled();
        }

        // ---- 声音 ----
        ImGui::SeparatorText(UiText(UiLabel::SectionAudio, cjkLabels));
        ImGui::TextUnformatted(UiText(UiLabel::Volume, cjkLabels));
        ImGui::SetNextItemWidth(-FLT_MIN);

        // 音量：0–100；拖动过程中实时写入设置，"结束编辑"时提交（应用增益 + 落盘）。
        int volume = context.settings.masterVolume;
        if (ImGui::SliderInt("##volume", &volume, kMasterVolumeMin, kMasterVolumeMax,
                             UiText(UiLabel::VolumeFormat, cjkLabels))) {
            context.settings.masterVolume = ClampMasterVolume(volume);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            context.volumeCommitted = true;
        }

        // ---- 性能 ----
        ImGui::SeparatorText(UiText(UiLabel::SectionPerformance, cjkLabels));
        ImGui::TextUnformatted(UiText(UiLabel::FrameRateCap, cjkLabels));
        ImGui::SetNextItemWidth(-FLT_MIN);

        // 帧率上限：范围 60 ~ 当前显示器刷新率，默认取刷新率（T17）。
        // 上界至少为 60，保证"60 Hz 显示器 / 刷新率未知"时区间非空。
        const int capMax = (context.displayRefreshRate > kFrameRateCapMin) ? context.displayRefreshRate
                                                                          : kFrameRateCapMin;
        int frameRateCap = context.settings.frameRateCap;
        if (ImGui::SliderInt("##frame_rate_cap", &frameRateCap, kFrameRateCapMin, capMax,
                             UiText(UiLabel::FrameRateCapFormat, cjkLabels))) {
            // 拖动过程中实时写入并立即应用（目标 == 刷新率 → 垂直同步；低于 → 睡眠限帧）。
            context.settings.frameRateCap = ClampFrameRateCap(frameRateCap, context.displayRefreshRate);
            context.frameRateCapChanged   = true;
        }

        // ---- 退出：整块锚定到内容区底部，按钮占满整行宽度 ----
        const float buttonHeight = ImGui::GetFrameHeight();
        const float quitBlock    = ImGui::GetTextLineHeightWithSpacing() + buttonHeight + style.ItemSpacing.y;
        const float remaining    = ImGui::GetContentRegionAvail().y;
        if (remaining > quitBlock) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (remaining - quitBlock));
        }
        ImGui::SeparatorText(UiText(UiLabel::SectionQuit, cjkLabels));
        if (ImGui::Button(UiText(UiLabel::QuitGame, cjkLabels), ImVec2(-FLT_MIN, buttonHeight))) {
            context.requestQuit = true;
        }
    }
    ImGui::End();
}

}  // namespace vx
