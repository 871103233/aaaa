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
using vx::GizmoAngleDegrees;
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

// V0.11 / A8j 缺陷修复：环角必须**与引擎 `+Y` 旋转同手性**（从 +X 起、**朝 −Z 为正**）。
// 判据：光标自 +X 移到 **−Z** ⇒ **正** 90°，移到 **+Z** ⇒ **负** 90°。
// 为什么 +Z 是负的：`yaw += delta` 后物件的 +X 要**跟着光标**走 —— 绕 `+Y` 正向旋转把 +X 转向 **−Z**
// ⇒ 光标走向 −Z 时 yaw 必须**增大**。若这里符号反了，拖环时物件会**朝光标的反方向**转
// （所有者 2026-10-09 实测："拖动黄色环的转动方向反了"）。
TEST(Gizmo, RingAngleFollowsEngineYawHandedness) {
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    EXPECT_NEAR(GizmoAngleDegrees(layout, 1.0, 0.0), 0.0, 1.0e-9);      // +X
    EXPECT_NEAR(GizmoAngleDegrees(layout, 0.0, -1.0), 90.0, 1.0e-9);   // −Z ⇒ +90°
    EXPECT_NEAR(GizmoAngleDegrees(layout, 0.0, 1.0), -90.0, 1.0e-9);   // +Z ⇒ −90°
    // −X 落在 ±180 边界上：`atan2(−0.0, −1)` 取 **−180**（正负号对增量无影响 —— `GizmoYawDeltaDegrees`
    // 会把它归一化到 `(-180, 180]`，−180 与 +180 等价）。
    EXPECT_NEAR(GizmoAngleDegrees(layout, -1.0, 0.0), -180.0, 1.0e-9);  // −X
}

TEST(Gizmo, GizmoMeshesAreNonEmpty) {
    const vx::MeshData axis = BuildGizmoAxisMesh(2.0, 0.07);
    EXPECT_FALSE(axis.vertices.empty());
    EXPECT_FALSE(axis.indices.empty());

    const vx::MeshData ring = BuildGizmoRingMesh(1.6, 0.14);
    EXPECT_FALSE(ring.vertices.empty());
    EXPECT_FALSE(ring.indices.empty());

    const vx::MeshData plane = vx::BuildGizmoPlaneMesh(0.5);
    EXPECT_FALSE(plane.vertices.empty());
    EXPECT_FALSE(plane.indices.empty());
}

// ---- V0.11 / A8：中心平面手柄（XZ 内任意方向）与屏幕恒定尺度 ----

TEST(Gizmo, PlaneHandleWinsNearCenter) {
    // (0.2, 0.0) 同时落在 X 轴盒内 ⇒ 必须由**平面手柄优先**夺走（"想自由拖却点到轴"是缺陷）。
    const GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    EXPECT_EQ(PickGizmoHandle(DownRay(0.2, 0.0), layout, 100.0), GizmoHandle::Plane);
    EXPECT_EQ(PickGizmoHandle(DownRay(0.0, 0.2), layout, 100.0), GizmoHandle::Plane);
    EXPECT_EQ(PickGizmoHandle(DownRay(0.3, -0.3), layout, 100.0), GizmoHandle::Plane);
}

TEST(Gizmo, OutsidePlaneHalfExtentFallsBackToAxis) {
    GizmoLayout layout = LayoutAt(0.0, 10.0, 0.0);
    layout.planeHalfExtent = 0.3;
    EXPECT_EQ(PickGizmoHandle(DownRay(0.5, 0.0), layout, 100.0), GizmoHandle::TranslateX)
        << "平面手柄只占中心一小块 ⇒ 外侧仍是轴";
    EXPECT_EQ(PickGizmoHandle(DownRay(0.0, 1.2), layout, 100.0), GizmoHandle::TranslateZ);
}

TEST(Gizmo, WorldScaleIsScreenConstantWhenUnclamped) {
    // 屏幕恒定 ⇒ 世界尺度 ∝ 相机距离（同一视口 / FOV / 目标像素）。
    const double near = vx::GizmoWorldScale(10.0, 70.0F, 1080.0F, 90.0, /*modelRadius=*/0.0, 1.5, 0.01, 100.0);
    const double far  = vx::GizmoWorldScale(20.0, 70.0F, 1080.0F, 90.0, 0.0, 1.5, 0.01, 100.0);
    EXPECT_NEAR(far, near * 2.0, 1.0e-9);
    EXPECT_GT(near, 0.0);
}

TEST(Gizmo, WorldScalePicksUpTheModelSizeFloor) {
    // 模型很大时，手柄必须**伸出模型之外**（不被包住）⇒ 由模型半径给下限。
    const double smallModel = vx::GizmoWorldScale(10.0, 70.0F, 1080.0F, 90.0, /*modelRadius=*/0.5, 1.5, 0.01, 100.0);
    const double bigModel   = vx::GizmoWorldScale(10.0, 70.0F, 1080.0F, 90.0, /*modelRadius=*/8.0, 1.5, 0.01, 100.0);
    EXPECT_NEAR(bigModel, 12.0, 1.0e-9) << "8 × 1.5 = 12 格 ⇒ 箭头伸出一座大建筑之外";
    EXPECT_LT(smallModel, bigModel);
}

TEST(Gizmo, WorldScaleClampsAndToleratesDegenerateViewport) {
    EXPECT_DOUBLE_EQ(vx::GizmoWorldScale(10.0, 70.0F, 1080.0F, 90.0, 0.0, 1.5, 2.0, 5.0), 2.0)
        << "低于下限 ⇒ 取 minWorldSize";
    EXPECT_DOUBLE_EQ(vx::GizmoWorldScale(10000.0, 70.0F, 1080.0F, 90.0, 0.0, 1.5, 0.01, 50.0), 50.0)
        << "高于上限 ⇒ 取 maxWorldSize";
    EXPECT_DOUBLE_EQ(vx::GizmoWorldScale(10.0, 70.0F, /*viewportHeightPixels=*/0.0F, 90.0, 0.0, 1.5, 0.7, 5.0), 0.7)
        << "视口非法 ⇒ 退回下限（调用方仍能拿到一个可用尺度）";
}
