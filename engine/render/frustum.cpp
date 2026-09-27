#include "render/frustum.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vx {
namespace {

/// 取矩阵的**第 `row` 行**（glm 是列主序，故行为 `m[0..3][row]`）。
[[nodiscard]] glm::vec4 MatrixRow(const glm::mat4& matrix, int row) noexcept {
    return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
}

/// 归一化平面（`xyz` 为单位法线）；退化（长度近零）时返回**永不剔除**的平面。
///
/// 为什么退化时取 `(0,0,0,+1)`：判据是 `dot(n, c) + w + radius < 0` 才剔除，零法线时退化为
/// `w < 0`，故 `w = +1` 恒不剔除 —— 宁可多提交，也绝不因退化输入误剔可见物。
[[nodiscard]] glm::vec4 NormalizePlane(const glm::vec4& plane) noexcept {
    const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
    if (!(length > 0.0F)) {
        return glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
    }
    return plane / length;
}

}  // namespace

Frustum FrustumFromViewProjection(const glm::mat4& viewProjection) noexcept {
    const glm::vec4 row0 = MatrixRow(viewProjection, 0);
    const glm::vec4 row1 = MatrixRow(viewProjection, 1);
    const glm::vec4 row2 = MatrixRow(viewProjection, 2);  // 深度 0~1：第 3 行即近平面
    const glm::vec4 row3 = MatrixRow(viewProjection, 3);

    Frustum frustum;
    frustum.planes[0] = NormalizePlane(row3 + row0);  // 左
    frustum.planes[1] = NormalizePlane(row3 - row0);  // 右
    frustum.planes[2] = NormalizePlane(row3 + row1);  // 下
    frustum.planes[3] = NormalizePlane(row3 - row1);  // 上
    frustum.planes[4] = NormalizePlane(row2);         // 近（0~1 深度约定）
    frustum.planes[5] = NormalizePlane(row3 - row2);  // 远
    return frustum;
}

bool FrustumIntersectsAabb(const Frustum& frustum, const glm::vec3& minimum, const glm::vec3& maximum) noexcept {
    // 先用 AABB 的"中心 + 半长"表述，便于取每个平面方向上的最远角。
    const glm::vec3 center    = (minimum + maximum) * 0.5F;
    const glm::vec3 halfSize  = (maximum - minimum) * 0.5F;
    const glm::vec3 halfAbs   = glm::vec3(std::fabs(halfSize.x), std::fabs(halfSize.y), std::fabs(halfSize.z));

    for (const glm::vec4& plane : frustum.planes) {
        // 平面法线方向上的"最远角"到平面的有符号距离；< 0 即整个 AABB 在外侧。
        const float distance = glm::dot(glm::vec3(plane), center) + plane.w;
        const float radius   = glm::dot(glm::abs(glm::vec3(plane)), halfAbs);
        if (distance + radius < 0.0F) {
            return false;
        }
    }
    return true;
}

}  // namespace vx
