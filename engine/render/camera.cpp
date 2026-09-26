#include "render/camera.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vx {
namespace {

/// 把渲染插值系数钳制到 [0, 1]，非值（NaN）按 0 处理；不做外插。
[[nodiscard]] float clamp_alpha(double alpha) noexcept {
    if (!(alpha > 0.0)) {  // 同时覆盖 NaN 与负值
        return 0.0F;
    }
    if (alpha > 1.0) {
        return 1.0F;
    }
    return static_cast<float>(alpha);
}

[[nodiscard]] glm::vec3 world_up() noexcept {
    return glm::vec3(0.0F, 1.0F, 0.0F);
}

/// 相机被埋在实心体内时，沿 +Y "顶出实心"的步长（格）。
///
/// 用固定步长的离散顶出而不是解析求交：只依赖 `ITerrainQuery::IsSolid` 这一个点查询
/// （无需世界层再暴露"该列实心顶面"），且步长固定 ⇒ 结果确定（红线 7）。
constexpr float kSolidLiftStepBlocks = 0.25F;

/// 顶出的最大步数（0.25 × 64 = 16 格）：防止异常数据下无限循环。
constexpr int kMaxSolidLiftSteps = 64;

}  // namespace

ThirdPersonCamera::ThirdPersonCamera(CameraSettings settings) noexcept : m_settings(settings) {}

void ThirdPersonCamera::Advance(const glm::vec3& targetPosition) noexcept {
    m_targetPrevious = m_targetCurrent;
    m_targetCurrent  = targetPosition;
}

void ThirdPersonCamera::SnapTo(const glm::vec3& targetPosition) noexcept {
    m_targetPrevious = targetPosition;
    m_targetCurrent  = targetPosition;
}

void ThirdPersonCamera::SetYaw(float yawRadians) noexcept {
    m_yaw = yawRadians;
}

void ThirdPersonCamera::AddYaw(float deltaRadians) noexcept {
    m_yaw += deltaRadians;
}

void ThirdPersonCamera::SetPitch(float pitchRadians) noexcept {
    m_pitch = std::clamp(pitchRadians, -kCameraPitchLimit, kCameraPitchLimit);
}

void ThirdPersonCamera::AddPitch(float deltaRadians) noexcept {
    SetPitch(m_pitch + deltaRadians);
}

void ThirdPersonCamera::SetFollowDistance(float distance) noexcept {
    m_settings.followDistance = std::max(distance, 0.0F);
}

void ThirdPersonCamera::SetAspectRatio(float aspectRatio) noexcept {
    m_settings.aspectRatio = aspectRatio;
}

glm::vec3 ThirdPersonCamera::Forward() const noexcept {
    const float cosPitch = std::cos(m_pitch);
    return glm::vec3(cosPitch * std::sin(m_yaw), std::sin(m_pitch), cosPitch * std::cos(m_yaw));
}

glm::vec3 ThirdPersonCamera::PivotAt(float alpha) const noexcept {
    glm::vec3 pivot = glm::mix(m_targetPrevious, m_targetCurrent, alpha);
    pivot.y += m_settings.pivotHeight;
    return pivot;
}

CameraView ThirdPersonCamera::Evaluate(double alpha, const ITerrainQuery* terrain) const noexcept {
    const float     clampedAlpha = clamp_alpha(alpha);
    const glm::vec3 forward      = Forward();   // 单位视线方向
    const glm::vec3 backward     = -forward;    // 相机位于注视点后方

    CameraView view;
    view.target = PivotAt(clampedAlpha);

    float     distance    = m_settings.followDistance;
    glm::vec3 eye         = view.target + backward * distance;
    const float unobstructedDistance = distance;

    // 避障：沿视线查询遮挡，把相机拉近到安全比例处，并预留余量。
    if (terrain != nullptr && distance > 0.0F) {
        float safeT = 1.0F;
        if (terrain->QueryObstruction(view.target, eye, safeT)) {
            const float clampedT = std::clamp(safeT, 0.0F, 1.0F);
            distance             = std::max(0.0F, distance * clampedT - m_settings.collisionMargin);
            distance             = std::min(distance, unobstructedDistance);  // 只会拉近，绝不拉远
            eye                  = view.target + backward * distance;
        }
    }

    // 不变量：`eye` 与 `target` 的间距恒不小于 `kCameraMinDistance`。
    //
    // 为什么必须在**用离地间隙抬高 eye 之前**托底：遮挡可能把 distance 压到 0，使 `eye == target`，
    // 视线基向量退化为零；随后"离地间隙"只会抬高 `eye.y`，若此时 eye 与 target 的水平偏移也为 0，
    // 视线方向就与世界上方向**平行**，`glm::lookAt` 归一化得到 NaN，视图矩阵失效、整帧几何被丢弃，
    // 画面只剩清屏色（缺陷 B2）。先保证一个正的跟随距离，则 `backward` 的水平分量
    // （`cos(pitch) >= cos(89°) > 0`）保证横向偏移恒非零，抬高纵坐标不会再造成退化。
    if (distance < kCameraMinDistance) {
        distance = kCameraMinDistance;
        eye      = view.target + backward * distance;
    }

    // 安全网：相机不得停留在**实心**体内。
    //
    // 判据必须是"该点是否实心"（`ITerrainQuery::IsSolid`），**不能**用"该列地表高度"：
    // 可挖体积挖出的洞在地表高度场里**仍然显示为实心**（爆炸只改体积密度、不改高度场），
    // 拿高度场当"无限地板"会把站在洞里的角色的相机顶到旧地表之上 ⇒ 视角退化为俯视
    //（人工实测第 7 轮）。这条与 ADR 0011 / 0012 的「谁来画 / 谁来挡必须同源」是同一个原则：
    // 能挖出洞的地方，相机也不能被"已经不存在的旧地表"顶出来。
    if (terrain != nullptr && terrain->IsSolid(eye)) {
        for (int step = 0; step < kMaxSolidLiftSteps && terrain->IsSolid(eye); ++step) {
            eye.y += kSolidLiftStepBlocks;
        }
        eye.y += m_settings.groundClearance;
    }

    view.eye      = eye;
    view.distance = glm::length(eye - view.target);
    view.view     = glm::lookAt(eye, view.target, world_up());
    view.projection =
        glm::perspectiveRH_ZO(glm::radians(m_settings.fieldOfViewDegrees), m_settings.aspectRatio,
                              m_settings.nearPlane, m_settings.farPlane);
    view.viewProjection = view.projection * view.view;
    return view;
}

}  // namespace vx
