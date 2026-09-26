#pragma once

#include <array>
#include <cstddef>

namespace vx {

/// UI 标签的**唯一**枚举（T16）。
///
/// 两个面板的每一段文字——包括窗口标题、分区标题、控件标签与数值格式串——都必须经
/// [`UiText()`](#UiText) 取值。这样做的目的：**标签语言由"是否加载到 CJK 字体"决定**，
/// 加载到就显示中文，没加载到就整表回退纯 ASCII 英文，于是**任何机器上都不会出现缺字 `?`**。
///
/// 新增标签的流程（缺一项测试即失败，见 `tests/ui_text_test.cpp`）：
///   1. 在本枚举里加一项（放在 `kCount` 之前）；
///   2. 在两个标签表 `kUiLabelsEnglish` / `kUiLabelsChinese` 的**同一位置**各加一项；
///   3. 面板里通过 `UiText(...)` 使用，**不得**直接写字符串字面量。
enum class UiLabel : int {
    // ---- 系统面板（Esc）----
    SystemPanelTitle = 0,  ///< 面板标题
    SectionDisplay,        ///< 分区：显示
    DisplayMode,           ///< 控件：显示模式
    ModeWindowed,          ///< 选项：窗口
    ModeFullscreen,        ///< 选项：全屏
    Resolution,            ///< 控件：分辨率
    SectionAudio,          ///< 分区：声音
    Volume,                ///< 控件：主音量
    VolumeFormat,          ///< 主音量数值格式（`%d / 100`）
    SectionPerformance,    ///< 分区：性能（T17）
    FrameRateCap,          ///< 控件：帧率上限
    FrameRateCapFormat,    ///< 帧率上限数值格式（`%d Hz`）
    SectionQuit,           ///< 分区：退出
    QuitGame,              ///< 按钮：退出游戏

    // ---- 调试面板（F1，只读）----
    DebugPanelTitle,              ///< 面板标题
    SectionTiming,                ///< 分区：时间步
    FrameTime,                    ///< 行标签：帧时间
    FrameTimeFormat,              ///< 帧时间数值格式
    Fps,                          ///< 行标签：FPS
    FpsFormat,                    ///< FPS 数值格式
    FixedSteps,                   ///< 行标签：本帧固定步
    FixedStepsFormat,             ///< 固定步数值格式
    SectionCharacter,             ///< 分区：角色
    Position,                     ///< 行标签：位置
    PositionFormat,               ///< 位置数值格式
    Cell,                         ///< 行标签：所在格
    CellFormat,                   ///< 所在格数值格式
    State,                        ///< 行标签：状态
    StateFormat,                  ///< 状态数值格式
    ValueYes,                     ///< 取值：是
    ValueNo,                      ///< 取值：否
    ValueReady,                   ///< 取值：就绪
    ValueNotReady,                ///< 取值：未就绪
    SectionCameraBrush,           ///< 分区：相机 / 笔刷
    CameraOrientation,            ///< 行标签：朝向
    CameraOrientationFormat,      ///< 朝向数值格式
    MouseCapture,                 ///< 行标签：鼠标捕获
    MouseCaptureOn,               ///< 取值：捕获开
    MouseCaptureOff,              ///< 取值：捕获关
    BrushRadius,                  ///< 行标签：笔刷半径
    BrushRadiusFormat,            ///< 笔刷半径数值格式
    SectionWorldPhysics,          ///< 分区：世界 / 物理
    LoadedTiles,                  ///< 行标签：已加载 tile
    DirtyTiles,                   ///< 行标签：上次弄脏 tile
    TileBodies,                   ///< 行标签：地表碰撞体 tile
    CountFormat,                  ///< 计数数值格式（`%zu`）

    kCount  ///< 标签总数（必须保持在最后）
};

/// 标签总数（`kUiLabelCount` 同时是两张表的固定长度）。
inline constexpr int kUiLabelCount = static_cast<int>(UiLabel::kCount);

/// 英文标签表：**必须全部为纯 ASCII**，作为"没有 CJK 字体"时的回退文案。
///
/// `tests/ui_text_test.cpp` 会逐项断言本表每个字符串都是纯 ASCII；这也是"面板上不会出现 `?`"
/// 的可执行证据。
inline constexpr std::array<const char*, kUiLabelCount> kUiLabelsEnglish = {
    "System (Esc)",
    "Display",
    "Display mode",
    "Windowed",
    "Fullscreen",
    "Resolution (windowed only)",
    "Audio",
    "Master volume",
    "%d / 100",
    "Performance",
    "Frame rate cap",
    "%d Hz",
    "Quit",
    "Quit Game",
    "V0.1 Debug Panel",
    "Timing",
    "Frame time",
    "%.2f ms (P50 %.2f / P95 %.2f ms)",
    "FPS",
    "%.1f",
    "Fixed steps",
    "%d (dt = %.5f s)",
    "Character",
    "Position",
    "(%.2f, %.2f, %.2f) blocks",
    "Cell",
    "(%d, %d, %d)",
    "State",
    "Grounded: %s  Physics: %s  Flying: %s",
    "yes",
    "no",
    "ready",
    "not ready",
    "Camera / Brush",
    "Orientation",
    "yaw %.1f deg  pitch %.1f deg  dist %.2f blocks",
    "Mouse capture",
    "on (relative mode; Esc release / click recapture)",
    "off (cursor visible)",
    "Brush radius",
    "%.1f blocks",
    "World / Physics",
    "Loaded tiles",
    "Dirty tiles (last)",
    "Terrain colliders",
    "%zu",
};

/// 中文标签表：仅当**成功加载 CJK 字体**时启用（此时不可能缺字）。
///
/// 纯格式串（如 `%d / 100`）在中英两表中相同，故测试允许"中文项与英文项一致"，
/// 但要求中文表中确实存在非 ASCII 项，防止整列误填成英文。
inline constexpr std::array<const char*, kUiLabelCount> kUiLabelsChinese = {
    "系统面板",
    "显示",
    "显示模式",
    "窗口",
    "全屏",
    "分辨率（仅窗口模式）",
    "声音",
    "主音量",
    "%d / 100",
    "性能",
    "帧率上限",
    "%d Hz",
    "退出",
    "退出游戏",
    "V0.1 调试面板",
    "时间步",
    "帧时间",
    "%.2f ms（P50 %.2f / P95 %.2f ms）",
    "FPS",
    "%.1f",
    "本帧固定步",
    "%d（dt = %.5f s）",
    "角色",
    "位置",
    "(%.2f, %.2f, %.2f) 格",
    "所在格",
    "(%d, %d, %d)",
    "状态",
    "着地：%s  物理：%s  飞行：%s",
    "是",
    "否",
    "就绪",
    "未就绪",
    "相机 / 笔刷",
    "朝向",
    "yaw %.1f°  pitch %.1f°  距离 %.2f 格",
    "鼠标捕获",
    "开（相对模式，Esc 释放 / 点击重捕获）",
    "关（光标可见）",
    "笔刷半径",
    "%.1f 格",
    "世界 / 物理",
    "已加载 tile",
    "上次弄脏 tile",
    "地表碰撞体 tile",
    "%zu",
};

/// 纯函数：判断字符串是否**只含 ASCII 字节**（`cjkFontAvailable = false` 时的硬约束）。
///
/// 空串视为只含 ASCII。不读全局状态、不分配内存。
[[nodiscard]] inline bool IsAsciiOnly(const char* text) noexcept {
    if (text == nullptr) {
        return true;
    }
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != 0; ++cursor) {
        if (*cursor >= 0x80U) {
            return false;
        }
    }
    return true;
}

/// 标签缝：返回 `label` 在指定语言下的文本。
///
/// `cjk` 为 true（已加载 CJK 字体）返回中文表项，为 false 返回**纯 ASCII** 英文表项。
/// 越界索引返回空串（防御性，正常调用不会发生）。
[[nodiscard]] inline const char* UiText(UiLabel label, bool cjk) noexcept {
    const int index = static_cast<int>(label);
    if (index < 0 || index >= kUiLabelCount) {
        return "";
    }
    const std::size_t offset = static_cast<std::size_t>(index);
    return cjk ? kUiLabelsChinese[offset] : kUiLabelsEnglish[offset];
}

}  // namespace vx
