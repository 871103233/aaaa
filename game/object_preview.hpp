#pragma once

// 物件选择器的**预览小图**（V0.5 E4；[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md)）。
//
// 形态 = **CPU 正交投影 + 固定光源朗伯明暗**（纯函数、零 GPU 资源、零渲染器改动）：
// 输入是**与最终摆放同一份** `buildLocalMesh` 产出的 `MeshData` ⇒ 所见即所得（同源），
// 输出是归一化的屏幕空间三角形序列，由面板用 ImGui 绘制列表直接画出来。
//
// 为什么不用"离屏渲染真缩略图"：那需要渲染器新增"渲到任意纹理"的公开 API + 独立小管线 / 目标 +
// ImGui 纹理接线；本项目渲染器当前只输出到交换链。该方案已作为**备选**登记在 ADR 0032
// （切换条件见该 ADR §二 / §四），本阶段先交付 CPU 投影版。

#include "render/mesh_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

namespace vx {

/// 预览三角（屏幕空间，**归一化到 `[0,1]²`、左上为原点**；`shade` ∈ `[0,1]` = 固定光源下的朗伯明暗）。
///
/// 归一化而非像素坐标 ⇒ 面板可自由决定预览区尺寸，几何计算与 UI 布局解耦。
struct PreviewTriangle {
    glm::vec2 a { 0.0F };
    glm::vec2 b { 0.0F };
    glm::vec2 c { 0.0F };
    float     shade = 1.0F;
};

/// 预览的固定俯仰角（弧度，≈ 20°）：略高于水平 ⇒ 既看得到高度、也看得到顶面。
inline constexpr float kObjectPreviewPitchRadians = 0.35F;

/// 归一化比例：物体包围球半径 → 半屏比例。`0.44` ⇒ 直径占 88%、四周各留 6% 边距。
inline constexpr float kObjectPreviewHalfExtent = 0.44F;

/// 由 `MeshData` 生成预览三角序列（V0.5 E4）。**纯函数、确定性**。
///
/// 口径：
///   - **正交视图**：先绕 Y 转 `yawRadians`、再绕 X 转 `pitchRadians`；相机在 `+Z` 侧看向 `−Z`；
///   - **背面剔除**用**顶点法线的平均**（不依赖三角形绕序 ⇒ 对 CW / CCW 网格都成立），
///     平均法线近似退化时回落到面法线；
///   - **画家算法**：按平均深度**升序**（远先画）排序 ⇒ 近处覆盖远处；
///   - **尺寸恒定**：归一化基准取**未旋转**的包围球（旋转不变量）⇒ 转动时不会"呼吸"。
///
/// 空 / 退化网格（顶点 < 3、索引 < 3、包围球退化为 0）⇒ 返回空序列（调用方据此显示"无预览"）。
[[nodiscard]] inline std::vector<PreviewTriangle> BuildObjectPreview(const MeshData& mesh, float yawRadians,
                                                                    float pitchRadians) {
    std::vector<PreviewTriangle> triangles;
    if (mesh.vertices.size() < 3U || mesh.indices.size() < 3U) {
        return triangles;
    }

    // ---- 1) 未旋转的包围盒中心 + 包围球半径（旋转不变量 ⇒ 转动时大小恒定）----
    glm::vec3 minimum(std::numeric_limits<float>::max());
    glm::vec3 maximum(std::numeric_limits<float>::lowest());
    for (const MeshVertex& vertex : mesh.vertices) {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }
    const glm::vec3 center = (minimum + maximum) * 0.5F;

    float radius = 0.0F;
    for (const MeshVertex& vertex : mesh.vertices) {
        const glm::vec3 position(vertex.position[0] - center.x, vertex.position[1] - center.y,
                                 vertex.position[2] - center.z);
        radius = std::max(radius, glm::length(position));
    }
    if (!(radius > 1.0e-4F)) {
        return triangles;  // 退化（单点 / 全部重合）⇒ 无预览
    }
    const float scale = kObjectPreviewHalfExtent / radius;

    // ---- 2) 旋转：绕 Y（yaw）→ 绕 X（pitch），均为右手系标准式 ----
    const float cosYaw   = std::cos(yawRadians);
    const float sinYaw   = std::sin(yawRadians);
    const float cosPitch = std::cos(pitchRadians);
    const float sinPitch = std::sin(pitchRadians);

    const auto rotateToView = [&](const glm::vec3& local) {
        const glm::vec3 d = local - center;
        const glm::vec3 yawed(d.x * cosYaw + d.z * sinYaw, d.y, -d.x * sinYaw + d.z * cosYaw);
        return glm::vec3(yawed.x, yawed.y * cosPitch - yawed.z * sinPitch,
                         yawed.y * sinPitch + yawed.z * cosPitch);
    };

    std::vector<glm::vec3> viewPositions(mesh.vertices.size());
    std::vector<glm::vec3> viewNormals(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        viewPositions[i]         = rotateToView(glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]));
        // 法线只受旋转影响（不平移）：用同一套旋转作用在方向上。
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        const glm::vec3 yawed(normal.x * cosYaw + normal.z * sinYaw, normal.y,
                              -normal.x * sinYaw + normal.z * cosYaw);
        viewNormals[i] = glm::vec3(yawed.x, yawed.y * cosPitch - yawed.z * sinPitch,
                                   yawed.y * sinPitch + yawed.z * cosPitch);
    }

    /// 固定光源（视图空间；与相机同侧偏上 ⇒ 正面受光、底面变暗）。
    const glm::vec3 light = glm::normalize(glm::vec3(-0.35F, 0.55F, 0.76F));

    struct SortedTriangle {
        PreviewTriangle triangle;
        float           depth = 0.0F;
    };
    std::vector<SortedTriangle> ordered;
    ordered.reserve(mesh.indices.size() / 3U);

    for (std::size_t i = 0; i + 2U < mesh.indices.size(); i += 3U) {
        const std::size_t i0 = mesh.indices[i];
        const std::size_t i1 = mesh.indices[i + 1U];
        const std::size_t i2 = mesh.indices[i + 2U];
        if (i0 >= viewPositions.size() || i1 >= viewPositions.size() || i2 >= viewPositions.size()) {
            continue;  // 索引越界 ⇒ 跳过该面（不静默产出错几何）
        }
        const glm::vec3& p0 = viewPositions[i0];
        const glm::vec3& p1 = viewPositions[i1];
        const glm::vec3& p2 = viewPositions[i2];

        const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
        const float     area2      = glm::length(faceNormal);
        if (!(area2 > 1.0e-12F)) {
            continue;  // 退化三角形（零面积）
        }

        glm::vec3 normal = viewNormals[i0] + viewNormals[i1] + viewNormals[i2];
        if (glm::dot(normal, normal) > 1.0e-12F) {
            normal = glm::normalize(normal);  // 顶点法线平均（平滑；不依赖绕序）
        } else {
            normal = faceNormal / area2;  // 无有效顶点法线 ⇒ 回落到面法线
        }
        if (normal.z <= 0.0F) {
            continue;  // 背面（相机在 +Z 侧看向 −Z）
        }

        SortedTriangle entry;
        entry.triangle.shade = 0.22F + 0.78F * std::max(0.0F, glm::dot(normal, light));
        entry.triangle.a     = glm::vec2(0.5F + p0.x * scale, 0.5F - p0.y * scale);
        entry.triangle.b     = glm::vec2(0.5F + p1.x * scale, 0.5F - p1.y * scale);
        entry.triangle.c     = glm::vec2(0.5F + p2.x * scale, 0.5F - p2.y * scale);
        entry.depth          = (p0.z + p1.z + p2.z) / 3.0F;
        ordered.push_back(entry);
    }

    // 画家算法：远（z 小）先画。`stable_sort` ⇒ 同深度保持原顺序（确定性）。
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const SortedTriangle& lhs, const SortedTriangle& rhs) { return lhs.depth < rhs.depth; });

    triangles.reserve(ordered.size());
    for (const SortedTriangle& entry : ordered) {
        triangles.push_back(entry.triangle);
    }
    return triangles;
}

}  // namespace vx
