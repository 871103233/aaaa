#pragma once

#include "object/object_layer.hpp"
#include "object/object_mesh.hpp"      // `object_mesh_detail::AppendBoxCentered`（几何复用）
#include "render/mesh_renderer.hpp"    // `MeshData` / `MeshVertex`

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vx {

/// **gizmo 的可交互手柄**（V0.11 / I4；[ADR 0038](../../docs/adr/0038-construction-editor-and-runtime-separation.md) 决策四；
/// V0.11 / A8 增 `Plane`，见 [ADR 0041](../../docs/adr/0041-immersive-modify-mode-and-editor-camera.md)）。
///
/// 口径：**平移只做水平两轴（X / Z）** —— 物件底面由**地表高度**求解（`y` 是地形的函数），
/// 允许沿 Y 拖动会造出"悬空物件"，与 ADR 0032 / V0.11 I2 的"底面贴地 / 不悬空"契约冲突。
/// **旋转只做绕 Y（yaw）** —— `ObjectPlacement` / `ObjectBuilding` 只有 `yawDegrees` 一个朝向自由度。
enum class GizmoHandle : std::uint8_t {
    None,
    TranslateX,  ///< 沿世界 +X 平移
    TranslateZ,  ///< 沿世界 +Z 平移
    Plane,       ///< **中心平面手柄**：在 **XZ 水平面内任意方向**平移（V0.11 / A8；`y` 仍由地表决定 ⇒ 不悬空）
    RotateY,     ///< 绕世界 Y 轴旋转（yaw）
};

/// gizmo 在**世界坐标**中的布局（原点 = 选中物件的**包围盒半高**处；尺度**由调用方按"屏幕空间恒定"求解**）。
struct GizmoLayout {
    double x            = 0.0;   ///< 原点（物件包围盒中心：**半高**，避免被大模型挡住）
    double y            = 0.0;
    double z            = 0.0;
    double axisLength   = 2.0;   ///< 平移箭头长度（格）
    double handleRadius = 0.20;  ///< 箭头 / 环带的**有效拾取半径**（格，略大于视觉厚度 ⇒ 好点中）
    double ringRadius   = 1.6;   ///< 旋转环半径（格）
    /// **中心平面手柄的半边长**（格）：射线命中以原点为中心、边长 `2 × 本值` 的 XZ 方块 ⇒ 判为 `Plane`。
    /// **优先于轴线**（中心区域归平面手柄，避免"想自由拖却点到轴"）。
    double planeHalfExtent = 0.5;
};

/// 世界坐标**射线**（`dir` 须为**单位向量**）。
struct GizmoRay {
    double ox = 0.0;
    double oy = 0.0;
    double oz = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double dz = 1.0;
};

/// **纯函数**：射线 × 轴对齐盒（slab 法）。命中返回 `true` 并写出最近正 `t`。
[[nodiscard]] inline bool GizmoRayHitsAabb(const GizmoRay& ray, double minX, double minY, double minZ, double maxX,
                                           double maxY, double maxZ, double& outT) noexcept {
    double tMin = 0.0;
    double tMax = 1.0e30;
    const double origin[3]    = { ray.ox, ray.oy, ray.oz };
    const double direction[3] = { ray.dx, ray.dy, ray.dz };
    const double lo[3]        = { minX, minY, minZ };
    const double hi[3]        = { maxX, maxY, maxZ };
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1.0e-12) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) {
                return false;  // 平行且在板外
            }
            continue;
        }
        const double inv = 1.0 / direction[axis];
        double       t0  = (lo[axis] - origin[axis]) * inv;
        double       t1  = (hi[axis] - origin[axis]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    outT = tMin;
    return true;
}

/// **纯函数**：射线与 gizmo 手柄求交，返回**最近**命中的手柄（无命中 = `GizmoHandle::None`）。
///
/// 口径（与视觉一致、可判定）：
///   - `Plane`（**优先**）：射线命中以原点为中心、半边长 `planeHalfExtent` 的**扁方块**（XZ 平面手柄）；
///   - `TranslateX` / `TranslateZ`：射线 × 该轴箭头的**轴对齐盒**（沿轴 `axisLength`、截面 `handleRadius`）；
///   - `RotateY`：射线 × **水平面 `y = 原点 y`**，命中点**到原点的水平半径**落在 `ringRadius ± handleRadius` 内。
/// 平面手柄**优先**（中心区域归它）；其余三者取**最近**者（避免"环带挡住箭头"这类歧义）。`maxDistance` 之外的命中忽略。
[[nodiscard]] inline GizmoHandle PickGizmoHandle(const GizmoRay& ray, const GizmoLayout& gizmo,
                                                 double maxDistance) noexcept {
    GizmoHandle best   = GizmoHandle::None;
    double      bestT  = maxDistance;
    const double r     = gizmo.handleRadius;
    const double len   = gizmo.axisLength;

    double t = 0.0;
    // ① 中心平面手柄（**优先**）：扁方块（y 方向也只留一层厚度 ⇒ 视线越平越难点，符合"俯视拖平面"的直觉）。
    const double planeHalfY = std::max(r, gizmo.planeHalfExtent * 0.35);
    if (GizmoRayHitsAabb(ray, gizmo.x - gizmo.planeHalfExtent, gizmo.y - planeHalfY, gizmo.z - gizmo.planeHalfExtent,
                         gizmo.x + gizmo.planeHalfExtent, gizmo.y + planeHalfY, gizmo.z + gizmo.planeHalfExtent, t) &&
        t < bestT) {
        return GizmoHandle::Plane;  // 命中即以平面手柄为准（不再与轴线比远近）
    }
    // ② +X 箭头：从原点到 (x + len, y, z)，截面半径 r。
    if (GizmoRayHitsAabb(ray, gizmo.x - r, gizmo.y - r, gizmo.z - r, gizmo.x + len, gizmo.y + r, gizmo.z + r, t) &&
        t < bestT) {
        best  = GizmoHandle::TranslateX;
        bestT = t;
    }
    // ③ +Z 箭头。
    if (GizmoRayHitsAabb(ray, gizmo.x - r, gizmo.y - r, gizmo.z - r, gizmo.x + r, gizmo.y + r, gizmo.z + len, t) &&
        t < bestT) {
        best  = GizmoHandle::TranslateZ;
        bestT = t;
    }
    // 旋转环：水平面求交 + 半径带判定。
    if (std::abs(ray.dy) > 1.0e-9) {
        const double planeT = (gizmo.y - ray.oy) / ray.dy;
        if (planeT > 0.0 && planeT < bestT) {
            const double px     = ray.ox + planeT * ray.dx;
            const double pz     = ray.oz + planeT * ray.dz;
            const double radius = std::sqrt((px - gizmo.x) * (px - gizmo.x) + (pz - gizmo.z) * (pz - gizmo.z));
            if (std::abs(radius - gizmo.ringRadius) <= r) {
                best  = GizmoHandle::RotateY;
                bestT = planeT;
            }
        }
    }
    return best;
}

/// **纯函数**：环平面上一点相对环心的**偏航角**（度，`[-180, 180]`）—— **与引擎 `+Y` 旋转同手性**
/// （从 `+X` 起、**朝 `−Z` 为正**；等价于 `atan2(−dz, dx)`）。
///
/// 为什么是 `−dz` 而不是 `dz`：物件的旋转最终按 `glm::angleAxis(yaw, +Y)` 施加，而**绕 `+Y` 的正向旋转把 `+X` 转向 `−Z`**
/// （右手系）。若这里取 `atan2(+dz, dx)`（"数学习惯"的普通极角），算出的角位移**与引擎旋转手性相反**
/// ⇒ 拖环时**物件朝光标的反方向转**（所有者 2026-10-09 实测缺陷："拖动黄色环的转动方向反了"）。
/// ⇒ 口径：环的角位移**一律用本函数换算**，不要在调用点再补一次取反（会双重反向）。
[[nodiscard]] inline double GizmoAngleDegrees(const GizmoLayout& gizmo, double px, double pz) noexcept {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    return std::atan2(-(pz - gizmo.z), px - gizmo.x) * kRadToDeg;
}

/// **纯函数**：从"起始角"到"当前角"的 **yaw 增量**（度），归一化到 `(-180, 180]`。
///
/// 为什么必须归一化：环上跨过 ±180° 边界时，`current − start` 会算出 ≈ ∓360° 的跳变 ⇒ 物件会瞬间反转一圈。
[[nodiscard]] inline double GizmoYawDeltaDegrees(double startAngleDegrees, double currentAngleDegrees) noexcept {
    double delta = currentAngleDegrees - startAngleDegrees;
    while (delta > 180.0) {
        delta -= 360.0;
    }
    while (delta <= -180.0) {
        delta += 360.0;
    }
    return delta;
}

/// 平移箭头的**局部**网格（沿 `+X`，起点在原点）：细长轴杆 + 靠近末端的**箭头块**。
/// 调用方按轴旋转（Z 轴 = 绕 Y 转 90°）并平移到 gizmo 原点。
[[nodiscard]] inline MeshData BuildGizmoAxisMesh(double length, double thickness) {
    MeshData mesh;
    const float len  = static_cast<float>(length);
    const float thin = static_cast<float>(thickness);
    const float headLength = len * 0.28F;
    const float shaftEnd   = len - headLength;
    // 轴杆：中心在 (shaftEnd/2, 0, 0)、半尺寸 (shaftEnd/2, thin, thin)。
    object_mesh_detail::AppendBoxCentered(mesh, shaftEnd * 0.5F, 0.0F, 0.0F, shaftEnd * 0.5F, thin, thin, 0.0F);
    // 箭头：更大的方块（读作"箭头"；锥体需要按任意轴生成，代价不值当）。
    object_mesh_detail::AppendBoxCentered(mesh, shaftEnd + headLength * 0.5F, 0.0F, 0.0F, headLength * 0.5F,
                                          thin * 2.2F, thin * 2.2F, 0.0F);
    return mesh;
}

/// 旋转环的**局部**网格（XZ 平面上的**扁平圆环**，中心在原点；含上下两层法线 ⇒ 两面可见）。
///
/// 取扁平环而不做圆管：几何小、拾取判据（`PickGizmoHandle` 的环带）与视觉**逐字对应**。
/// **已知限制**：正侧视（视线几乎与环平面重合）时环看起来偏细 —— 登记为观感项，不影响可用性。
[[nodiscard]] inline MeshData BuildGizmoRingMesh(double radius, double thickness) {
    MeshData mesh;
    constexpr int kSegments = 48;
    const float   r         = static_cast<float>(radius);
    const float   band      = static_cast<float>(thickness);
    const float   inner     = std::max(0.0F, r - band);
    const float   outer     = r + band;
    constexpr float kPi     = 3.14159265358979323846F;

    const auto appendLayer = [&](float normalY) {
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = kPi * 2.0F * static_cast<float>(i) / static_cast<float>(kSegments);
            const float a1 = kPi * 2.0F * static_cast<float>(i + 1) / static_cast<float>(kSegments);
            const float c0 = std::cos(a0);
            const float s0 = std::sin(a0);
            const float c1 = std::cos(a1);
            const float s1 = std::sin(a1);

            const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(object_mesh_detail::MakeVertex(inner * c0, 0.0F, inner * s0, 0.0F, normalY, 0.0F,
                                                                   0.0F));
            mesh.vertices.push_back(object_mesh_detail::MakeVertex(outer * c0, 0.0F, outer * s0, 0.0F, normalY, 0.0F,
                                                                   0.0F));
            mesh.vertices.push_back(object_mesh_detail::MakeVertex(outer * c1, 0.0F, outer * s1, 0.0F, normalY, 0.0F,
                                                                   0.0F));
            mesh.vertices.push_back(object_mesh_detail::MakeVertex(inner * c1, 0.0F, inner * s1, 0.0F, normalY, 0.0F,
                                                                   0.0F));
            // 绕序：法线朝 +Y 时从上方看逆时针；朝 −Y 时反转。
            if (normalY > 0.0F) {
                mesh.indices.push_back(base + 0U);
                mesh.indices.push_back(base + 2U);
                mesh.indices.push_back(base + 1U);
                mesh.indices.push_back(base + 0U);
                mesh.indices.push_back(base + 3U);
                mesh.indices.push_back(base + 2U);
            } else {
                mesh.indices.push_back(base + 0U);
                mesh.indices.push_back(base + 1U);
                mesh.indices.push_back(base + 2U);
                mesh.indices.push_back(base + 0U);
                mesh.indices.push_back(base + 2U);
                mesh.indices.push_back(base + 3U);
            }
        }
    };
    appendLayer(1.0F);
    appendLayer(-1.0F);
    return mesh;
}

/// 中心**平面手柄**的局部网格（V0.11 / A8）：以原点为中心的**扁方块**（XZ 平面、薄），
/// 视觉与 `PickGizmoHandle` 的 `planeHalfExtent` 判据**逐字对应**。
[[nodiscard]] inline MeshData BuildGizmoPlaneMesh(double halfExtent) {
    MeshData mesh;
    const float half = static_cast<float>(halfExtent);
    object_mesh_detail::AppendBoxCentered(mesh, 0.0F, 0.0F, 0.0F, half, half * 0.3F, half, 0.0F);
    return mesh;
}

/// **纯函数**：gizmo 的**世界尺度**（格）——"**屏幕空间恒定**"与"**不低于模型自身比例**"取大者（V0.11 / A8）。
///
/// 两个诉求（[ADR 0041](../../docs/adr/0041-immersive-modify-mode-and-editor-camera.md)）：
///   ① **屏幕空间恒定**（Unity `Handles` / UE5 / Blender 的口径）：手柄在屏幕上的像素尺寸恒定
///      ⇒ 世界尺度 ∝ 相机距离——`worldPerPixel = 2·tan(fov/2)·distance / viewportHeight`，
///      乘上目标像素数即得世界尺度 ⇒ **再远也点得到**；
///   ② **不低于模型比例**：手柄必须**伸出模型之外**，否则大模型会把手柄整个包住 ⇒ 看不见、点不到。
///
/// 取**两者较大值**并钳到 `[minWorldSize, maxWorldSize]`。`modelRadius` ≤ 0 / 视口非法 ⇒ 只按屏幕恒定。
[[nodiscard]] inline double GizmoWorldScale(double cameraDistance, float verticalFovDegrees,
                                            float viewportHeightPixels, double targetPixels, double modelRadius,
                                            double modelSizeRatio, double minWorldSize, double maxWorldSize) noexcept {
    double worldPerPixel = 0.0;
    if (viewportHeightPixels > 1.0F && cameraDistance > 0.0) {
        constexpr double kPi       = 3.14159265358979323846;
        const double     halfFov   = static_cast<double>(verticalFovDegrees) * kPi / 360.0;
        worldPerPixel = 2.0 * std::tan(halfFov) * cameraDistance / static_cast<double>(viewportHeightPixels);
    }
    const double screenSized = worldPerPixel * targetPixels;
    const double modelSized  = (modelRadius > 0.0) ? modelRadius * modelSizeRatio : 0.0;
    return std::clamp(std::max(screenSized, modelSized), minWorldSize, maxWorldSize);
}

}  // namespace vx
