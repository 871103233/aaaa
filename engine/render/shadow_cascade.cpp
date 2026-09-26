#include "render/shadow_cascade.hpp"

#include "render/lighting_table.hpp"

#include <glm/common.hpp>          // glm::min / glm::max
#include <glm/geometric.hpp>       // glm::cross / dot / length / normalize
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt / orthoRH_ZO / radians

#include <algorithm>
#include <cmath>

namespace vx {
namespace {

/// 光空间的正交基：`right` / `lightUp` 都垂直于太阳方向，用于 texel 对齐的量化轴。
///
/// 与 `BuildCascadeLightMatrix` 的视图矩阵一致：视图的 x 轴 = `-right`、y 轴 = `lightUp`，
/// 故沿 `right` / `lightUp` 量化 = 沿视图 x / y 量化（网格对称，符号无影响）。
struct LightBasis {
    glm::vec3 direction;
    glm::vec3 right;
    glm::vec3 up;
};

/// 光方向的竖直分量下限：既避免 `1 / |dir.y|` 除零，也把"太阳贴地平线"时爆炸的投射体扩展量钳成有限值。
constexpr float kCasterMinVerticalComponent = 0.05F;

/// 投射体扩展的水平量：`casterHeight · tanθ`（`tanθ = sqrt(1 - dir.y²) / max(|dir.y|, eps)`）。
/// 与 `BuildCascadeLightMatrix` 内部同一公式；`BuildShadowUniform` 需要它来算覆盖 texel 的尺寸。
[[nodiscard]] float CasterHorizontalExtension(const glm::vec3& sunDirection, float casterHeight) noexcept {
    const float absDirectionY = std::max(std::abs(sunDirection.y), kCasterMinVerticalComponent);
    const float tanTheta =
        std::sqrt(std::max(0.0F, 1.0F - sunDirection.y * sunDirection.y)) / absDirectionY;
    return std::max(casterHeight, 0.0F) * tanTheta;
}

[[nodiscard]] LightBasis MakeLightBasis(const glm::vec3& sunDirection) noexcept {
    LightBasis basis;
    basis.direction = glm::normalize(sunDirection);
    // 参考上向：太阳接近垂直时改用 +X，避免 cross 退化为零向量。
    const glm::vec3 reference = (std::abs(basis.direction.y) > 0.99F) ? glm::vec3(1.0F, 0.0F, 0.0F)
                                                                      : glm::vec3(0.0F, 1.0F, 0.0F);
    basis.right = glm::normalize(glm::cross(basis.direction, reference));
    basis.up    = glm::cross(basis.right, basis.direction);
    return basis;
}

/// 视空间视锥的一个"切片"（`near` ~ `far`）的 8 个角点，变换到**渲染原点相对**坐标系。
void FrustumSlabCornersRelative(const glm::mat4& inverseView, float nearDistance, float farDistance, float tanHalfFov,
                                float aspectRatio, glm::vec3 outCorners[8]) noexcept {
    int index = 0;
    for (int farSide = 0; farSide < 2; ++farSide) {
        const float distance = (farSide == 0) ? nearDistance : farDistance;
        const float halfHeight = distance * tanHalfFov;
        const float halfWidth  = halfHeight * aspectRatio;
        for (int signY = -1; signY <= 1; signY += 2) {
            for (int signX = -1; signX <= 1; signX += 2) {
                // 视空间为右手系：相机看向 -Z，故前方点的 z = -distance。
                const glm::vec4 viewPoint(static_cast<float>(signX) * halfWidth,
                                          static_cast<float>(signY) * halfHeight, -distance, 1.0F);
                outCorners[index] = glm::vec3(inverseView * viewPoint);
                ++index;
            }
        }
    }
}

}  // namespace

std::array<float, kMaxShadowCascades> ComputeCascadeSplits(float nearPlane, float farPlane, int cascadeCount,
                                                           float lambda) noexcept {
    std::array<float, kMaxShadowCascades> splits {};
    splits.fill(farPlane);

    const int count = std::clamp(cascadeCount, 1, kMaxShadowCascades);
    const float ratio = farPlane / nearPlane;

    for (int i = 1; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float logarithmic = nearPlane * std::pow(ratio, t);
        const float uniform     = nearPlane + (farPlane - nearPlane) * t;
        splits[static_cast<std::size_t>(i - 1)] = lambda * logarithmic + (1.0F - lambda) * uniform;
    }
    return splits;
}

float QuantizeTexelWorldSize(float rawTexelWorldSize, int resolution) noexcept {
    // 兜底：非正输入或非法分辨率不量化（调用方不应发生）。
    if (!(rawTexelWorldSize > 0.0F) || resolution <= 0) {
        return rawTexelWorldSize;
    }
    // 向上取整到 2 的幂：2^ceil(log2(raw))。raw 本身是 2 的幂时结果不变。
    return std::exp2(std::ceil(std::log2(rawTexelWorldSize)));
}

float CascadeBlendWeight(float viewDepth, float splitInner, float blendBand) noexcept {
    if (!(blendBand > 0.0F)) {
        return 1.0F;  // cascade_blend = 0：逐字退回单级采样（近级权重恒为 1）
    }
    const float bandInner = splitInner - blendBand;
    if (viewDepth <= bandInner) {
        return 1.0F;  // 带外近侧：纯近级
    }
    if (viewDepth >= splitInner) {
        return 0.0F;  // 带外远侧：纯远级
    }
    const float t = (viewDepth - bandInner) / blendBand;
    return 1.0F - t * t * (3.0F - 2.0F * t);  // smoothstep；与远级权重之和恒为 1
}

glm::mat4 BuildCascadeLightMatrix(const glm::vec3& sunDirection, const glm::vec3& cascadeCenter, float cascadeRadius,
                                  float cascadeTexelSize, float casterHeight) noexcept {
    const LightBasis basis = MakeLightBasis(sunDirection);

    // ---- 投射体扩展（见头文件推导）----
    // 以 |dir.y| 为竖直分量；eps 既避免除零，也把"太阳贴地平线"时爆炸的扩展量钳成有限值（|dir.y| ≥ eps）。
    const float absDirectionY = std::max(std::abs(basis.direction.y), kCasterMinVerticalComponent);
    const float vertical      = std::max(casterHeight, 0.0F);
    // 沿光轴向前延伸量：把"竖直高度 h"按光轴斜率折算（h / cosθ ≥ h·cosθ，保守覆盖）。
    const float axisExtension = vertical / absDirectionY;
    // X/Y 各扩量：竖直位移在光平面内的最大投影 = h·sinθ，这里取更大的保守量 h·tanθ（见 CasterHorizontalExtension）。
    const float halfExtent    = cascadeRadius + CasterHorizontalExtension(basis.direction, vertical);
    const float farDepth      = 2.0F * cascadeRadius + axisExtension;

    // ---- texel 对齐：把中心在光空间 x / y 轴上的投影量化到 texel 网格 ----
    // 量化是"单位对齐"的关键：相机连续移动时，只有跨过 texel 边界才会改变矩阵的平移分量，
    // 否则阴影图会随相机做亚 texel 的斜移，表现为阴影边缘持续抖动（shimmering）。
    // 量化轴（right / up）与扩展无关（扩展只沿光轴与光平面各向同性放大），故仍按原轴量化。
    const float centerX = glm::dot(cascadeCenter, basis.right);
    const float centerY = glm::dot(cascadeCenter, basis.up);
    const float snappedX = std::round(centerX / cascadeTexelSize) * cascadeTexelSize;
    const float snappedY = std::round(centerY / cascadeTexelSize) * cascadeTexelSize;
    const glm::vec3 snappedCenter =
        cascadeCenter + basis.right * (snappedX - centerX) + basis.up * (snappedY - centerY);

    // ---- 视图：眼睛在量化后的中心沿太阳方向外移（半径 + 轴向扩展量），看向中心 ----
    // 用 basis.up 作为 lookAt 的上向：它与视线方向严格正交，不会像世界上向那样在太阳接近垂直时退化。
    const glm::vec3 eye = snappedCenter + basis.direction * (cascadeRadius + axisExtension);
    const glm::mat4 view = glm::lookAt(eye, snappedCenter, basis.up);

    // ---- 正交投影：±halfExtent 的正方形，深度 [0, 2·radius + axisExtension]（右手系 + 0~1 深度）----
    // 扩展量为 0 时逐字退回旧行为：包围球在视图空间的 xy ∈ [-radius, radius]、z ∈ [-2·radius, 0]，
    // 8 个角点全部落在 NDC [-1, 1]³ 内；扩展后盒只变大，原切片角点仍在盒内（覆盖不丢）。
    const glm::mat4 projection =
        glm::orthoRH_ZO(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0F, farDepth);
    return projection * view;
}

ShadowUniform BuildShadowUniform(const LightingTable& table, const glm::mat4& viewRelative, float fieldOfViewDegrees,
                                 float aspectRatio, float nearPlane, float farPlane,
                                 float casterTopRelative) noexcept {
    const ShadowSettings& settings = table.Shadow();

    ShadowUniform uniform {};
    uniform.lightMatrices[0] = glm::mat4(1.0F);
    uniform.lightMatrices[1] = glm::mat4(1.0F);
    uniform.lightMatrices[2] = glm::mat4(1.0F);
    uniform.lightMatrices[3] = glm::mat4(1.0F);
    uniform.splitDistances[0] = farPlane;
    uniform.splitDistances[1] = farPlane;
    uniform.splitDistances[2] = farPlane;
    uniform.splitDistances[3] = farPlane;

    // 相机世界前向：视图矩阵第三列即 -forward。
    const glm::vec3 cameraForward = -glm::vec3(viewRelative[2]);
    uniform.cameraForwardX = cameraForward.x;
    uniform.cameraForwardY = cameraForward.y;
    uniform.cameraForwardZ = cameraForward.z;

    uniform.texelSize    = 1.0F / static_cast<float>(settings.resolution);
    uniform.depthBias    = settings.depthBias;
    uniform.normalOffset = settings.normalOffset;
    uniform.cascadeBlend = settings.cascadeBlend;  // 缺陷 B8：级联过渡带宽度比例（透传给着色器）

    if (!settings.enabled) {
        // 显式关闭：着色器按 enabled 整段跳过，级数置 0 以免遍历到无意义的矩阵。
        uniform.enabled      = 0.0F;
        uniform.cascadeCount = 0.0F;
        return uniform;
    }

    uniform.enabled = 1.0F;

    const int count = std::clamp(settings.cascadeCount, 1, kMaxShadowCascades);

    // 阴影只覆盖到 min(相机远平面, max_distance)：更远的地形在雾里，投影没有意义、还会摊薄分辨率。
    const float shadowFar = std::min(farPlane, settings.maxDistance);
    const std::array<float, kMaxShadowCascades> splits =
        ComputeCascadeSplits(nearPlane, shadowFar, count, settings.splitLambda);

    // 太阳方向（配置里是长度 3 的数组，此处归一化为单位向量，与 BuildLightingUniform 同口径）。
    const std::array<float, 3>& sunArray = table.Sun().direction;
    const glm::vec3 sunDirection =
        MakeLightBasis(glm::vec3(sunArray[0], sunArray[1], sunArray[2])).direction;
    const float     tanHalfFov   = std::tan(glm::radians(fieldOfViewDegrees) * 0.5F);
    const glm::mat4 inverseView  = glm::inverse(viewRelative);

    float previousSplit = nearPlane;
    for (int i = 0; i < count; ++i) {
        const float splitFar = splits[static_cast<std::size_t>(i)];

        glm::vec3 corners[8];
        FrustumSlabCornersRelative(inverseView, previousSplit, splitFar, tanHalfFov, aspectRatio, corners);

        glm::vec3 minimum = corners[0];
        glm::vec3 maximum = corners[0];
        for (int c = 1; c < 8; ++c) {
            minimum = glm::min(minimum, corners[c]);
            maximum = glm::max(maximum, corners[c]);
        }

        // 包围球 = 视锥切片 AABB 的外接球（旋转不变，避免相机转动时级联范围"跳"）。
        const glm::vec3 center = (minimum + maximum) * 0.5F;
        const float     radius = glm::length((maximum - minimum) * 0.5F);

        // 投射体扩展：该级中心之上必须覆盖的高度 = max(配置下限, 地形最高点相对原点高度 − 中心相对高度)。
        // `casterTopRelative` 与 `center.y` **同为渲染原点相对坐标**，相减即"最高地形高度 − 该级中心高度"
        // （渲染原点 Y 抵消），因此扩展量不随渲染原点重定基而漂移（见头文件对该坐标系的强调）。
        const float casterHeight = std::max(settings.casterHeightMin, casterTopRelative - center.y);

        // 缺陷 B8（机制 2）：把 texel 世界尺寸向上量化到 2 的幂，并据量化后的 texel 反推**实际使用的
        // 半宽** `halfExtentUsed = quantizedTexel · resolution / 2`（= `nextPowerOfTwo(halfExtentRaw)`，
        // 因 resolution 也是 2 的幂）。投影缩放（1/halfExtentUsed）与 texel 网格因此**分段恒定**：
        // 相机小幅旋转（半径不跨 2 的幂边界）时二者都不变，texel 对齐网格不再随视角爬行。
        // 覆盖不缩水：`halfExtentUsed ≥ halfExtentRaw = radius + 水平扩展`。
        const float horizontalExtension = CasterHorizontalExtension(sunDirection, casterHeight);
        const float halfExtentRaw       = radius + horizontalExtension;
        const float rawTexelWorldSize   = 2.0F * halfExtentRaw / static_cast<float>(settings.resolution);
        const float quantizedTexel      = QuantizeTexelWorldSize(rawTexelWorldSize, settings.resolution);
        const float halfExtentUsed      = std::max(halfExtentRaw,
                                                   quantizedTexel * static_cast<float>(settings.resolution) * 0.5F);
        // BuildCascadeLightMatrix 内部还会叠加 caster 水平扩展，故传入的"半径"取 halfExtentUsed − 水平扩展，
        // 使函数内部正交半宽恰为 halfExtentUsed、texel 恰为 quantizedTexel（两者一致，对齐网格与盒匹配）。
        const float cascadeRadiusArg = std::max(0.0F, halfExtentUsed - horizontalExtension);
        uniform.lightMatrices[static_cast<std::size_t>(i)] =
            BuildCascadeLightMatrix(sunDirection, center, cascadeRadiusArg, quantizedTexel, casterHeight);
        uniform.splitDistances[static_cast<std::size_t>(i)] = splitFar;

        previousSplit = splitFar;
    }

    uniform.cascadeCount = static_cast<float>(count);
    return uniform;
}

}  // namespace vx
