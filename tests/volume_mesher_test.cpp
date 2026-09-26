// 可挖体积的等值面网格化（Naive Surface Nets，ADR 0007）单元测试。
//
// 判据（阶段计划 T8）：对球面 SDF 生成的网格必须
//   ① 顶点全部落在球面附近；② 法线朝外；③ 索引合法（不越界、无退化三角形）；
//   ④ 三角形绕序使正面朝向空侧（即与外法线方向一致，可被背面剔除保留）；
//   ⑤ 全实心 / 全空的块不产生任何几何。

#include "dig/volume_mesher.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::IVolumeSampler;
using vx::kDensityMax;
using vx::kDensityMin;
using vx::kDensityUnitsPerBlock;
using vx::kVolumeBlockSize;
using vx::MeshData;

/// 球面 SDF 采样器：球心在块内 `(16, 16, 16)`，半径 `radius`（格）。
///
/// 采样索引可越界（`-1` / `32`）——本采样器是解析式，故天然满足"越界给出同一份密度"。
class SphereSampler final : public IVolumeSampler {
public:
    explicit SphereSampler(float radius) noexcept : m_radius(radius) {}

    [[nodiscard]] float Sample(int i, int j, int k) const override {
        const float dx       = static_cast<float>(i) - 16.0F;
        const float dy       = static_cast<float>(j) - 16.0F;
        const float dz       = static_cast<float>(k) - 16.0F;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        return std::clamp((distance - m_radius) * kDensityUnitsPerBlock, static_cast<float>(kDensityMin),
                          static_cast<float>(kDensityMax));
    }

private:
    float m_radius;
};

/// 常量采样器：全实心（`kDensityMin`）或全空（`kDensityMax`）。
class ConstantSampler final : public IVolumeSampler {
public:
    explicit ConstantSampler(float value) noexcept : m_value(value) {}

    [[nodiscard]] float Sample(int, int, int) const override { return m_value; }

private:
    float m_value;
};

/// 半空间采样器 + 材质：`y < level` 为实心，且**实心侧**的材质槽位 = `solidSlot`。
///
/// 用于验证 ADR 0014：**洞的内表面**（= 被切开的实体那一侧）必须携带该实体的材质槽位，
/// 片元才能直接用"被切开的那种材质"着色；否则会按世界高度 / 坡度重算 ⇒ 洞底出现"地下草地"。
class MaterialHalfSpaceSampler final : public IVolumeSampler {
public:
    MaterialHalfSpaceSampler(float level, std::uint8_t solidSlot) noexcept
        : m_level(level), m_solidSlot(solidSlot) {}

    [[nodiscard]] float Sample(int, int j, int) const override {
        return (static_cast<float>(j) < m_level) ? static_cast<float>(kDensityMin)
                                                 : static_cast<float>(kDensityMax);
    }

    [[nodiscard]] std::uint8_t SampleMaterial(int, int, int) const override { return m_solidSlot; }

private:
    float        m_level;
    std::uint8_t m_solidSlot;
};

/// 临时诊断用：水平面 `z = level`（下实上空）。
class PlaneSampler final : public IVolumeSampler {
public:
    explicit PlaneSampler(float level) noexcept : m_level(level) {}

    [[nodiscard]] float Sample(int, int, int k) const override {
        return (static_cast<float>(k) - m_level) * kDensityUnitsPerBlock;
    }

private:
    float m_level;
};

[[nodiscard]] glm::vec3 Position(const MeshData& mesh, std::uint32_t index) {
    const vx::MeshVertex& vertex = mesh.vertices[index];
    return glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
}

[[nodiscard]] glm::vec3 Normal(const MeshData& mesh, std::uint32_t index) {
    const vx::MeshVertex& vertex = mesh.vertices[index];
    return glm::vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
}

constexpr float       kSphereRadius = 12.0F;
const     glm::vec3   kSphereCenter(16.0F, 16.0F, 16.0F);

}  // namespace

// 水平面（`z = 16.5`）：每个 z 棱恰发射一次 ⇒ 32×32 = 1024 个四边形、总面积 ≈ 1024 格²。
// 这条用例专门钉死"每条网格棱只发射一次、且不漏发"——曾用它抓到"读错角位"导致四边形几乎全漏的缺陷。
TEST(VolumeMesher, HorizontalPlaneProducesOneQuadPerCrossingEdge) {
    const PlaneSampler sampler(16.5F);
    const MeshData     mesh = vx::BuildVolumeMesh(sampler);

    EXPECT_EQ(mesh.indices.size(), 1024U * 6U) << "面内 32×32 条竖棱各应发射一个四边形";

    double area = 0.0;
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const glm::vec3 a = Position(mesh, mesh.indices[triangle]);
        const glm::vec3 b = Position(mesh, mesh.indices[triangle + 1]);
        const glm::vec3 c = Position(mesh, mesh.indices[triangle + 2]);
        area += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
    }
    EXPECT_NEAR(area, 1024.0, 64.0) << "平面片的总面积应等于其覆盖面积（32×32 格）";
}

// 球面：顶点全部落在球面附近（SN 顶点为 cell 内棱交点平均，误差上限约一个体素）。
TEST(VolumeMesher, SphereVerticesLieOnSurface) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.vertices.empty()) << "球面 SDF 必须产生几何";
    ASSERT_FALSE(mesh.indices.empty());

    float maxError = 0.0F;
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        const float     distance = glm::length(position - kSphereCenter);
        maxError                 = std::max(maxError, std::abs(distance - kSphereRadius));
    }
    EXPECT_LT(maxError, 1.0F) << "顶点到球面的偏差应小于 1 个体素（实测 " << maxError << "）";
}

// 球面：法线朝外（密度梯度指向空侧），且为单位向量。
TEST(VolumeMesher, SphereNormalsPointOutward) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.vertices.empty());
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        EXPECT_NEAR(glm::length(normal), 1.0F, 1.0e-4F) << "法线必须是单位向量";
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        EXPECT_GT(glm::dot(normal, position - kSphereCenter), 0.0F) << "法线必须指向球外（空侧）";
    }
}

// 球面：索引合法、无退化三角形，且三角形绕序的正面朝向**空侧**（与外法线一致）。
TEST(VolumeMesher, SphereTrianglesAreValidAndOutwardFacing) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 3U, 0U) << "索引数必须是 3 的倍数";

    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const std::uint32_t ia = mesh.indices[triangle];
        const std::uint32_t ib = mesh.indices[triangle + 1];
        const std::uint32_t ic = mesh.indices[triangle + 2];
        ASSERT_LT(ia, mesh.vertices.size());
        ASSERT_LT(ib, mesh.vertices.size());
        ASSERT_LT(ic, mesh.vertices.size());

        const glm::vec3 a          = Position(mesh, ia);
        const glm::vec3 b          = Position(mesh, ib);
        const glm::vec3 c          = Position(mesh, ic);
        const glm::vec3 faceNormal = glm::cross(b - a, c - a);
        EXPECT_GT(glm::length(faceNormal), 1.0e-6F) << "不得出现退化三角形";
        const glm::vec3 centroid = (a + b + c) / 3.0F;
        EXPECT_GT(glm::dot(faceNormal, centroid - kSphereCenter), 0.0F)
            << "逆时针绕序的正面必须朝向球外（与背面剔除口径一致）";
    }
}

// 全实心 / 全空的块：不产生任何顶点与索引（等值面不存在）。
TEST(VolumeMesher, UniformBlocksProduceNoGeometry) {
    const ConstantSampler solid(static_cast<float>(vx::kDensityMin));
    const ConstantSampler air(static_cast<float>(vx::kDensityMax));

    EXPECT_TRUE(vx::BuildVolumeMesh(solid).vertices.empty());
    EXPECT_TRUE(vx::BuildVolumeMesh(solid).indices.empty());
    EXPECT_TRUE(vx::BuildVolumeMesh(air).vertices.empty());
    EXPECT_TRUE(vx::BuildVolumeMesh(air).indices.empty());
}

// 顶点位置是**块内局部坐标**：球心在块中央，故顶点应大致落在 [4, 28] 区间内（红线 6：世界定位由块坐标承担）。
TEST(VolumeMesher, VerticesStayWithinBlockLocalRange) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.vertices.empty());
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_GT(vertex.position[axis], -1.0F);
            EXPECT_LT(vertex.position[axis], static_cast<float>(kVolumeBlockSize) + 1.0F);
        }
    }
}

// 闭合性（强判据）：三角形总面积应接近球面积 4πR²。
// 这能捕获"顶点算出来了但四边形发射漏掉 / 重复发射"的一类错误——漏发会让面积显著偏小，
// 重复发射会让面积显著偏大。容差 15% 覆盖 SN 对球面的分段近似误差。
// ADR 0014：洞的**内表面**顶点必须携带"被切开的实体"的材质槽位 —— 片元据此直接选层。
// 否则会按世界高度 / 坡度重算 ⇒ 洞底（位于地下、坡度 0）被判成"草"。
// 人工实测第 7 轮："破坏后内部底部是绿色跟现实逻辑不符"。
TEST(VolumeMesher, VerticesCarrySolidSideMaterialSlot) {
    const MaterialHalfSpaceSampler sampler(16.0F, 2U);  // 槽位 2 = 岩
    const MeshData                 mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.vertices.empty());
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        EXPECT_FLOAT_EQ(vertex.material, 2.0F);
    }
}

// 只关心密度、不实现 `SampleMaterial` 的采样器必须回落为 `kNoMaterialOverride`
// ⇒ 片元按高度 / 坡度算权重（地表网格与既有调用方因此零改动）。
TEST(VolumeMesher, VerticesFallBackToNoMaterialOverride) {
    const PlaneSampler sampler(16.0F);
    const MeshData     mesh = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(mesh.vertices.empty());
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        EXPECT_FLOAT_EQ(vertex.material, vx::kNoMaterialOverride);
    }
}

TEST(VolumeMesher, SphereSurfaceAreaMatchesAnalyticValue) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      mesh = vx::BuildVolumeMesh(sampler);

    double area = 0.0;
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const glm::vec3 a = Position(mesh, mesh.indices[triangle]);
        const glm::vec3 b = Position(mesh, mesh.indices[triangle + 1]);
        const glm::vec3 c = Position(mesh, mesh.indices[triangle + 2]);
        area += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
    }

    const double expected = 4.0 * 3.14159265358979323846 * static_cast<double>(kSphereRadius) *
                            static_cast<double>(kSphereRadius);
    EXPECT_GT(area, expected * 0.85) << "三角形总面积偏小 ⇒ 可能有四边形漏发（实测 " << area << "，期望约 "
                                     << expected << "）";
    EXPECT_LT(area, expected * 1.15) << "三角形总面积偏大 ⇒ 可能有四边形重复发射（实测 " << area << "，期望约 "
                                     << expected << "）";
}
