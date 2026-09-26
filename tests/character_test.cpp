// 缺陷 B1 / T13 的回归测试：
//   - B1：移动基向量的左右语义必须以**相机自身基向量**为准（D = 相机右、A = 相机左，W/S 同理）；
//   - T13：主角可视胶囊的尺寸、法线与三角形绕序必须与碰撞胶囊一致且能被正面看到。

#include "character_mesh.hpp"
#include "character_movement.hpp"
#include "render/camera.hpp"

#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using vx::CameraRelativeMoveDirection;
using vx::CameraSettings;
using vx::CameraView;
using vx::ThirdPersonCamera;

/// 相机自身的基向量（权威来源）。
///
/// 前向取 `target - eye` 的单位向量，右向取 `cross(前向, 世界上方)`——
/// 这与 `glm::lookAt` 视空间的 +X 轴一致。测试不另写一套三角函数当作真值，
/// 从而能真实捕捉"实现把右向量写成相反数"这类错误。
struct CameraBasis {
    glm::vec3 forward { 0.0F };
    glm::vec3 right { 0.0F };
};

[[nodiscard]] CameraBasis BasisFor(float yaw, float pitch) {
    CameraSettings settings;
    settings.followDistance = 10.0F;
    settings.pivotHeight    = 0.0F;

    ThirdPersonCamera camera(settings);
    camera.SnapTo(glm::vec3(0.0F));
    camera.SetYaw(yaw);
    camera.SetPitch(pitch);

    const CameraView view = camera.Evaluate(0.0, nullptr);
    CameraBasis      basis;
    basis.forward = glm::normalize(view.target - view.eye);
    basis.right   = glm::normalize(glm::cross(basis.forward, glm::vec3(0.0F, 1.0F, 0.0F)));
    return basis;
}

}  // namespace

// 对任意 yaw / pitch：D 朝相机右、A 朝相机左；W 沿相机前向、S 与之相反。
TEST(CharacterMovement, StrafeMatchesCameraBasisForAnyYaw) {
    const std::array<float, 8> yaws    = { 0.0F, 0.7F, 1.5708F, 2.5F, -1.3F, 3.14159F, 4.9F, -2.8F };
    const std::array<float, 3> pitches = { 0.0F, -0.42F, 0.55F };

    for (const float yaw : yaws) {
        for (const float pitch : pitches) {
            const CameraBasis basis = BasisFor(yaw, pitch);

            const glm::vec3 rightDir = CameraRelativeMoveDirection(yaw, 0.0F, 1.0F);   // D
            const glm::vec3 leftDir  = CameraRelativeMoveDirection(yaw, 0.0F, -1.0F);  // A
            const glm::vec3 fwdDir   = CameraRelativeMoveDirection(yaw, 1.0F, 0.0F);   // W
            const glm::vec3 backDir  = CameraRelativeMoveDirection(yaw, -1.0F, 0.0F);  // S

            EXPECT_GT(glm::dot(rightDir, basis.right), 0.0F) << "D 应朝相机右 (yaw=" << yaw << ")";
            EXPECT_LT(glm::dot(leftDir, basis.right), 0.0F) << "A 应朝相机左 (yaw=" << yaw << ")";
            EXPECT_GT(glm::dot(fwdDir, basis.forward), 0.0F) << "W 应沿相机前 (yaw=" << yaw << ")";
            EXPECT_LT(glm::dot(backDir, basis.forward), 0.0F) << "S 应沿相机后 (yaw=" << yaw << ")";

            // 地面移动方向始终水平，不把相机俯仰带进来。
            EXPECT_FLOAT_EQ(rightDir.y, 0.0F);
            EXPECT_FLOAT_EQ(fwdDir.y, 0.0F);
        }
    }
}

// 具体数值锚点：yaw = 0 时相机前向 = +Z、右向 = -X（右手下 lookAt 视空间的 +X 轴）。
TEST(CharacterMovement, MatchesExplicitBasisAtZeroYaw) {
    const glm::vec3 right = CameraRelativeMoveDirection(0.0F, 0.0F, 1.0F);   // D
    const glm::vec3 left  = CameraRelativeMoveDirection(0.0F, 0.0F, -1.0F);  // A
    const glm::vec3 fwd   = CameraRelativeMoveDirection(0.0F, 1.0F, 0.0F);   // W
    const glm::vec3 back  = CameraRelativeMoveDirection(0.0F, -1.0F, 0.0F);  // S

    EXPECT_NEAR(right.x, -1.0F, 1e-5F);
    EXPECT_NEAR(right.z, 0.0F, 1e-5F);
    EXPECT_NEAR(left.x, 1.0F, 1e-5F);
    EXPECT_NEAR(fwd.z, 1.0F, 1e-5F);
    EXPECT_NEAR(back.z, -1.0F, 1e-5F);
}

// 主角胶囊：半径 0.30 / 总高 1.80，脚底在原点；法线单位长度。
// （ADR 0009 起顶点不再承载材质权重，故不再断言权重槽。）
TEST(CharacterMesh, MatchesCollisionCapsuleBounds) {
    vx::CapsuleMeshSpec spec;
    spec.radius             = 0.30F;
    spec.cylinderHalfHeight = 0.60F;

    const vx::MeshData mesh = vx::BuildCapsuleMesh(spec);

    ASSERT_FALSE(mesh.vertices.empty());
    ASSERT_FALSE(mesh.indices.empty());
    ASSERT_EQ(mesh.indices.size() % 3U, 0U);

    float minY          = mesh.vertices.front().position[1];
    float maxY          = minY;
    float maxHorizontal = 0.0F;
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        minY = std::min(minY, vertex.position[1]);
        maxY = std::max(maxY, vertex.position[1]);
        maxHorizontal = std::max(maxHorizontal, std::sqrt(vertex.position[0] * vertex.position[0] +
                                                          vertex.position[2] * vertex.position[2]));

        const float normalLength = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                                             vertex.normal[1] * vertex.normal[1] +
                                             vertex.normal[2] * vertex.normal[2]);
        EXPECT_NEAR(normalLength, 1.0F, 1e-4F);
    }

    EXPECT_NEAR(minY, 0.0F, 1e-4F);
    EXPECT_NEAR(maxY, 1.80F, 1e-4F);
    EXPECT_NEAR(maxHorizontal, 0.30F, 1e-4F);

    for (const std::uint32_t index : mesh.indices) {
        EXPECT_LT(index, mesh.vertices.size());
    }
}

// 绕序必须朝外：每个非退化三角形的几何法线要与顶点法线同向，
// 否则在 MeshRenderer 的背面剔除下主角会整片消失。
TEST(CharacterMesh, TriangleWindingFacesOutward) {
    const vx::MeshData mesh = vx::BuildCapsuleMesh(vx::CapsuleMeshSpec {});

    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const vx::MeshVertex& a = mesh.vertices[mesh.indices[triangle]];
        const vx::MeshVertex& b = mesh.vertices[mesh.indices[triangle + 1]];
        const vx::MeshVertex& c = mesh.vertices[mesh.indices[triangle + 2]];

        const glm::vec3 pa(a.position[0], a.position[1], a.position[2]);
        const glm::vec3 pb(b.position[0], b.position[1], b.position[2]);
        const glm::vec3 pc(c.position[0], c.position[1], c.position[2]);

        const glm::vec3 geometricNormal = glm::cross(pb - pa, pc - pa);
        if (glm::length(geometricNormal) < 1e-6F) {
            continue;  // 极点处的退化三角形
        }

        const glm::vec3 averageNormal =
            glm::normalize(glm::vec3(a.normal[0] + b.normal[0] + c.normal[0], a.normal[1] + b.normal[1] + c.normal[1],
                                     a.normal[2] + b.normal[2] + c.normal[2]));

        EXPECT_GT(glm::dot(glm::normalize(geometricNormal), averageNormal), 0.0F)
            << "三角形 " << triangle / 3 << " 绕序朝内";
    }
}
