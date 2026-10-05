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
using vx::ShouldHideFollowTarget;
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

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override { return IsInside(point); }

    [[nodiscard]] bool IsInside(const glm::vec3& point) const {
        return glm::length(point - m_center) < m_radius;
    }

private:
    glm::vec3 m_center;
    float     m_radius = 0.0F;
};

/// 无限水平地面（高度恒为 0，`y <= 0` 即实心），且**永不报告线段遮挡**——
/// 只用来验证"相机在实心体内时**沿悬臂收缩**"的安全网（遮挡由线段查询兜住，这里刻意让线段查询闭嘴）。
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

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override { return point.y <= 0.0F; }
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

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override { return point.y <= 0.0F; }
};

/// 洞穴地形桩（人工实测第 7 轮）：**地表高度恒为 0**（= 高度场"不知道洞的存在"），
/// 但其下一处 8×8×6 的立方空腔（模拟可挖体积挖出的洞）在 `IsSolid` 里报告为**空**。
///
/// 这正是修复前"相机被顶出洞外 ⇒ 视角退化为俯视"的数据条件。
class CaveTerrain : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;  // 旧地表：被挖掉却仍报出高度的那份数据
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        return false;  // 洞内畅通：体积接管后不再有"假遮挡"
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override {
        if (point.y > 0.0F) {
            return false;  // 地表之上是空气
        }
        const bool insideCave = point.y >= -6.0F && std::abs(point.x) <= 8.0F && std::abs(point.z) <= 8.0F;
        return !insideCave;
    }
};

/// 带**洞顶**的洞穴桩（W6b）：空腔 = `|x| ≤ 6、|z| ≤ 6、-2 ≤ y ≤ 2`，其余为实心。
///
/// `QueryObstruction` **总是报告不遮挡**（模拟"单射线在狭小空间里没查到"的情形）⇒ 只能靠安全网
/// 把相机收回洞内。修复前安全网只会**向上顶**，会把相机穿过洞顶抬到地表之上（"看到地图外"）。
class CaveWithCeilingTerrain : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 2.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        return false;
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override {
        const bool insideAir = std::abs(point.x) <= 6.0F && std::abs(point.z) <= 6.0F && point.y >= -2.0F &&
                               point.y <= 2.0F;
        return !insideAir;
    }
};

/// **窄隧道**桩（W6b）：`y ≤ 2` 是空气、其余实心；线段查询**恒报"起点即被挡"**（`safeT = 0`）
/// ⇒ 避障把悬臂压到最小距离（相机贴近主角）。
class NarrowTunnelTerrain : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 2.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 0.0F;
        return true;
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override { return point.y > 2.0F; }
};

/// **紧身气室**桩（W6c）：只有以原点为中心、半径 0.35 格的球形空间是空气，其余全为实心。
///
/// 代表"角色被窄缝 / 水道裹住"的极端情形：**整根悬臂（含最小距离处）都在实心里**，且线段遮挡查询
/// 没查到（`QueryObstruction` 恒报不遮挡）⇒ 只能靠 `IsSolid` 安全网。
/// 缺陷 W6c 的机制：最小距离托底（旧值 0.5）处仍是实心 ⇒ 收缩循环卡死 ⇒ 落到"向上顶"兜底
/// ⇒ 相机被抬到注视点**正上方** ⇒ 视线变为垂直向下（俯视），且每帧重复 ⇒ "锁定俯视"。
class TightPocketTerrain : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;
        return false;  // 气室被实心裹住，没有可用的地表高度
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        return false;  // 单射线在窄缝里漏检
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override {
        return glm::length(point) > kAirRadius;
    }

private:
    static constexpr float kAirRadius = 0.35F;
};

/// **球探针契约**桩（W6f）：无半径的 `QueryObstruction` 恒报畅通；`QueryObstructionWithRadius` 在
/// `radius > 0` 时报告"在 t = 0.5 处被挡"。用于验证相机确实走了**带半径**的查询，
/// 且 `cameraProbeRadius = 0` 时退回**线段**（不被拉近）。
class RadiusOnlyObstruction : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        return false;  // 无半径：畅通
    }

    [[nodiscard]] bool QueryObstructionWithRadius(const glm::vec3& /*from*/, const glm::vec3& /*to*/, float radius,
                                                  float& outSafeT) const override {
        if (radius > 0.0F) {
            outSafeT = 0.5F;
            return true;
        }
        outSafeT = 1.0F;
        return false;
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& /*point*/) const override { return false; }
};

/// **可切换遮挡**桩（W6h）：`blocked` 为真时报告"在安全比例 `safeT` 处被挡"，否则畅通。
/// 用于模拟"玩家在临界点（遮挡刚出现 / 消失）反复横跳"。
class SwitchableObstruction : public ITerrainQuery {
public:
    [[nodiscard]] bool QueryHeight(float /*worldX*/, float /*worldZ*/, float& outHeight) const override {
        outHeight = 0.0F;
        return true;
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                        float& outSafeT) const override {
        outSafeT = blocked ? safeT : 1.0F;
        return blocked;
    }

    [[nodiscard]] bool QueryObstructionWithRadius(const glm::vec3& /*from*/, const glm::vec3& /*to*/,
                                                  float /*radius*/, float& outSafeT) const override {
        outSafeT = blocked ? safeT : 1.0F;
        return blocked;
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& /*point*/) const override { return false; }

    bool  blocked = false;
    float safeT   = 0.5F;
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

// W6c（判据③"仰视扎地"）：向上看时相机被压到注视点下方、扎进地面，必须**沿悬臂朝注视点拉近**
// 回到地面之上 —— 而不是被"向上顶"（旧行为，已删除）。
TEST(ThirdPersonCamera, PullsTheCameraOntoTheGroundAlongTheBoomWhenLookingUp) {
    CameraSettings settings;
    settings.followDistance = 5.0F;
    settings.pivotHeight    = 1.6F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    const float pitch = glm::radians(30.0F);  // 向上看 → 相机被压到注视点下方，扎进地面
    camera.SetPitch(pitch);

    const FlatGround terrain;
    const CameraView view = camera.Evaluate(0.0, &terrain);

    // 沿悬臂拉近的必然结果：相机回到地面之上，且仍在悬臂线上。
    EXPECT_FALSE(terrain.IsSolid(view.eye)) << "相机不得停留在地面之下（实心体）";
    EXPECT_GT(view.eye.y, 0.0F) << "仰视扎地时必须沿悬臂拉近到地面之上";
    EXPECT_LT(view.distance, settings.followDistance) << "悬臂应被收缩";

    const glm::vec3 backward(0.0F, -std::sin(pitch), -std::cos(pitch));  // yaw = 0
    const glm::vec3 boom = glm::normalize(view.eye - view.target);
    EXPECT_NEAR(glm::length(glm::cross(boom, backward)), 0.0F, 1.0e-4F) << "相机必须始终停在悬臂线上";
    EXPECT_TRUE(IsFinite(view.viewProjection));
}

// 人工实测第 7 轮回归：角色站在**体积挖出的洞**里时，相机不得被"旧地表"顶到洞顶之上。
//
// 契约：相机的避障与安全网只关心"哪里是实心"（`ITerrainQuery::IsSolid`，且**必须包含可挖体积**），
//   而不是"该列地表高度"。修复前安全网用 `QueryHeight`（旧地表 = 0）把 eye 强抬到 0.2 格
//   ⇒ 相机跑到洞顶之上、且避障把跟随距离压到最小 ⇒ 视角退化为**俯视**。
// 本桩正是那个数据条件：`QueryHeight` 恒报 0（高度场不知道洞存在），而 `IsSolid` 在洞内报"空"。
TEST(ThirdPersonCamera, DoesNotLiftCameraOutOfExcavatedCave) {
    CameraSettings settings;
    settings.followDistance = 5.0F;
    settings.pivotHeight    = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, -4.0F, 0.0F));  // 角色站在洞里（该列地表高度是 0）
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);  // 水平视角：相机应停在角色正后方、**同一高度**

    const CaveTerrain terrain;
    const CameraView  view = camera.Evaluate(0.0, &terrain);

    EXPECT_NEAR(view.eye.y, -4.0F, 1e-4F) << "洞内相机必须与角色同高，不得被地表高度顶出洞外";
    EXPECT_FALSE(terrain.IsSolid(view.eye)) << "相机不得落在实心体内";
    EXPECT_NEAR(view.distance, settings.followDistance, 1e-4F) << "洞内无遮挡，跟随距离应保持";
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

// W6b 回归（"看到地图外"）：相机被**洞顶**挤住时，必须**沿视线收缩悬臂**停在洞内，
// 而不是被"向上顶"穿过洞顶抬到地表之上（旧行为会把 `eye.y` 抬到远超洞顶 = 玩家从地图外看世界）。
TEST(ThirdPersonCamera, PullsCameraAlongTheBoomInsteadOfLiftingItThroughACeiling) {
    CameraSettings settings;
    settings.followDistance = 5.0F;
    settings.pivotHeight    = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(glm::radians(-45.0F));  // 俯视 ⇒ 相机被抬到注视点**上方**，撞上洞顶（y = 2）

    const CaveWithCeilingTerrain terrain;
    const CameraView             view = camera.Evaluate(0.0, &terrain);

    EXPECT_FALSE(terrain.IsSolid(view.eye)) << "相机不得停留在实心体内";
    EXPECT_LT(view.eye.y, 2.0F + 1.0e-3F) << "相机必须留在洞内，不得被顶到洞顶之上（旧行为会穿过洞顶）";
    EXPECT_LT(view.distance, settings.followDistance) << "悬臂应被收缩";
    EXPECT_GE(view.distance, kCameraMinDistance - 1.0e-5F);
    EXPECT_TRUE(IsFinite(view.viewProjection));
}

// W6b（"看到人物内部"）：相机被避障挤到很近时，`ShouldHideFollowTarget` 必须判定**隐藏主角**；
// 开阔处则必须保持可见（否则正常视角下主角会莫名消失）。
TEST(ThirdPersonCamera, HidesFollowTargetWhenTheCameraIsJammedClose) {
    CameraSettings settings;
    settings.followDistance     = 6.0F;
    settings.pivotHeight        = 1.6F;
    settings.targetHideDistance = 1.5F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);

    // 开阔地：距离保持 ⇒ 主角**必须可见**。
    const FlatGround open;
    const CameraView openView = camera.Evaluate(0.0, &open);
    EXPECT_NEAR(openView.distance, settings.followDistance, 1.0e-4F);
    EXPECT_FALSE(ShouldHideFollowTarget(openView, settings));

    // 窄隧道：避障把悬臂压到最小距离（相机贴近主角）⇒ 主角**必须隐藏**，且相机仍在空气里。
    const NarrowTunnelTerrain tunnel;
    const CameraView          tunnelView = camera.Evaluate(0.0, &tunnel);
    EXPECT_LE(tunnelView.distance, settings.targetHideDistance);
    EXPECT_TRUE(ShouldHideFollowTarget(tunnelView, settings));
    EXPECT_FALSE(tunnel.IsSolid(tunnelView.eye)) << "相机仍不得落在实心体内";
    EXPECT_LE(tunnelView.eye.y, 2.0F) << "相机不得被顶到隧道顶之上";
}

// W6c 回归（"窄处锁定俯视"）：窄缝里相机的**朝向不得被改变** —— `normalize(eye − target)` 必须恒等于
// 悬臂方向 `backward`（由玩家 yaw / pitch 决定）。
//
// 缺陷前：最小距离托底（旧值 0.5）处仍为实心 ⇒ 收缩循环卡死 ⇒ "向上顶"兜底把相机抬到注视点**正上方**
// ⇒ 视线变为垂直向下（俯视），且每帧重复 ⇒ 玩家看到"镜头锁定俯视"。
TEST(ThirdPersonCamera, KeepsTheCameraOnTheBoomLineInATightPocket) {
    CameraSettings settings;
    settings.followDistance = 5.0F;
    settings.pivotHeight    = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    const float pitch = glm::radians(-30.0F);
    camera.SetPitch(pitch);

    const TightPocketTerrain terrain;
    const CameraView         view = camera.Evaluate(0.0, &terrain);

    // 悬臂方向（yaw = 0）：backward = (0, −sin(pitch), −cos(pitch))。
    const glm::vec3 backward(0.0F, -std::sin(pitch), -std::cos(pitch));
    const glm::vec3 boom = glm::normalize(view.eye - view.target);

    EXPECT_NEAR(glm::length(glm::cross(boom, backward)), 0.0F, 1.0e-4F)
        << "相机必须始终停在悬臂线上（朝向由玩家决定，不因避障改变）";
    EXPECT_GT(glm::dot(boom, backward), 0.0F) << "收缩方向必须指向注视点，不得反向";
    EXPECT_LT(boom.y, 0.99F) << "相机不得被抬到注视点正上方（旧'向上顶'兜底 ⇒ 垂直俯视）";

    EXPECT_FALSE(terrain.IsSolid(view.eye)) << "相机不得停在实心体内";
    EXPECT_GE(view.distance, kCameraMinDistance - 1.0e-5F);
    EXPECT_LT(view.distance, settings.followDistance) << "悬臂应被收缩到最小距离附近";
    EXPECT_TRUE(IsFinite(view.viewProjection));
}

// W6g 回归（肩位偏移）：注视点沿**相机右方**平移 `shoulderOffset`，且**不改变朝向**
// （`normalize(eye − target)` 仍 = 悬臂方向）——否则"偏移"会退化为"改朝向"。
TEST(ThirdPersonCamera, ShoulderOffsetShiftsThePivotWithoutChangingFacing) {
    CameraSettings settings;
    settings.followDistance = 6.0F;
    settings.pivotHeight    = 0.0F;
    settings.shoulderOffset = 0.6F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    const float pitch = glm::radians(-20.0F);
    camera.SetPitch(pitch);

    const CameraView view = camera.Evaluate(0.0, nullptr);

    // yaw = 0 ⇒ 右方 = (cos 0, 0, −sin 0) = (+1, 0, 0)：注视点应沿 +X 偏移 `shoulderOffset`。
    EXPECT_NEAR(view.target.x, settings.shoulderOffset, 1.0e-4F) << "注视点应沿相机右方偏移";
    EXPECT_NEAR(view.target.z, 0.0F, 1.0e-4F);

    // 朝向不变：eye 仍在注视点的悬臂方向上（yaw = 0 ⇒ backward = (0, −sin pitch, −cos pitch)）。
    const glm::vec3 backward(0.0F, -std::sin(pitch), -std::cos(pitch));
    const glm::vec3 boom = glm::normalize(view.eye - view.target);
    EXPECT_NEAR(glm::length(glm::cross(boom, backward)), 0.0F, 1.0e-4F) << "肩位偏移不得改变朝向";
    EXPECT_GT(glm::dot(boom, backward), 0.0F);
    EXPECT_NEAR(view.distance, settings.followDistance, 1.0e-4F);
}

// W6f 回归（球投射探针）：相机必须走**带半径**的遮挡查询 —— 半径 > 0 时按桩报告被拉近；
// 半径 = 0 时退回**线段**查询（桩报畅通）⇒ 距离保持。这是"薄墙不再从相机旁擦过漏检"的接线判据。
TEST(ThirdPersonCamera, UsesRadiusAwareObstructionWhenProbeRadiusIsSet) {
    CameraSettings settings;
    settings.followDistance  = 6.0F;
    settings.pivotHeight     = 0.0F;
    settings.collisionMargin = 0.2F;

    const RadiusOnlyObstruction terrain;

    settings.cameraProbeRadius = 0.25F;
    ThirdPersonCamera withProbe(settings);
    withProbe.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    withProbe.SetYaw(0.0F);
    withProbe.SetPitch(0.0F);
    const CameraView probeView = withProbe.Evaluate(0.0, &terrain);
    EXPECT_LT(probeView.distance, settings.followDistance) << "带半径的查询报告遮挡 ⇒ 相机应被拉近";
    EXPECT_NEAR(probeView.distance, settings.followDistance * 0.5F - settings.collisionMargin, 1.0e-4F);

    settings.cameraProbeRadius = 0.0F;
    ThirdPersonCamera withoutProbe(settings);
    withoutProbe.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    withoutProbe.SetYaw(0.0F);
    withoutProbe.SetPitch(0.0F);
    const CameraView plainView = withoutProbe.Evaluate(0.0, &terrain);
    EXPECT_NEAR(plainView.distance, settings.followDistance, 1.0e-4F) << "半径 = 0 应退回线段查询（无遮挡）";
}

// W6e（淡出主角）：`FollowTargetFadeOpacity` 随相机—注视点距离**单调下降**；开阔处 = 1、极近处 = 0。
TEST(ThirdPersonCamera, FadeOpacityDropsMonotonicallyWithCameraDistance) {
    CameraSettings settings;
    settings.targetFadeStartDistance = 1.5F;
    settings.targetFadeEndDistance   = 0.4F;

    CameraView view;

    view.distance = 3.0F;
    EXPECT_FLOAT_EQ(vx::FollowTargetFadeOpacity(view, settings), 1.0F) << "开阔处：完全不透明";
    view.distance = 1.5F;  // 恰在起点
    EXPECT_FLOAT_EQ(vx::FollowTargetFadeOpacity(view, settings), 1.0F);
    view.distance = 0.95F;  // (0.95 − 0.4) / (1.5 − 0.4) = 0.5
    EXPECT_NEAR(vx::FollowTargetFadeOpacity(view, settings), 0.5F, 1.0e-5F);
    view.distance = 0.4F;  // 恰在终点
    EXPECT_FLOAT_EQ(vx::FollowTargetFadeOpacity(view, settings), 0.0F);
    view.distance = 0.2F;  // 比终点更近（= kCameraMinDistance）
    EXPECT_FLOAT_EQ(vx::FollowTargetFadeOpacity(view, settings), 0.0F);

    // 单调不减（距离越小越透明）。
    float previous = 1.0F;
    for (float d = 1.5F; d >= 0.39F; d -= 0.1F) {
        view.distance       = d;
        const float opacity = vx::FollowTargetFadeOpacity(view, settings);
        EXPECT_LE(opacity, previous + 1.0e-6F) << "不透明度必须随距离下降而单调不增";
        previous = opacity;
    }

    // 区间退化（起点 <= 终点）：不得产生 NaN / 反号。
    settings.targetFadeStartDistance = 0.4F;
    settings.targetFadeEndDistance   = 1.5F;
    view.distance                    = 1.0F;
    const float degenerate = vx::FollowTargetFadeOpacity(view, settings);
    EXPECT_TRUE(std::isfinite(degenerate));
    EXPECT_GE(degenerate, 0.0F);
    EXPECT_LE(degenerate, 1.0F);
}

// W6h 回归（迟滞 + 推远阻尼）：遮挡消失后 `avoidanceClearHold` 窗口内**不得回推**；
// 越过迟滞后**平滑**推远（单帧增量 < 全部差额），最终收敛。这是"临界点反复横跳 ⇒ 画面闪烁"的根因对策。
TEST(ThirdPersonCamera, AvoidanceHoldsThenExtendsSmoothlyAfterOcclusionClears) {
    CameraSettings settings;
    settings.followDistance          = 10.0F;
    settings.pivotHeight             = 0.0F;
    settings.collisionMargin         = 0.0F;
    settings.cameraProbeRadius       = 0.0F;
    settings.avoidanceClearHold      = 0.2F;
    settings.avoidanceExtendDamping  = 0.25F;
    settings.avoidanceExtendMaxSpeed = 0.0F;  // 不限速，便于断言"平滑"

    SwitchableObstruction terrain;  // safeT = 0.5 ⇒ 目标距离 = 5
    ThirdPersonCamera     camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);

    // ① 遮挡出现：首帧直接吸附到 10 × 0.5 = 5（无历史 ⇒ 无平滑）。
    terrain.blocked = true;
    camera.UpdateAvoidance(0.016F, 0.0, &terrain);
    EXPECT_NEAR(camera.Evaluate(0.0, &terrain).distance, 5.0F, 1.0e-4F);

    // ② 遮挡消失：迟滞窗口内**保持不动**（临界点横跳不闪的关键）。
    terrain.blocked = false;
    camera.UpdateAvoidance(0.10F, 0.0, &terrain);  // 累计 0.10 < 0.20
    EXPECT_NEAR(camera.Evaluate(0.0, &terrain).distance, 5.0F, 1.0e-4F) << "迟滞窗口内不得回推";

    // ③ 越过迟滞：开始**平滑**推远（绝不瞬间回到跟随距离）。
    camera.UpdateAvoidance(0.15F, 0.0, &terrain);  // 累计 0.25 >= 0.20
    const float afterFirstExtend = camera.Evaluate(0.0, &terrain).distance;
    EXPECT_GT(afterFirstExtend, 5.0F) << "越过迟滞后应开始推远";
    EXPECT_LT(afterFirstExtend, settings.followDistance) << "推远必须平滑，不得瞬间回到跟随距离";

    // ④ 持续清晰 ⇒ 收敛到跟随距离。
    for (int i = 0; i < 200; ++i) {
        camera.UpdateAvoidance(0.016F, 0.0, &terrain);
    }
    EXPECT_NEAR(camera.Evaluate(0.0, &terrain).distance, settings.followDistance, 1.0e-2F);
}

// W6h 回归（拉近立即）：遮挡**出现**的那一帧距离必须**立即**收紧 —— 相机绝不允许留在墙里（安全优先）。
TEST(ThirdPersonCamera, AvoidancePullsInImmediatelyWhenOcclusionAppears) {
    CameraSettings settings;
    settings.followDistance    = 10.0F;
    settings.pivotHeight       = 0.0F;
    settings.collisionMargin   = 0.0F;
    settings.cameraProbeRadius = 0.0F;

    SwitchableObstruction terrain;  // 先畅通
    ThirdPersonCamera     camera(settings);
    camera.SnapTo(glm::vec3(0.0F, 0.0F, 0.0F));
    camera.SetYaw(0.0F);
    camera.SetPitch(0.0F);

    camera.UpdateAvoidance(0.016F, 0.0, &terrain);
    EXPECT_NEAR(camera.Evaluate(0.0, &terrain).distance, 10.0F, 1.0e-4F);

    terrain.blocked = true;  // 遮挡出现（安全比例 0.5 ⇒ 目标 5）
    camera.UpdateAvoidance(0.001F, 0.0, &terrain);  // 极小帧时间：仍须**立即**收紧
    EXPECT_NEAR(camera.Evaluate(0.0, &terrain).distance, 5.0F, 1.0e-4F) << "拉近必须立即，不得有阻尼";
}

