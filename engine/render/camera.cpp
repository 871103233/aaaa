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

/// 相机被埋在实心体内时，**沿视线朝注视点收缩悬臂**的步长（格）与最大步数。
///
/// 为什么是收缩而不是"向上顶"：狭小空间（洞 / 水道 / 窄缝）里实心在相机身后或上方，向上顶会把相机**穿过洞顶**
/// 抬到地表之上 ⇒ 玩家从"地图外"看世界；或把相机抬到注视点**正上方** ⇒ 视线变垂直向下（俯视，缺陷 W6c）。
/// 注视点在角色头部（空气），沿视线收缩一定能回到空气，且相机**始终留在悬臂线上、朝向不变**，
/// 也始终留在角色所在的空间里（UE5 `USpringArmComponent` 的 boom 碰撞同口径）。
constexpr float kSolidPullStepBlocks = 0.25F;
constexpr int   kMaxSolidPullSteps    = 64;

/// 把 `distance` 沿悬臂收缩到"相机不落在实心体内"的最大安全距离（W6b / W6c 的安全网）。
///
/// **只沿悬臂收缩、绝不改变朝向**。`distance` 已是安全值则原样返回（常见情形：零次迭代）。
[[nodiscard]] float pullToSafeDistance(const glm::vec3& pivot, const glm::vec3& backward, float distance,
                                       const ITerrainQuery& terrain) noexcept {
    glm::vec3 eye = pivot + backward * distance;
    for (int step = 0; step < kMaxSolidPullSteps && terrain.IsSolid(eye) && distance > kCameraMinDistance; ++step) {
        distance = std::max(kCameraMinDistance, distance - kSolidPullStepBlocks);
        eye      = pivot + backward * distance;
    }
    return distance;
}

}  // namespace

ThirdPersonCamera::ThirdPersonCamera(CameraSettings settings) noexcept : m_settings(settings) {}

void ThirdPersonCamera::Advance(const glm::vec3& targetPosition) noexcept {
    m_targetPrevious = m_targetCurrent;
    m_targetCurrent  = targetPosition;
}

void ThirdPersonCamera::SnapTo(const glm::vec3& targetPosition) noexcept {
    m_targetPrevious = targetPosition;
    m_targetCurrent  = targetPosition;
    // W6h：传送 / 初始化 ⇒ 避障平滑状态一并重置（下一帧 `UpdateAvoidance` 直接吸附，不产生"拖影式"回拉）。
    m_avoidanceDistance   = -1.0F;
    m_avoidanceClearTimer = 0.0F;
}

void ThirdPersonCamera::UpdateAvoidance(float frameDt, double alpha, const ITerrainQuery* terrain) noexcept {
    const float     clampedAlpha = clamp_alpha(alpha);
    const glm::vec3 backward     = -Forward();
    const glm::vec3 pivot        = PivotAt(clampedAlpha);
    const float     follow       = m_settings.followDistance;

    // ① 期望距离 = 带半径的遮挡查询（拉近）+ 安全网（不得埋在实心内），并夹进 [kCameraMinDistance, follow]。
    float target = follow;
    if (terrain != nullptr && follow > 0.0F) {
        const glm::vec3 desiredEye = pivot + backward * follow;
        float           safeT      = 1.0F;
        if (terrain->QueryObstructionWithRadius(pivot, desiredEye, m_settings.cameraProbeRadius, safeT)) {
            const float clampedT = std::clamp(safeT, 0.0F, 1.0F);
            target               = std::max(0.0F, follow * clampedT - m_settings.collisionMargin);
        }
        target = std::clamp(target, kCameraMinDistance, follow);
        target = pullToSafeDistance(pivot, backward, target, *terrain);
    }

    // ② 首次（或未初始化）：直接吸附 —— 无历史则无平滑可言。
    if (!(m_avoidanceDistance >= 0.0F)) {
        m_avoidanceDistance   = target;
        m_avoidanceClearTimer = 0.0F;
        return;
    }

    constexpr float kEpsilon = 1.0e-4F;

    // ③ **拉近：立即**（安全优先 —— 相机绝不允许留在墙里），并清零迟滞计时。
    if (target < m_avoidanceDistance - kEpsilon) {
        m_avoidanceDistance   = target;
        m_avoidanceClearTimer = 0.0F;
        return;
    }
    if (target <= m_avoidanceDistance + kEpsilon) {
        return;  // 已在目标处：不动
    }

    // ④ **推远：先迟滞，再指数平滑 + 限速**（进快出慢的非对称阻尼，治临界点横跳闪烁）。
    if (!(frameDt > 0.0F)) {
        return;  // 无帧时间 ⇒ 本帧不推进
    }
    m_avoidanceClearTimer += frameDt;
    if (m_avoidanceClearTimer < m_settings.avoidanceClearHold) {
        return;  // 迟滞窗口内**不回推**（关键：临界点反复横跳时距离保持不动）
    }

    float next = target;  // 阻尼 = 0 ⇒ 立即回推（旧行为）
    if (m_settings.avoidanceExtendDamping > 0.0F) {
        const float k = 1.0F - std::exp(-frameDt / m_settings.avoidanceExtendDamping);  // 帧率无关
        next          = m_avoidanceDistance + (target - m_avoidanceDistance) * k;
    }
    if (m_settings.avoidanceExtendMaxSpeed > 0.0F) {
        next = std::min(next, m_avoidanceDistance + m_settings.avoidanceExtendMaxSpeed * frameDt);
    }
    m_avoidanceDistance = std::min(next, target);  // 绝不越过目标
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
    // W6g：肩位偏移 —— 注视点沿**相机右方**平移（over-the-shoulder）。水平前向 = (sin yaw, 0, cos yaw)，
    // 故右方 = (cos yaw, 0, −sin yaw)（两者正交）。**只平移注视点，不改变朝向**：
    // 悬臂方向仍由 yaw / pitch 决定 ⇒ `normalize(eye − target)` 不变。
    if (m_settings.shoulderOffset != 0.0F) {
        pivot.x += std::cos(m_yaw) * m_settings.shoulderOffset;
        pivot.z += -std::sin(m_yaw) * m_settings.shoulderOffset;
    }
    return pivot;
}

CameraView ThirdPersonCamera::Evaluate(double alpha, const ITerrainQuery* terrain) const noexcept {
    const float     clampedAlpha = clamp_alpha(alpha);
    const glm::vec3 forward      = Forward();   // 单位视线方向
    const glm::vec3 backward     = -forward;    // 相机位于注视点后方

    CameraView view;
    view.target = PivotAt(clampedAlpha);

    float distance = m_settings.followDistance;

    if (m_avoidanceDistance >= 0.0F) {
        // W6h：使用 `UpdateAvoidance` 推进出的**平滑后**距离 —— 不再逐帧按遮挡的布尔翻转而瞬间跳变
        //（临界点反复横跳 ⇒ 画面闪烁的根因）。
        distance = std::clamp(m_avoidanceDistance, 0.0F, m_settings.followDistance);
    } else if (terrain != nullptr && distance > 0.0F) {
        // 旧路径（未启用平滑 = W6h 之前的瞬时行为）：沿视线做**带半径**的遮挡查询（W6f 球投射探针：
        // 相机近似为半径 `cameraProbeRadius` 的球，薄墙不再从旁"擦过"漏检），把相机拉近到安全比例处，并预留余量。
        const glm::vec3 desiredEye = view.target + backward * distance;
        float           safeT      = 1.0F;
        if (terrain->QueryObstructionWithRadius(view.target, desiredEye, m_settings.cameraProbeRadius, safeT)) {
            const float clampedT = std::clamp(safeT, 0.0F, 1.0F);
            distance             = std::max(0.0F, distance * clampedT - m_settings.collisionMargin);
            distance             = std::min(distance, m_settings.followDistance);  // 只会拉近，绝不拉远
        }
    }

    // 不变量：`eye` 与 `target` 的间距恒不小于 `kCameraMinDistance`。
    //
    // 遮挡可能把 distance 压到 0，使 `eye == target`、视线基向量退化为零；`glm::lookAt` 归一化得到 NaN，
    // 视图矩阵失效、整帧几何被丢弃，画面只剩清屏色（缺陷 B2）。先保证一个正的跟随距离，
    // 则 `backward` 的水平分量（`cos(pitch) >= cos(89°) > 0`）保证横向偏移恒非零。
    if (distance < kCameraMinDistance) {
        distance = kCameraMinDistance;
    }

    glm::vec3 eye = view.target + backward * distance;

    // 安全网：相机不得停留在**实心**体内。
    //
    // 唯一手段 = **沿视线朝注视点收缩悬臂**：狭小空间（洞 / 水道 / 窄缝）里实心就在相机身后或上方，
    // 把相机朝注视点拉近就能回到空气里，且**相机始终留在悬臂线上**（朝向恒由玩家 yaw / pitch 决定、
    // 不因避障改变），也始终留在角色所在的空间里 —— 与 UE5 `USpringArmComponent` 的 boom 碰撞同口径。
    //
    // **不得**用"向上顶"之类的兜底：那会把相机穿过洞顶抬到地表之上 ⇒ 玩家从"地图外"看世界；
    // 或在窄缝里把相机抬到注视点正上方 ⇒ 视线变垂直向下（俯视）—— 这正是缺陷 W6c（人工实测：
    // "进入狭窄地方会锁定镜头为俯视"）。收缩极限 = `kCameraMinDistance`（0.2，对齐 Cinemachine）。
    //
    // 判据必须是"该点是否实心"（`ITerrainQuery::IsSolid`），**不能**用"该列地表高度"：
    // 可挖体积挖出的洞在地表高度场里**仍然显示为实心**（爆炸只改体积密度、不改高度场），
    // 拿高度场当"无限地板"会把站在洞里的角色的相机顶到旧地表之上（人工实测第 7 轮）。
    // 这与 ADR 0011 / 0012 的「谁来画 / 谁来挡必须同源」是同一个原则。
    // 本安全网**不做平滑**（安全优先，两条路径都生效）：它只会把距离**再收紧**，不会造成回推抖动。
    if (terrain != nullptr) {
        distance = pullToSafeDistance(view.target, backward, distance, *terrain);
        eye      = view.target + backward * distance;
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

bool ShouldHideFollowTarget(const CameraView& view, const CameraSettings& settings) noexcept {
    return view.distance < settings.targetHideDistance;
}

float FollowTargetFadeOpacity(const CameraView& view, const CameraSettings& settings) noexcept {
    const float start = settings.targetFadeStartDistance;
    const float end   = settings.targetFadeEndDistance;
    if (!(view.distance < start)) {
        return 1.0F;  // 含 NaN 与"远于起点"：完全不透明
    }
    if (!(start > end)) {
        return 0.0F;  // 区间退化（含 NaN）：阶跃
    }
    if (view.distance <= end) {
        return 0.0F;
    }
    return (view.distance - end) / (start - end);
}

}  // namespace vx
