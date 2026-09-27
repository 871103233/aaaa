// 视锥体裁剪的纯函数单测（T39 / 项目所有者要求的"先剔除再提交"）。
//
// 判据（本文件钉死的语义）：
//   ① 视锥内的 AABB 必须**可见**（绝不误剔）—— 这是硬要求，宁可少剔也不能剔掉可见物；
//   ② 相机背后 / 侧向远处 / 超出远平面的 AABB 必须被剔除（否则剔除等于没做）；
//   ③ 与近平面**相交**的 AABB 必须保留（保守性：相交不等于完全在外）；
//   ④ 退化矩阵（零矩阵）不得剔除任何东西（防御性，绝不因异常输入丢几何）。
//
// 约定：与 `ThirdPersonCamera` 同一套投影 —— `glm::perspectiveRH_ZO`，**深度 0~1**
// （因此近平面取矩阵第 3 行，见 `FrustumFromViewProjection` 的说明）。

#include "render/frustum.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

namespace {

using vx::Frustum;
using vx::FrustumFromViewProjection;
using vx::FrustumIntersectsAabb;

/// 相机在原点、朝 −Z 看、fov 70°、16:9、近 0.1 / 远 1000 格 —— 与游戏内相机同约定。
[[nodiscard]] glm::mat4 TestViewProjection() {
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(70.0F), 16.0F / 9.0F, 0.1F, 1000.0F);
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    return projection * view;
}

[[nodiscard]] bool Visible(const Frustum& frustum, const glm::vec3& center, float halfSize) {
    return FrustumIntersectsAabb(frustum, center - glm::vec3(halfSize), center + glm::vec3(halfSize));
}

}  // namespace

// ① 正前方可见。
TEST(Frustum, BoxInFrontIsVisible) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    EXPECT_TRUE(Visible(frustum, glm::vec3(0.0F, 0.0F, -10.0F), 1.0F));
    EXPECT_TRUE(Visible(frustum, glm::vec3(0.0F, 0.0F, -900.0F), 1.0F)) << "远平面之内仍须可见";
}

// ② 相机背后。
TEST(Frustum, BoxBehindCameraIsCulled) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    EXPECT_FALSE(Visible(frustum, glm::vec3(0.0F, 0.0F, 10.0F), 1.0F));
    EXPECT_FALSE(Visible(frustum, glm::vec3(0.0F, 0.0F, 500.0F), 1.0F));
}

// ② 侧向远超视锥宽度（z = −10 处的半宽约 12.4 格）。
TEST(Frustum, BoxFarToTheSideIsCulled) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    EXPECT_FALSE(Visible(frustum, glm::vec3(500.0F, 0.0F, -10.0F), 1.0F));
    EXPECT_FALSE(Visible(frustum, glm::vec3(0.0F, 500.0F, -10.0F), 1.0F));
}

// ② 超出远平面（far = 1000）。
TEST(Frustum, BoxBeyondFarPlaneIsCulled) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    EXPECT_FALSE(Visible(frustum, glm::vec3(0.0F, 0.0F, -2000.0F), 1.0F));
}

// ③ 与近平面相交（跨过相机所在平面）必须保留 —— 保守性，绝不因"压在裁剪面上"丢几何。
TEST(Frustum, BoxStraddlingTheNearPlaneStaysVisible) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    EXPECT_TRUE(Visible(frustum, glm::vec3(0.0F, 0.0F, 0.0F), 1.0F));
    EXPECT_TRUE(Visible(frustum, glm::vec3(0.0F, 0.0F, -0.2F), 0.5F));
}

// ④ 退化矩阵（全零）：不得剔除任何东西（宁多提交，不丢几何）。
TEST(Frustum, DegenerateMatrixNeverCulls) {
    const Frustum frustum = FrustumFromViewProjection(glm::mat4(0.0F));
    EXPECT_TRUE(Visible(frustum, glm::vec3(0.0F, 0.0F, -10.0F), 1.0F));
    EXPECT_TRUE(Visible(frustum, glm::vec3(1000.0F, -500.0F, 5000.0F), 3.0F));
}

// 退化 AABB（min == max，即一个点）也要给出稳定结果：视锥内为 true、背后为 false。
TEST(Frustum, DegenerateAabbIsHandled) {
    const Frustum frustum = FrustumFromViewProjection(TestViewProjection());
    const glm::vec3 point(0.0F, 0.0F, -10.0F);
    EXPECT_TRUE(FrustumIntersectsAabb(frustum, point, point));
    const glm::vec3 behind(0.0F, 0.0F, 10.0F);
    EXPECT_FALSE(FrustumIntersectsAabb(frustum, behind, behind));
}
