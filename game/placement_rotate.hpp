#pragma once

#include <algorithm>
#include <cmath>

namespace vx {

/// **摆放模式的旋转手感**（V0.11 / I1b；[ADR 0038](../../docs/adr/0038-construction-editor-and-runtime-separation.md) 决策四的"旋转"）
/// —— **纯函数、header-only**：编辑器与运行时"游戏内建造"**共用**（与 UI / GPU 无关）。
///
/// 口径（**可判定**）：
///   - **点按 = 恰好一步**（`stepDegrees`）；
///   - **长按 = 按速率连续转**（`rateDegPerSec`，度/秒）；速率 `<= 0` ⇒ **关闭长按连续转**（只有点按有效）；
///   - **长按启动延迟**（`holdDelaySeconds`，缺省 **0.4 s**）：按下后须**连续按住这么久**才**开始**连续转
///     ⇒ **轻点（< 0.4 s）恰好一步、不漂**（避免"单击也带一点连续转"污染手感）；判定见 `PlacementHoldActive`。
///   - **单帧计入上限**（`maxFrameSeconds`）：卡顿 / 掉帧后不会一次跳很大一段（防空转）。
///
/// 业界参照（点名）：UE5 / Unity 编辑器 gizmo 的"按住持续旋转"、Valheim 建造旋转（按住连续转）、Blender / Godot 拖拽旋转
/// —— 共同口径 = **点按一步、按住连续**（并常带一个"按住启动延迟"以区分点击与长按）。
struct PlacementRotateSettings {
    double stepDegrees      = 1.0;   ///< 点按一次的角度（`Shift` 时由调用方改为 90°）；**V0.11（2026-10-08）：缺省 15 → 1**（精细调整）
    double rateDegPerSec    = 90.0;  ///< 长按的角速度（度/秒）；`<= 0` = 只点按。**V0.11（2026-10-08）：缺省 180 → 90**（所有者要求降为一半）
    double holdDelaySeconds = 0.4;   ///< **长按启动延迟**（秒）：按住超过该时长才**开始**连续转。**V0.11（2026-10-08）新增；缺省 0.4**（兼顾"不污染单击"与响应速度）
    double maxFrameSeconds  = 0.1;   ///< 单帧计入的时长上限（防卡顿后突跳）
};

/// **长按是否已"生效"**（V0.11，2026-10-08）：`held` 且已连续按住 `heldSeconds` 秒、且 **≥ `holdDelaySeconds`**。
/// `heldSeconds` 由调用方**逐帧累加**（未按住 ⇒ 传 0）。未按住 ⇒ 恒 `false`（不会因 `holdDelaySeconds = 0` 误判）。
[[nodiscard]] inline bool PlacementHoldActive(bool held, double heldSeconds,
                                              const PlacementRotateSettings& settings) noexcept {
    return held && heldSeconds >= settings.holdDelaySeconds;
}

/// 每帧的**朝向增量**（度）：**点按优先**（当帧有按下边沿 ⇒ 恰好走一步），否则**长按已生效**（`holdActiveLeft/Right`，
/// 见 `PlacementHoldActive`）⇒ 按速率连续转。
/// 左 = 负、右 = 正；两侧同时点按 ⇒ 抵消为 0（可判定）。`dtSeconds` 会被钳到 `[0, maxFrameSeconds]`。
[[nodiscard]] inline double PlacementRotationDeltaDegrees(bool pressedLeft, bool holdActiveLeft, bool pressedRight,
                                                          bool holdActiveRight, double dtSeconds,
                                                          const PlacementRotateSettings& settings) noexcept {
    const double clampedSeconds =
        std::min(std::max(dtSeconds, 0.0), std::max(settings.maxFrameSeconds, 0.0));
    const double heldStep = (settings.rateDegPerSec > 0.0) ? settings.rateDegPerSec * clampedSeconds : 0.0;

    double delta = 0.0;
    if (pressedLeft) {
        delta -= settings.stepDegrees;  // 点按 = 一步（当帧不再叠加按住量 ⇒ 点按恰好一步）
    } else if (holdActiveLeft) {
        delta -= heldStep;
    }
    if (pressedRight) {
        delta += settings.stepDegrees;
    } else if (holdActiveRight) {
        delta += heldStep;
    }
    return delta;
}

}  // namespace vx
