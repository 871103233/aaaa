#pragma once

namespace vx {

/// 应用统一的暗色游戏 UI 主题（T16）。
///
/// 这是工程内**唯一**修改 ImGui 样式的地方：所有颜色、圆角、留白、边框、字号缩放都集中在此，
/// 面板代码里**不得**再散落 `ImGui::PushStyleColor` / 直接改 `GetStyle()` 的调用。
/// 前置条件：ImGui 上下文已创建、且在第一帧之前调用一次；线程约定：仅在渲染线程调用。
void ApplyUiTheme();

}  // namespace vx
