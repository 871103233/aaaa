#pragma once

#include "terrain/world_bounds.hpp"

#include <glm/vec3.hpp>

namespace vx {

/// 出界救援余量（格）：角色位置超出边界盒**这么多**才判定为"掉出世界"。
///
/// 取 16 格，使墙外的浅层越界（例如贴着墙角外侧）不会立刻被传送，而真正的坠落与远距离飞离必然触发。
/// **T84（2026-10-05）起本救援降为纯兜底**：边界已**六面封闭**（四周不可见墙 + 不可见顶盖），
/// 正常玩法下不会越界；本机制只防边界漏洞与意外坠落（与边界墙分工：墙负责"挡住"，本机制负责"兜底"）。
inline constexpr double kOutOfBoundsMargin = 16.0;

/// 纯函数：角色是否已掉出世界（需要救援）。
///
/// 规则：水平方向超出边界盒 `margin` 之外，或竖直方向**高出上沿 / 低于下沿** `margin` 之外。
///
/// 历史口径更正（T84）：此前**不**判定"高于上沿"，因为飞行模式允许升到边界盒之上；T84 加了顶盖后
/// 该前提不再成立 ⇒ 上沿同样纳入判定（否则"从上方逃逸"这一类漏洞无法被兜底）。
///
/// `margin <= 0` 时按 0 处理。纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline bool IsCharacterOutOfBounds(const glm::dvec3& position, const WorldBounds& bounds,
                                                 double margin) noexcept {
    const double safeMargin = (margin > 0.0) ? margin : 0.0;
    if (position.y < bounds.min.y - safeMargin || position.y > bounds.max.y + safeMargin) {
        return true;
    }
    if (position.x < bounds.min.x - safeMargin || position.x > bounds.max.x + safeMargin) {
        return true;
    }
    if (position.z < bounds.min.z - safeMargin || position.z > bounds.max.z + safeMargin) {
        return true;
    }
    return false;
}

}  // namespace vx
