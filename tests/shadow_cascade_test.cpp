// T21b：级联阴影的纯函数单测 —— 级联分割、正交光空间矩阵（含 texel 对齐）、
// 以及 BuildShadowUniform 的几何正确性（每级视锥切片必须被该级矩阵完整覆盖）。
//
// 口径依据：docs/adr/0010-render-quality-pipeline.md（P1 第二步 · CSM 级联阴影）。
// 这些断言是"阴影不抖动、不错位"的**可自动化证据**：本环境无法目视，故把
//   - 分割单调性与端点；
//   - 包围球 → NDC [-1,1]³ 的覆盖；
//   - texel 对齐（亚 texel 平移不改变矩阵、整 texel 平移按量级跳变）；
// 都钉成数值断言。

#include "render/lighting_table.hpp"
#include "render/shadow_cascade.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using vx::BuildCascadeLightMatrix;
using vx::BuildShadowUniform;
using vx::ComputeCascadeSplits;
using vx::kMaxShadowCascades;
using vx::LightingTable;
using vx::ShadowUniform;

constexpr float kTolerance = 1e-4F;

/// 一份只求"能通过校验"的临时配置：阴影关闭、级数 2、分辨率取下限 256、偏移为 0。
/// 用于覆盖 enabled = false 与边界值的解析 / 投影路径。
constexpr const char* kDisabledShadowConfig =
    "schema_version = 2\n"
    "[sun]\n"
    "direction = [0.0, 1.0, 0.0]\n"
    "color = [1.0, 1.0, 1.0]\n"
    "intensity = 1.0\n"
    "[sky]\n"
    "zenith_color = [0.5, 0.5, 0.5]\n"
    "horizon_color = [0.5, 0.5, 0.5]\n"
    "ground_color = [0.1, 0.1, 0.1]\n"
    "intensity = 1.0\n"
    "[fog]\n"
    "enabled = false\n"
    "density = 0.001\n"
    "height_falloff = 0.01\n"
    "[shadow]\n"
    "enabled = false\n"
    "cascade_count = 2\n"
    "resolution = 256\n"
    "max_distance = 64.0\n"
    "split_lambda = 1.0\n"
    "depth_bias = 0.0\n"
    "normal_offset = 0.0\n";

[[nodiscard]] std::filesystem::path WriteTempConfig(const char* name, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream               out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

void RemoveTempConfig(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace

// 级联分割必须严格递增、首项 > near、末项 = far；未用槽位填 far（调用方不做越界读取）。
TEST(ShadowCascade, SplitsIncreaseMonotonicallyAndReachEndpoints) {
    const float nearPlane = 0.1F;
    const float farPlane  = 500.0F;

    const std::array<float, kMaxShadowCascades> splits = ComputeCascadeSplits(nearPlane, farPlane, 4, 0.75F);

    EXPECT_GT(splits[0], nearPlane);
    for (int i = 1; i < 4; ++i) {
        EXPECT_GT(splits[static_cast<std::size_t>(i)], splits[static_cast<std::size_t>(i - 1)]) << "i=" << i;
    }
    EXPECT_NEAR(splits[3], farPlane, kTolerance);

    const std::array<float, kMaxShadowCascades> three = ComputeCascadeSplits(nearPlane, farPlane, 3, 0.75F);
    EXPECT_NEAR(three[2], farPlane, kTolerance);
    EXPECT_FLOAT_EQ(three[3], farPlane) << "未使用的槽位应填 far，避免越界读取到 0";
}

// λ = 0 → 均匀分割：split_i = near + (far - near)·i/n。
TEST(ShadowCascade, LambdaZeroIsUniformSplit) {
    const float nearPlane = 1.0F;
    const float farPlane  = 101.0F;

    const std::array<float, kMaxShadowCascades> splits = ComputeCascadeSplits(nearPlane, farPlane, 4, 0.0F);

    for (int i = 1; i <= 4; ++i) {
        const float expected = nearPlane + (farPlane - nearPlane) * static_cast<float>(i) / 4.0F;
        EXPECT_NEAR(splits[static_cast<std::size_t>(i - 1)], expected, kTolerance) << "i=" << i;
    }
}

// λ = 1 → 完全对数分割：split_i = near·(far/near)^(i/n)。
TEST(ShadowCascade, LambdaOneIsLogarithmicSplit) {
    const float nearPlane = 1.0F;
    const float farPlane  = 100.0F;

    const std::array<float, kMaxShadowCascades> splits = ComputeCascadeSplits(nearPlane, farPlane, 4, 1.0F);

    for (int i = 1; i <= 4; ++i) {
        const float t        = static_cast<float>(i) / 4.0F;
        const float expected = nearPlane * std::pow(farPlane / nearPlane, t);
        EXPECT_NEAR(splits[static_cast<std::size_t>(i - 1)], expected, kTolerance * expected) << "i=" << i;
    }
    EXPECT_NEAR(splits[0], std::sqrt(10.0F), kTolerance) << "100^(1/4) = 10^(1/2)";
    EXPECT_NEAR(splits[3], farPlane, kTolerance);
}

// λ 介于两者之间时必须落在两条退化曲线之间（分割系数确有影响）。
TEST(ShadowCascade, LambdaInterpolatesBetweenUniformAndLogarithmic) {
    const float nearPlane = 1.0F;
    const float farPlane  = 100.0F;

    const std::array<float, kMaxShadowCascades> uniform = ComputeCascadeSplits(nearPlane, farPlane, 3, 0.0F);
    const std::array<float, kMaxShadowCascades> mixed   = ComputeCascadeSplits(nearPlane, farPlane, 3, 0.75F);
    const std::array<float, kMaxShadowCascades> loga    = ComputeCascadeSplits(nearPlane, farPlane, 3, 1.0F);

    for (int i = 0; i < 3; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        EXPECT_GE(mixed[index], loga[index] - kTolerance);
        EXPECT_LE(mixed[index], uniform[index] + kTolerance);
    }
}

// 正交光空间矩阵必须把该级包围球上的点全部映射进 NDC [-1, 1]³。
TEST(ShadowCascade, LightMatrixMapsBoundingSphereIntoNdc) {
    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5F, 0.8F, 0.3F));
    const glm::vec3 center(3.0F, 4.0F, 5.0F);
    const float     radius = 10.0F;
    const float     texel  = 2.0F * radius / 1024.0F;

    const glm::mat4 matrix = BuildCascadeLightMatrix(sunDirection, center, radius, texel);

    for (int signX = -1; signX <= 1; signX += 2) {
        for (int signY = -1; signY <= 1; signY += 2) {
            for (int signZ = -1; signZ <= 1; signZ += 2) {
                const glm::vec3 direction(static_cast<float>(signX), static_cast<float>(signY),
                                          static_cast<float>(signZ));
                const glm::vec3 point = center + glm::normalize(direction) * radius;
                const glm::vec4 clip  = matrix * glm::vec4(point, 1.0F);

                EXPECT_GE(clip.x, -1.0F - kTolerance);
                EXPECT_LE(clip.x, 1.0F + kTolerance);
                EXPECT_GE(clip.y, -1.0F - kTolerance);
                EXPECT_LE(clip.y, 1.0F + kTolerance);
                EXPECT_GE(clip.z, -1.0F - kTolerance);
                EXPECT_LE(clip.z, 1.0F + kTolerance);
            }
        }
    }
}

// texel 对齐：相机平移不足一个 texel 时矩阵**不变**（不产生亚 texel 抖动）；
// 恰好平移一个 texel 时，NDC 平移量正好是一个 texel。
TEST(ShadowCascade, LightMatrixSnapsToTexelGrid) {
    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5F, 0.8F, 0.3F));
    const glm::vec3 worldUp(0.0F, 1.0F, 0.0F);
    const glm::vec3 right = glm::normalize(glm::cross(sunDirection, worldUp));  // 恰好是量化轴

    const float     radius = 12.0F;
    const float     texel  = 2.0F * radius / 1024.0F;
    const glm::vec3 center(0.0F, 0.0F, 0.0F);  // 落在 texel 网格上，使量化结果可预期

    const glm::mat4 base     = BuildCascadeLightMatrix(sunDirection, center, radius, texel);
    const glm::mat4 subTexel = BuildCascadeLightMatrix(sunDirection, center + right * (0.4F * texel), radius, texel);
    const glm::mat4 oneTexel = BuildCascadeLightMatrix(sunDirection, center + right * texel, radius, texel);

    // 亚 texel 平移：量化到同一网格 → 矩阵逐元素相同。
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            EXPECT_NEAR(subTexel[column][row], base[column][row], kTolerance) << column << "," << row;
        }
    }

    // 整 texel 平移：固定点的 NDC x 位移恰为 texel / radius（视图 x 轴 = -right，故符号为正）。
    const glm::vec4 baseClip  = base * glm::vec4(center, 1.0F);
    const glm::vec4 movedClip = oneTexel * glm::vec4(center, 1.0F);
    EXPECT_NEAR(movedClip.x - baseClip.x, texel / radius, kTolerance);
}

// BuildShadowUniform：级数 / texel 尺寸 / 启用标志与配置一致，且**每级矩阵完整覆盖该级视锥切片**
// （切片 8 个角点全部落在 NDC [-1, 1]³）——这是"阴影不错位"的几何证据。
TEST(ShadowCascade, UniformCoversEachCascadeFrustumSlice) {
    const LightingTable table = LightingTable::Default();

    const float fieldOfViewDegrees = 60.0F;
    const float aspectRatio        = 1.0F;
    const float nearPlane          = 0.1F;
    const float farPlane           = 100.0F;  // < max_distance(180)，故覆盖到 farPlane
    const int   cascadeCount       = table.Shadow().cascadeCount;
    ASSERT_EQ(cascadeCount, 3);

    const ShadowUniform uniform =
        BuildShadowUniform(table, glm::mat4(1.0F), fieldOfViewDegrees, aspectRatio, nearPlane, farPlane);

    EXPECT_FLOAT_EQ(uniform.enabled, 1.0F);
    EXPECT_FLOAT_EQ(uniform.cascadeCount, static_cast<float>(cascadeCount));
    EXPECT_FLOAT_EQ(uniform.texelSize, 1.0F / static_cast<float>(table.Shadow().resolution));
    EXPECT_FLOAT_EQ(uniform.depthBias, table.Shadow().depthBias);
    EXPECT_FLOAT_EQ(uniform.normalOffset, table.Shadow().normalOffset);

    // identity 视图 → 相机看向 -Z，前向 = (0, 0, -1)。
    EXPECT_NEAR(uniform.cameraForwardX, 0.0F, kTolerance);
    EXPECT_NEAR(uniform.cameraForwardY, 0.0F, kTolerance);
    EXPECT_NEAR(uniform.cameraForwardZ, -1.0F, kTolerance);

    const std::array<float, kMaxShadowCascades> expected =
        ComputeCascadeSplits(nearPlane, farPlane, cascadeCount, table.Shadow().splitLambda);
    for (int i = 0; i < cascadeCount; ++i) {
        EXPECT_NEAR(uniform.splitDistances[static_cast<std::size_t>(i)], expected[static_cast<std::size_t>(i)],
                    kTolerance)
            << "i=" << i;
    }
    EXPECT_NEAR(uniform.splitDistances[static_cast<std::size_t>(cascadeCount - 1)], farPlane, kTolerance);

    const float tanHalfFov = std::tan(glm::radians(fieldOfViewDegrees) * 0.5F);
    float       sliceNear  = nearPlane;
    for (int i = 0; i < cascadeCount; ++i) {
        const float sliceFar = expected[static_cast<std::size_t>(i)];
        for (int farSide = 0; farSide < 2; ++farSide) {
            const float distance = (farSide == 0) ? sliceNear : sliceFar;
            const float halfHeight = distance * tanHalfFov;
            const float halfWidth  = halfHeight * aspectRatio;
            for (int signY = -1; signY <= 1; signY += 2) {
                for (int signX = -1; signX <= 1; signX += 2) {
                    const glm::vec4 corner(static_cast<float>(signX) * halfWidth,
                                           static_cast<float>(signY) * halfHeight, -distance, 1.0F);
                    const glm::vec4 clip = uniform.lightMatrices[static_cast<std::size_t>(i)] * corner;
                    EXPECT_GE(clip.x, -1.0F - kTolerance) << "cascade " << i;
                    EXPECT_LE(clip.x, 1.0F + kTolerance) << "cascade " << i;
                    EXPECT_GE(clip.y, -1.0F - kTolerance) << "cascade " << i;
                    EXPECT_LE(clip.y, 1.0F + kTolerance) << "cascade " << i;
                    EXPECT_GE(clip.z, -1.0F - kTolerance) << "cascade " << i;
                    EXPECT_LE(clip.z, 1.0F + kTolerance) << "cascade " << i;
                }
            }
        }
        sliceNear = sliceFar;
    }
}

// 覆盖距离取 min(相机远平面, max_distance)：远平面 1000 时最远一级仍停在 max_distance。
TEST(ShadowCascade, UniformClampsCoverageToMaxDistance) {
    const LightingTable table = LightingTable::Default();

    const ShadowUniform uniform =
        BuildShadowUniform(table, glm::mat4(1.0F), 70.0F, 16.0F / 9.0F, 0.05F, 1000.0F);

    const int lastIndex = table.Shadow().cascadeCount - 1;
    EXPECT_NEAR(uniform.splitDistances[static_cast<std::size_t>(lastIndex)], table.Shadow().maxDistance, kTolerance);
}

// enabled = false → 块显式关闭（enabled = 0、级数 0），着色器据此整段跳过。
TEST(ShadowCascade, DisabledShadowFlagsUniform) {
    const std::filesystem::path path = WriteTempConfig("vx_shadow_disabled.toml", kDisabledShadowConfig);
    const LightingTable         table = LightingTable::LoadFromFile(path);

    const ShadowUniform uniform =
        BuildShadowUniform(table, glm::mat4(1.0F), 70.0F, 16.0F / 9.0F, 0.05F, 500.0F);

    EXPECT_FLOAT_EQ(uniform.enabled, 0.0F);
    EXPECT_FLOAT_EQ(uniform.cascadeCount, 0.0F);
    // 关闭时偏移与 texel 尺寸仍如实透传（着色器不读，但保持字段可预期）。
    EXPECT_FLOAT_EQ(uniform.depthBias, 0.0F);
    EXPECT_FLOAT_EQ(uniform.normalOffset, 0.0F);
    EXPECT_FLOAT_EQ(uniform.texelSize, 1.0F / 256.0F);
    RemoveTempConfig(path);
}

// uniform 字节数必须与 mesh.frag 的 ShadowBlock（4×mat4 + 3×vec4）逐字节一致。
TEST(ShadowCascade, UniformLayoutMatchesGlslBlock) {
    EXPECT_EQ(sizeof(ShadowUniform), static_cast<std::size_t>(4 * 64 + 3 * 16));
}
