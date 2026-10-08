// V0.11 / A8：**屏幕像素坐标 → 世界射线**的纯函数单测（[ADR 0041](../../docs/adr/0041-immersive-modify-mode-and-editor-camera.md)）。
//
// 契约（钉死在这里）：屏幕左上角为原点、y 向下；NDC 的 y 向上（翻转一次）；
// 方向为**单位向量**；屏幕中心的射线与相机前向一致；把任意世界点投影回屏幕后，
// 从该屏幕点打出的射线必经过该世界点（往返自洽）。

#include "screen_ray.hpp"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace {

constexpr float kWidth  = 1920.0F;
constexpr float kHeight = 1080.0F;

/// 一个固定的测试相机：位于 (0, 10, 20)，看向 (0, 10, 0)（即朝 −Z 平视），FOV 70°。
[[nodiscard]] glm::mat4 TestViewProjection() {
    const glm::mat4 view       = glm::lookAt(glm::vec3(0.0F, 10.0F, 20.0F), glm::vec3(0.0F, 10.0F, 0.0F),
                                             glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 projection = glm::perspective(glm::radians(70.0F), kWidth / kHeight, 0.05F, 1000.0F);
    return projection * view;
}

}  // namespace

TEST(ScreenRay, ScreenCenterLooksStraightAhead) {
    // 容差取 1e-4：`viewProjection` 是 float 矩阵，`glm::inverse` + 归一化后的方向误差量级约 1e-6~1e-5。
    const vx::ScreenRay ray = vx::ScreenPointToRay(TestViewProjection(), kWidth * 0.5F, kHeight * 0.5F, kWidth,
                                                  kHeight);
    EXPECT_NEAR(ray.direction.x, 0.0, 1.0e-4);
    EXPECT_NEAR(ray.direction.y, 0.0, 1.0e-4);
    EXPECT_NEAR(ray.direction.z, -1.0, 1.0e-4) << "屏幕中心 = 相机前向（本测试相机朝 −Z）";
}

TEST(ScreenRay, DirectionIsNormalized) {
    const vx::ScreenRay ray = vx::ScreenPointToRay(TestViewProjection(), 100.0F, 900.0F, kWidth, kHeight);
    EXPECT_NEAR(glm::length(ray.direction), 1.0, 1.0e-6);
}

TEST(ScreenRay, OffCenterPointsLeanOutwards) {
    const glm::mat4 vp = TestViewProjection();
    const vx::ScreenRay left  = vx::ScreenPointToRay(vp, 1.0F, kHeight * 0.5F, kWidth, kHeight);
    const vx::ScreenRay right = vx::ScreenPointToRay(vp, kWidth - 1.0F, kHeight * 0.5F, kWidth, kHeight);
    const vx::ScreenRay top   = vx::ScreenPointToRay(vp, kWidth * 0.5F, 1.0F, kWidth, kHeight);
    const vx::ScreenRay bottom = vx::ScreenPointToRay(vp, kWidth * 0.5F, kHeight - 1.0F, kWidth, kHeight);
    EXPECT_LT(left.direction.x, 0.0) << "屏幕左侧 ⇒ 世界 −X 方向";
    EXPECT_GT(right.direction.x, 0.0);
    EXPECT_GT(top.direction.y, 0.0) << "屏幕上方 ⇒ 世界 +Y 方向（y 轴已翻转）";
    EXPECT_LT(bottom.direction.y, 0.0);
}

TEST(ScreenRay, RayPassesThroughTheProjectedPoint) {
    // 往返自洽：把世界点投影到屏幕 ⇒ 从该屏幕点打射线 ⇒ 该点必落在射线上。
    const glm::mat4 vp    = TestViewProjection();
    const glm::vec3 world = glm::vec3(3.0F, 12.5F, 0.0F);

    const glm::vec4 clip = vp * glm::vec4(world, 1.0F);
    ASSERT_GT(clip.w, 1.0e-6F);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    const float     px  = (ndc.x * 0.5F + 0.5F) * kWidth;
    const float     py  = (1.0F - (ndc.y * 0.5F + 0.5F)) * kHeight;

    const vx::ScreenRay ray = vx::ScreenPointToRay(vp, px, py, kWidth, kHeight);

    // 点到射线的垂距（用 world 相对 origin 的向量与方向的叉积模长衡量）。
    const glm::dvec3 toPoint = glm::dvec3(world) - ray.origin;
    const double     along   = glm::dot(toPoint, ray.direction);
    ASSERT_GT(along, 0.0) << "投影点必须在射线正方向一侧";
    const double perpendicular = glm::length(glm::cross(toPoint, ray.direction));
    EXPECT_LT(perpendicular, 1.0e-3) << "射线必须穿过被投影的世界点";
    EXPECT_NEAR(along, glm::length(toPoint), 1.0e-3);
}

TEST(ScreenRay, DegenerateInputsYieldDefaultRay) {
    const glm::mat4 vp = TestViewProjection();
    const vx::ScreenRay zeroWidth = vx::ScreenPointToRay(vp, 0.0F, 0.0F, 0.0F, kHeight);
    EXPECT_DOUBLE_EQ(zeroWidth.direction.z, -1.0) << "视口非法 ⇒ 缺省射线（调用方可据此放弃拾取）";
    const vx::ScreenRay zeroHeight = vx::ScreenPointToRay(vp, 0.0F, 0.0F, kWidth, 0.0F);
    EXPECT_DOUBLE_EQ(zeroHeight.direction.z, -1.0);
}
