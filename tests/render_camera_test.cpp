#include "render/camera.hpp"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <cmath>

namespace {

using vx::CameraSettings;
using vx::CameraView;
using vx::ITerrainQuery;
using vx::kCameraMinDistance;
using vx::kCameraPitchLimit;
using vx::ThirdPersonCamera;

/// 实心球地形桩：球内为地形，球外为空。
///
/// `QueryObstruction` 用解析法求线段与球的交点，给出进入球面前的安全比例；
/// `IsInside` 只供测试断言"相机没有落在地形内部"。
class SphereTerrain : public ITerrainQuery {
public:
    SphereTerrain(glm::vec3 center, float radius) : m_center(center), m_radius(radius) {}

    [[nodiscard]] bool QueryHeight(float worldX, float worldZ, float& outHeight) const override {
        const float dx          = worldX - m_center.x;
        const float dz          = worldZ - m_center.z;
        const float horizontalSq = m_radius * m_radius - dx * dx - dz * dz;
        if (horizontalSq <= 0.0F) {
            return false;  // 该列没有地表
        }
        outHeight = m_center.y + std::sqrt(horizontalSq);
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& from, const glm::vec3& to,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;

        const glm::vec3 delta  = to - from;
        const glm::vec3 offset = from - m_center;

        const float a = glm::dot(delta, delta);
        if (a <= 0.0F) {
            return false;
        }
        const float c = glm::dot(offset, offset) - m_radius * m_radius;
        if (c < 0.0F) {
            outSafeT = 0.0F;  // 起点已在实心体内：不得前进
            return true;
        }

        const float b            = 2.0F * glm::dot(offset, delta);
        const float discriminant = b * b - 4.0F * a * c;
        if (discriminant <= 0.0F) {
            return false;  // 与球无交点（或相切）
        }

        const float enterT = (-b - std::sqrt(discriminant)) / (2.0F * a);
        if (enterT < 0.0F || enterT > 1.0F) {
            return false;  // 交点不在线段上
        }

        outSafeT = enterT;
        return true;
    }

    [[nodiscard]] bool IsInside(const glm::vec3& point) const {
        return glm::length(point - m_center) < m_radius;
    }

private:
    glm::vec3 m_center;
    float     m_radius = 0.0F;
};

/// 无限水平地面（高度恒为 0），且永不报告线段遮挡——只用来验证"离地间隙"安全网。
class FlatGround : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        return false;
    }
};

/// 退化地形桩：线段**从起点就被完全挡住**（`safeT = 0`），地表恒在 y = 0。
///
/// 这正是"把地表堆到注视点之上"时相机遇到的情形：避障会把跟随距离压到 0。
class FullyBlockedAtStartGround : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 0.0F;
        return true;
    }
};

/// 视图矩阵是否逐元素有限（出现 NaN / Inf 即为"整帧几何失效"）。
[[nodiscard]] bool IsFinite(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// 俯仰角必须被钳制在 ±89°：超限值的设置与增量调整都不能突破上下限。
TEST(ThirdPersonCamera, PitchIsClampedToPlusMinusEightyNineDegrees) {
    EXPECT_NEAR(kCameraPitchLimit, glm::radians(89.0F), 1e-6F);

    ThirdPersonCamera camera;

    camera.SetPitch(glm::radians(179.0F));
    EXPECT_NEAR(camera.Pitch(), kCameraPitchLimit, 1e-6F);

    camera.SetPitch(glm::radians(-179.0F));
    EXPECT_NEAR(camera.Pitch(), -kCameraPitchLimit, 1e-6F);

    camera.SetPitch(0.0F);
    camera.AddPitch(glm::radians(1000.0F));
    EXPECT_NEAR(camera.Pitch(), kCameraPitchLimit, 1e-6F);

    camera.AddPitch(glm::radians(-5000.0F));
    EXPECT_NEAR(camera.Pitch(), -kCameraPitchLimit, 1e-6F);

    // 边界值本身也要能稳定保持
    camera.SetPitch(kCameraPitchLimit);
    EXPECT_FLOAT_EQ(camera.Pitch(), kCameraPitchLimit);
    camera.SetPitch(-kCameraPitchLimit);
    EXPECT_FLOAT_EQ(camera.Pitch(), -kCameraPitchLimit);
}

// T14 回归（与缺陷 B2 同源）：反复施加**极大幅度**的 AddPitch（两方向各若干次）后，俯仰必须**恰好**
// 停在 ±89°；且两个极限处的视图矩阵（含视图投影）必须逐元素有限。
// 这正是"±89° 必须可达、但绝不能到 90°"的门禁：到 ±90° 时视线与世界上方向平行，
// `glm::lookAt` 归一化得 NaN，画面会只剩清屏色。
TEST(ThirdPersonCamera, ExtremeRepeatedPitchClampsExactlyAndStaysFinite) {
    ThirdPersonCamera camera;
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(1.1F);

    // 向上猛拉：每次都远超剩余行程，钳制必须精确停在 +kCameraPitchLimit（不是"接近"）。
    for (int i = 0; i < 8; ++i) {
        camera.AddPitch(glm::radians(100000.0F));
    }
    EXPECT_FLOAT_EQ(camera.Pitch(), kCameraPitchLimit);
    const CameraView up = camera.Evaluate(0.0, nullptr);
    EXPECT_TRUE(IsFinite(up.view));
    EXPECT_TRUE(IsFinite(up.viewProjection));
    EXPECT_TRUE(IsFinite(up.projection));

    // 向下猛拉：精确停在 -kCameraPitchLimit。
    for (int i = 0; i < 8; ++i) {
        camera.AddPitch(glm::radians(-100000.0F));
    }
    EXPECT_FLOAT_EQ(camera.Pitch(), -kCameraPitchLimit);
    const CameraView down = camera.Evaluate(0.0, nullptr);
    EXPECT_TRUE(IsFinite(down.view));
    EXPECT_TRUE(IsFinite(down.viewProjection));
    EXPECT_TRUE(IsFinite(down.projection));
}

// 无遮挡时，相机必须恰好落在请求的跟随距离上（不被安全网挪动）。
TEST(ThirdPersonCamera, SitsAtRequestedDistanceWhenUnobstructed) {
    CameraSettings settings;
    settings.followDistance = 6.0F;
    settings.pivotHeight    = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 5.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);

    // 球体远在视线之外，既不遮挡也不提供地表高度
    const SphereTerrain terrain(glm::vec3(0.0F, 0.0F, -50.0F), 1.0F);
    const CameraView    view = camera.Evaluate(0.0, &terrain);

    EXPECT_NEAR(view.distance, settings.followDistance, 1e-4F);
    // yaw = 0、pitch = 0 → 视线朝 +Z，相机在注视点后方（-Z）
    EXPECT_NEAR(view.eye.x, 0.0F, 1e-4F);
    EXPECT_NEAR(view.eye.y, 0.0F, 1e-4F);
    EXPECT_NEAR(view.eye.z, 5.0F - settings.followDistance, 1e-4F);
    EXPECT_FALSE(terrain.IsInside(view.eye));
}

// 有遮挡时相机必须被拉近，且最终位置不能落在地形内部。
TEST(ThirdPersonCamera, IsPulledCloserAndStaysOutsideTerrainWhenObstructed) {
    CameraSettings settings;
    settings.followDistance  = 6.0F;
    settings.pivotHeight     = 0.0F;
    settings.collisionMargin = 0.2F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 5.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);

    // 球心在原点、半径 3：期望视线从 (0,0,5) 指向 (0,0,-1)，必然穿过球体
    const SphereTerrain terrain(glm::vec3(0.0F, 0.0F, 0.0F), 3.0F);
    const glm::vec3     pivot  = glm::vec3(0.0F, 0.0F, 5.0F);
    const glm::vec3     desiredEye = glm::vec3(0.0F, 0.0F, 5.0F - settings.followDistance);

    float safeT = 1.0F;
    ASSERT_TRUE(terrain.QueryObstruction(pivot, desiredEye, safeT));
    ASSERT_LT(safeT, 1.0F);

    const CameraView view = camera.Evaluate(0.0, &terrain);

    // 被拉近到安全比例处，并预留了余量
    EXPECT_LT(view.distance, settings.followDistance);
    EXPECT_NEAR(view.distance, settings.followDistance * safeT - settings.collisionMargin, 1e-4F);

    // 关键不变量：相机不在实心体内，且停在球面之外
    EXPECT_FALSE(terrain.IsInside(view.eye));
    EXPECT_GT(view.eye.z, 3.0F);
}

// 即便线段查询没报告遮挡，视线也不得低于地表 + 离地间隙。
TEST(ThirdPersonCamera, NeverDropsBelowGroundClearance) {
    CameraSettings settings;
    settings.followDistance  = 5.0F;
    settings.pivotHeight     = 0.0F;
    settings.groundClearance = 0.5F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(glm::radians(30.0F));  // 向上看 → 相机被压到注视点下方

    const FlatGround terrain;
    const CameraView view = camera.Evaluate(0.0, &terrain);

    EXPECT_GE(view.eye.y, settings.groundClearance - 1e-5F);
    EXPECT_NEAR(view.eye.y, settings.groundClearance, 1e-4F);
}

// 应用渲染插值系数只影响渲染输出，绝不回写模拟状态（红线 11）。
TEST(ThirdPersonCamera, InterpolationAlphaDoesNotMutateSimulationState) {
    ThirdPersonCamera camera;
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.Advance(glm::vec3(10.0F, 0.0F, 0.0F));  // 上一个 = 0，当前 = 10
    camera.SetYaw(0.5F);
    camera.SetPitch(0.25F);
    camera.SetFollowDistance(7.0F);

    const glm::vec3 previousTarget = camera.TargetPrevious();
    const glm::vec3 currentTarget  = camera.TargetCurrent();
    const float     yaw            = camera.Yaw();
    const float     pitch          = camera.Pitch();
    const float     distance       = camera.FollowDistance();

    const double alphas[] = { -1.0, 0.0, 0.25, 0.5, 1.0, 2.0 };
    for (const double alpha : alphas) {
        const CameraView view = camera.Evaluate(alpha, nullptr);
        (void)view;  // 只关心副作用：调用后状态必须一字不变

        EXPECT_EQ(camera.TargetPrevious(), previousTarget);
        EXPECT_EQ(camera.TargetCurrent(), currentTarget);
        EXPECT_FLOAT_EQ(camera.Yaw(), yaw);
        EXPECT_FLOAT_EQ(camera.Pitch(), pitch);
        EXPECT_FLOAT_EQ(camera.FollowDistance(), distance);
    }
}

// 渲染插值确实发生在"上一目标 → 当前目标"之间，且越界 alpha 被钳制（不外插）。
TEST(ThirdPersonCamera, PivotInterpolatesBetweenLogicSteps) {
    CameraSettings settings;
    settings.pivotHeight = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.Advance(glm::vec3(10.0F, 0.0F, 0.0F));

    EXPECT_NEAR(camera.Evaluate(0.0, nullptr).target.x, 0.0F, 1e-5F);
    EXPECT_NEAR(camera.Evaluate(0.5, nullptr).target.x, 5.0F, 1e-5F);
    EXPECT_NEAR(camera.Evaluate(1.0, nullptr).target.x, 10.0F, 1e-5F);

    // 越界不产生外插
    EXPECT_NEAR(camera.Evaluate(-1.0, nullptr).target.x, 0.0F, 1e-5F);
    EXPECT_NEAR(camera.Evaluate(2.0, nullptr).target.x, 10.0F, 1e-5F);
}

// 缺陷 B2 回归：避障把跟随距离压到 0 时，相机**不得**退化出非有限视图矩阵。
//
// 退化路径（蓝屏根因）：`safeT = 0` ⇒ `distance = 0` ⇒ `eye == target`，`glm::lookAt` 的视线基向量成为
// 零向量；随后"离地间隙"安全网把 `eye` 顶到 `target` 正上方 ⇒ 视线又与世界上方向平行。
// 两者都会让视图矩阵变成 NaN，整帧几何被丢弃，画面只剩清屏色。
TEST(ThirdPersonCamera, DegenerateViewIsAvoidedWhenFollowDistanceCollapses) {
    CameraSettings settings;
    settings.followDistance  = 14.0F;  // 与 game/main.cpp 一致
    settings.pivotHeight     = 1.6F;
    settings.groundClearance = 0.2F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.7F);
    camera.SetPitch(-0.42F);

    const FullyBlockedAtStartGround terrain;
    const CameraView                view = camera.Evaluate(0.0, &terrain);

    // 相机始终与注视点保持一个正的最小距离（否则视线基向量退化）。
    EXPECT_GE(glm::length(view.eye - view.target), kCameraMinDistance - 1e-5F);
    EXPECT_GT(view.distance, 0.0F);

    // 视图与视图投影矩阵必须逐元素有限，否则整帧几何都会变成 NaN。
    EXPECT_TRUE(IsFinite(view.view));
    EXPECT_TRUE(IsFinite(view.viewProjection));
}

// 最小距离托底不得改变**正常（无遮挡）**情况下的跟随距离。
TEST(ThirdPersonCamera, MinDistanceGuardKeepsRequestedDistanceWhenUnobstructed) {
    CameraSettings settings;
    settings.followDistance = 14.0F;
    settings.pivotHeight    = 1.6F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.7F);
    camera.SetPitch(-0.42F);

    const FlatGround terrain;  // 不报告遮挡
    const CameraView view = camera.Evaluate(0.0, &terrain);

    EXPECT_NEAR(view.distance, settings.followDistance, 1e-4F);
    EXPECT_TRUE(IsFinite(view.viewProjection));
}

