#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace vx {

/// 视锥体：6 个**归一化**平面，约定 `dot(plane.xyz, p) + plane.w >= 0` 表示 `p` 在视锥内侧。
///
/// 平面顺序固定为 `左 / 右 / 下 / 上 / 近 / 远`（与 `FrustumFromViewProjection` 一一对应），
/// 便于调试时按序号定位。
struct Frustum {
    std::array<glm::vec4, 6> planes {};
};

/// 由**视图投影矩阵**提取视锥平面（Gribb–Hartmann 方法）。
///
/// 约定：`viewProjection` 的深度范围是 **0~1**（SDL_gpu / 本项目投影矩阵的约定），
/// 因此近平面取第 3 行、远平面取 `第 4 行 − 第 3 行`（若深度是 −1~1，则近平面要取 `第4行 + 第3行`）。
///
/// 坐标系：与传入的矩阵同空间。本项目上传的顶点是**相机相对**坐标，故传相机相对的
/// `viewProjection` 时，测试用的 AABB 也必须先减去渲染原点（见 `game/main.cpp` 的剔除循环）。
///
/// 纯函数：不读全局、不分配。
[[nodiscard]] Frustum FrustumFromViewProjection(const glm::mat4& viewProjection) noexcept;

/// AABB 是否与视锥**相交**（保守判定：返回 `false` 才表示"必定不可见"）。
///
/// 判据：对每个平面取 AABB 上沿平面法线最远的那个角（positive vertex），若它在平面外侧，
/// 则整个 AABB 在该平面外侧 ⇒ 不可见。
/// **保守性**：AABB 完全包含视锥、或与平面相交时返回 `true`（允许假阳性，绝不误剔可见物）。
///
/// 纯函数：不读全局、不分配。
[[nodiscard]] bool FrustumIntersectsAabb(const Frustum& frustum, const glm::vec3& minimum,
                                         const glm::vec3& maximum) noexcept;

}  // namespace vx
