#pragma once

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace vx {

/// 世界空间射线（**double** 定位，红线 6）。
struct ScreenRay {
    glm::dvec3 origin { 0.0 };
    glm::dvec3 direction { 0.0, 0.0, -1.0 };  ///< 单位向量；非法入参时保持缺省（朝 −Z）
};

/// **屏幕像素坐标 → 世界射线**（纯函数，可单测）。
///
/// 用途（V0.11 / A8 / [ADR 0041](../../docs/adr/0041-immersive-modify-mode-and-editor-camera.md)）：
/// 修改模式是**自由光标**的编辑器式界面 ⇒ 拾取必须由"**光标所在像素**"反投影出射线，
/// 而不是用屏幕正中的准星（那会把光标位置丢掉）。
///
/// 约定：
///   - `screenX / screenY` 是**窗口客户区像素**，原点在**左上角**、y 轴**向下**（SDL / ImGui 同口径）；
///   - NDC 的 y **向上** ⇒ 这里翻转一次；
///   - 取 NDC `z = 0`（近）与 `z = 1`（远）两点，方向 = 归一化（远 − 近）；
///     两个 z 的约定（0~1 或 −1~1）都不影响**方向**与**射线所在直线**，只影响 `origin` 取在近面还是中间面。
///   - 视口尺寸非正、矩阵奇异（`w == 0`）⇒ 返回缺省射线（调用方据此判定"拾取不可用"）。
///
/// 纯函数：只依赖入参，不读全局状态、不分配内存，可在热路径调用。
[[nodiscard]] inline ScreenRay ScreenPointToRay(const glm::mat4& viewProjection, float screenX, float screenY,
                                                float viewportWidth, float viewportHeight) noexcept {
    ScreenRay ray;
    if (viewportWidth <= 0.0F || viewportHeight <= 0.0F) {
        return ray;
    }
    const float     ndcX = (screenX / viewportWidth) * 2.0F - 1.0F;
    const float     ndcY = 1.0F - (screenY / viewportHeight) * 2.0F;  // 屏幕 y 向下 ⇒ NDC y 向上
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);

    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.0F, 1.0F);
    glm::vec4 farPoint  = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.0F, 1.0F);
    if (nearPoint.w == 0.0F || farPoint.w == 0.0F) {
        return ray;  // 矩阵奇异（相机退化）⇒ 不给射线
    }
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    const glm::dvec3 nearWorld(nearPoint.x, nearPoint.y, nearPoint.z);
    const glm::dvec3 farWorld(farPoint.x, farPoint.y, farPoint.z);
    const glm::dvec3 delta = farWorld - nearWorld;
    const double     length = glm::length(delta);
    if (length <= 0.0) {
        return ray;
    }
    ray.origin    = nearWorld;
    ray.direction = delta / length;
    return ray;
}

}  // namespace vx
