// gizmo 手柄的**纯逻辑**测试（V0.11 / I4；ADR 0038 决策四"gizmo"）。
//
// 判据（可判定）：
//   1. 射线命中**轴杆** ⇒ 对应平移手柄（X / Z）；
//   2. 射线命中**环带**（半径落在 `ringRadius ± handleRadius`）⇒ 旋转手柄；
//   3. 三者都未命中 ⇒ `None`（近者优先 ⇒ 手柄重合处取最近）；
//   4. yaw 增量在 ±180° 边界处**不跳变**（归一化到 `(-180, 180]`）；
//   5. 手柄网格非空（可见性依赖它）。

#include "object/gizmo.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using vx::BuildGizmoAxisMesh;
using vx::BuildGizmoRingMesh;
using vx::GizmoHandle;
using vx::GizmoLayout;
using vx::GizmoRay;
using vx::GizmoYawDeltaDegrees;
using vx::PickGizmoHandle;

GizmoLayout LayoutAt(double x, double y, double z) {
    GizmoLayout layout;
    layout.x            = x;
    layout.y            = y;
    layout.z            = z;
    layout.axisLength   = 2.0;
    layout.handleRadius = 0.25;
    layout.ringRadius   = 1.6;
    return layout;
}

/// 从正上方竖直向下的射线（`dx = x, dz = z`）。
GizmoRay DownRay(double x, double z) {
    GizmoRay ray;
    ray.ox = x;
    ray.oy = 20.0;
    ray.oz = z;
    ray.dx = 0.0;
    ray.dy = -1.0;
    ray.dz = 0.0;
    return ray;
}

}  // namespace

TEST(Gizmo, RayOnXAxisPicksTranslateX) {
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    EXPECT_EQ(PickGizmoHandle(DownRay(1.0, 0.0), layout, 100.0), GizmoHandle::TranslateX);
}

TEST(Gizmo, RayOnZAxisPicksTranslateZ) {
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    EXPECT_EQ(PickGizmoHandle(DownRay(0.0, 1.0), layout, 100.0), GizmoHandle::TranslateZ);
}

TEST(Gizmo, RayOnRingBandPicksRotateY) {
    // 取 −X 方向（轴杆都不覆盖该处）⇒ 只剩环带可命中。
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    EXPECT_EQ(PickGizmoHandle(DownRay(-1.6, 0.0), layout, 100.0), GizmoHandle::RotateY);
}

TEST(Gizmo, RayMissingEverythingPicksNone) {
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    // (0.9, 0.9)：X／Z 轴杆都够不着；到环心的水平距离 ≈ 1.27，偏离 1.6 超过 handleRadius ⇒ 未命中。
    EXPECT_EQ(PickGizmoHandle(DownRay(0.9, 0.9), layout, 100.0), GizmoHandle::None);
}

TEST(Gizmo, YawDeltaIsNormalizedAcrossBoundary) {
    EXPECT_NEAR(GizmoYawDeltaDegrees(170.0, -170.0), 20.0, 1.0e-9);
    EXPECT_NEAR(GizmoYawDeltaDegrees(-170.0, 170.0), -20.0, 1.0e-9);
    EXPECT_NEAR(GizmoYawDeltaDegrees(0.0, 90.0), 90.0, 1.0e-9);
    EXPECT_NEAR(GizmoYawDeltaDegrees(30.0, 10.0), -20.0, 1.0e-9);
}

TEST(Gizmo, GizmoMeshesAreNonEmpty) {
    const vx::MeshData axis = BuildGizmoAxisMesh(2.0, 0.07);
    EXPECT_FALSE(axis.vertices.empty());
    EXPECT_FALSE(axis.indices.empty());

    const vx::MeshData ring = BuildGizmoRingMesh(1.6, 0.14);
    EXPECT_FALSE(ring.vertices.empty());
    EXPECT_FALSE(ring.indices.empty());
}
