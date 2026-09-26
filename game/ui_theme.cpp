#include "ui_theme.hpp"

#include <imgui.h>

namespace vx {
namespace {

/// 主题基色（暗色板岩 / 青蓝强调）：所有面板共用同一套，保证两个面板视觉一致。
constexpr float kBackgroundR = 0.10F;  ///< 底色 R
constexpr float kBackgroundG = 0.12F;  ///< 底色 G
constexpr float kBackgroundB = 0.15F;  ///< 底色 B
constexpr float kAccentR     = 0.36F;  ///< 强调色 R
constexpr float kAccentG     = 0.64F;  ///< 强调色 G
constexpr float kAccentB     = 0.96F;  ///< 强调色 B

[[nodiscard]] constexpr ImVec4 Rgba(float red, float green, float blue, float alpha) {
    return ImVec4(red, green, blue, alpha);
}

}  // namespace

void ApplyUiTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // ---- 尺寸：留白、圆角、边框、字号缩放 ----
    style.WindowPadding          = ImVec2(16.0F, 16.0F);
    style.FramePadding           = ImVec2(12.0F, 7.0F);
    style.CellPadding            = ImVec2(10.0F, 5.0F);
    style.ItemSpacing            = ImVec2(12.0F, 9.0F);
    style.ItemInnerSpacing       = ImVec2(8.0F, 6.0F);
    style.IndentSpacing          = 24.0F;
    style.ScrollbarSize          = 13.0F;
    style.GrabMinSize            = 13.0F;
    style.WindowBorderSize       = 1.0F;
    style.ChildBorderSize        = 1.0F;
    style.PopupBorderSize        = 1.0F;
    style.FrameBorderSize        = 1.0F;
    style.WindowRounding         = 9.0F;
    style.ChildRounding          = 7.0F;
    style.FrameRounding          = 6.0F;
    style.PopupRounding          = 7.0F;
    style.ScrollbarRounding      = 9.0F;
    style.GrabRounding           = 6.0F;
    style.TabRounding            = 6.0F;
    style.WindowTitleAlign       = ImVec2(0.5F, 0.5F);  ///< 居中标题
    style.SeparatorTextBorderSize = 2.0F;
    style.SeparatorTextAlign     = ImVec2(0.0F, 0.5F);
    style.SeparatorTextPadding   = ImVec2(14.0F, 7.0F);
    style.AntiAliasedLines       = true;
    style.AntiAliasedFill        = true;
    // 字号缩放保持 1.0：可读字号来自 `ApplyUiFont` 加载的 18 px 字体，不在这里二次放大。
    style.FontScaleMain = 1.0F;

    // ---- 颜色 ----
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text]         = Rgba(0.90F, 0.92F, 0.96F, 1.00F);
    colors[ImGuiCol_TextDisabled] = Rgba(0.48F, 0.52F, 0.58F, 1.00F);

    colors[ImGuiCol_WindowBg] = Rgba(kBackgroundR, kBackgroundG, kBackgroundB, 0.96F);
    colors[ImGuiCol_ChildBg]  = Rgba(kBackgroundR + 0.02F, kBackgroundG + 0.02F, kBackgroundB + 0.03F, 0.90F);
    colors[ImGuiCol_PopupBg]  = Rgba(kBackgroundR + 0.02F, kBackgroundG + 0.02F, kBackgroundB + 0.03F, 0.98F);

    colors[ImGuiCol_Border]       = Rgba(0.30F, 0.34F, 0.42F, 0.85F);
    colors[ImGuiCol_BorderShadow] = Rgba(0.00F, 0.00F, 0.00F, 0.00F);

    colors[ImGuiCol_FrameBg]        = Rgba(0.16F, 0.19F, 0.24F, 1.00F);
    colors[ImGuiCol_FrameBgHovered] = Rgba(0.22F, 0.27F, 0.34F, 1.00F);
    colors[ImGuiCol_FrameBgActive]  = Rgba(0.27F, 0.34F, 0.44F, 1.00F);

    colors[ImGuiCol_TitleBg]          = Rgba(0.08F, 0.10F, 0.13F, 1.00F);
    colors[ImGuiCol_TitleBgActive]    = Rgba(0.15F, 0.21F, 0.29F, 1.00F);
    colors[ImGuiCol_TitleBgCollapsed] = Rgba(0.08F, 0.10F, 0.13F, 0.85F);
    colors[ImGuiCol_MenuBarBg]        = Rgba(0.12F, 0.14F, 0.18F, 1.00F);

    colors[ImGuiCol_ScrollbarBg]          = Rgba(0.09F, 0.11F, 0.14F, 1.00F);
    colors[ImGuiCol_ScrollbarGrab]        = Rgba(0.30F, 0.35F, 0.43F, 1.00F);
    colors[ImGuiCol_ScrollbarGrabHovered] = Rgba(0.38F, 0.45F, 0.54F, 1.00F);
    colors[ImGuiCol_ScrollbarGrabActive]  = Rgba(0.46F, 0.55F, 0.66F, 1.00F);

    colors[ImGuiCol_CheckMark]              = Rgba(kAccentR, kAccentG, kAccentB, 1.00F);
    colors[ImGuiCol_SliderGrab]             = Rgba(0.32F, 0.56F, 0.88F, 1.00F);
    colors[ImGuiCol_SliderGrabActive]       = Rgba(kAccentR, kAccentG, kAccentB, 1.00F);
    colors[ImGuiCol_Button]                 = Rgba(0.20F, 0.26F, 0.34F, 1.00F);
    colors[ImGuiCol_ButtonHovered]          = Rgba(0.27F, 0.37F, 0.49F, 1.00F);
    colors[ImGuiCol_ButtonActive]           = Rgba(0.33F, 0.47F, 0.63F, 1.00F);
    colors[ImGuiCol_Header]                 = Rgba(0.22F, 0.30F, 0.40F, 1.00F);
    colors[ImGuiCol_HeaderHovered]          = Rgba(0.27F, 0.37F, 0.49F, 1.00F);
    colors[ImGuiCol_HeaderActive]           = Rgba(0.33F, 0.47F, 0.63F, 1.00F);

    colors[ImGuiCol_Separator]        = Rgba(0.32F, 0.36F, 0.43F, 0.60F);
    colors[ImGuiCol_SeparatorHovered] = Rgba(0.40F, 0.56F, 0.76F, 0.78F);
    colors[ImGuiCol_SeparatorActive]  = Rgba(0.50F, 0.68F, 0.92F, 1.00F);

    colors[ImGuiCol_ResizeGrip]        = Rgba(0.32F, 0.36F, 0.43F, 0.30F);
    colors[ImGuiCol_ResizeGripHovered] = Rgba(kAccentR, kAccentG, kAccentB, 0.60F);
    colors[ImGuiCol_ResizeGripActive]  = Rgba(kAccentR, kAccentG, kAccentB, 0.90F);

    colors[ImGuiCol_Tab]                = Rgba(0.15F, 0.19F, 0.25F, 1.00F);
    colors[ImGuiCol_TabHovered]         = Rgba(0.27F, 0.37F, 0.49F, 1.00F);
    colors[ImGuiCol_TabSelected]        = Rgba(0.22F, 0.30F, 0.40F, 1.00F);
    colors[ImGuiCol_TabSelectedOverline] = Rgba(kAccentR, kAccentG, kAccentB, 1.00F);
    colors[ImGuiCol_TabDimmed]          = Rgba(0.12F, 0.15F, 0.19F, 1.00F);
    colors[ImGuiCol_TabDimmedSelected]  = Rgba(0.18F, 0.24F, 0.32F, 1.00F);

    colors[ImGuiCol_PlotLines]            = Rgba(0.62F, 0.78F, 0.98F, 1.00F);
    colors[ImGuiCol_PlotLinesHovered]     = Rgba(kAccentR, kAccentG, kAccentB, 1.00F);
    colors[ImGuiCol_PlotHistogram]        = Rgba(0.50F, 0.72F, 0.40F, 1.00F);
    colors[ImGuiCol_PlotHistogramHovered] = Rgba(0.62F, 0.86F, 0.48F, 1.00F);

    colors[ImGuiCol_TableHeaderBg]     = Rgba(0.16F, 0.19F, 0.24F, 1.00F);
    colors[ImGuiCol_TableBorderStrong] = Rgba(0.30F, 0.34F, 0.42F, 1.00F);
    colors[ImGuiCol_TableBorderLight]  = Rgba(0.22F, 0.26F, 0.32F, 1.00F);
    colors[ImGuiCol_TableRowBg]        = Rgba(0.00F, 0.00F, 0.00F, 0.00F);
    colors[ImGuiCol_TableRowBgAlt]     = Rgba(1.00F, 1.00F, 1.00F, 0.03F);

    colors[ImGuiCol_TextSelectedBg]        = Rgba(kAccentR, kAccentG, kAccentB, 0.35F);
    colors[ImGuiCol_DragDropTarget]        = Rgba(kAccentR, kAccentG, kAccentB, 0.90F);
    colors[ImGuiCol_NavCursor]             = Rgba(kAccentR, kAccentG, kAccentB, 1.00F);
    colors[ImGuiCol_NavWindowingHighlight] = Rgba(1.00F, 1.00F, 1.00F, 0.70F);
    colors[ImGuiCol_NavWindowingDimBg]     = Rgba(0.05F, 0.06F, 0.08F, 0.60F);
    colors[ImGuiCol_ModalWindowDimBg]      = Rgba(0.05F, 0.06F, 0.08F, 0.62F);
}

}  // namespace vx
