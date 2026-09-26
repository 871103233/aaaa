#pragma once

#include "terrain/world_bounds.hpp"

#include <glm/vec3.hpp>

namespace vx {

/// 出界救援余量（格）：角色位置超出边界盒**这么多**才判定为"掉出世界"。
///
/// 取 16 格，使墙外的浅层越界（例如刚从墙上缘跌出、或贴着墙角外侧）不会立刻被传送，
/// 而真正的坠落与远距离飞离必然触发。与边界墙分工：墙挡住地面行走，本救援兜住
/// "飞越墙后坠落"这类墙够不到的情形。
inline constexpr double kOutOfBoundsMargin = 16.0;

/// 纯函数：角色是否已掉出世界（需要救援）。
///
/// 规则：水平方向超出边界盒 `margin` 之外，或**竖直方向低于**边界盒下沿 `margin` 之外。
/// **不**对"高于上沿"判定——飞行模式允许升到边界盒之上，那不是掉出世界。
///
/// `margin <= 0` 时按 0 处理。纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline bool IsCharacterOutOfBounds(const glm::dvec3& position, const WorldBounds& bounds,
                                                 double margin) noexcept {
    const double safeMargin = (margin > 0.0) ? margin : 0.0;
    if (position.y < bounds.min.y - safeMargin) {
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
