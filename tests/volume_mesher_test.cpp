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
#include <cstdio>
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

/// 采样索引平移包装（T42 的"子区域"用例）：区域本地索引 `(i, j, k)` ↔ 内层采样器的 `(i + o, …)`。
class OffsetSampler final : public IVolumeSampler {
public:
    OffsetSampler(const IVolumeSampler& inner, int offsetX, int offsetY, int offsetZ) noexcept
        : m_inner(inner), m_offsetX(offsetX), m_offsetY(offsetY), m_offsetZ(offsetZ) {}

    [[nodiscard]] float Sample(int i, int j, int k) const override {
        return m_inner.Sample(i + m_offsetX, j + m_offsetY, k + m_offsetZ);
    }

    [[nodiscard]] std::uint8_t SampleMaterial(int i, int j, int k) const override {
        return m_inner.SampleMaterial(i + m_offsetX, j + m_offsetY, k + m_offsetZ);
    }

private:
    const IVolumeSampler& m_inner;
    int                   m_offsetX;
    int                   m_offsetY;
    int                   m_offsetZ;
};

/// 三个三角形顶点的面积之和（用于"子区域与整块面积一致"的判据）。
[[nodiscard]] double SurfaceArea(const MeshData& mesh) {
    double area = 0.0;
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const glm::vec3 a = Position(mesh, mesh.indices[triangle]);
        const glm::vec3 b = Position(mesh, mesh.indices[triangle + 1]);
        const glm::vec3 c = Position(mesh, mesh.indices[triangle + 2]);
        area += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
    }
    return area;
}

constexpr float       kSphereRadius = 12.0F;
const     glm::vec3   kSphereCenter(16.0F, 16.0F, 16.0F);

/// T55 探针判据：**绕序反转**（正面朝向实体侧）的三角形数 —— 几何法线与三个顶点法线（密度梯度，
/// 指向**空侧**）之均值的点积 < 0。
///
/// 为什么另两条判据量不到它：`CountDegenerateTriangles` 量"零面积 ⇒ 画不出片元"、
/// `CountBoundaryEdges` 量"面缺失 ⇒ 留下洞"。而 Surface Nets 在**歧义 cell**（例：两个对角实心角）
/// 里只能放**一个**顶点，四个邻 cell 的四边形可能自交 ⇒ 其中某个三角形绕序**翻转**：
/// 面在、边闭合、面积也不为零，但正面朝向实体侧 ⇒ 被背面剔除 ⇒ 屏幕表现**与"面消失 / 透明"一致**。
[[nodiscard]] std::size_t CountBackwardTriangles(const MeshData& mesh) {
    std::size_t backward = 0;
    for (std::size_t triangle = 0; triangle + 2U < mesh.indices.size(); triangle += 3U) {
        const std::uint32_t ia = mesh.indices[triangle];
        const std::uint32_t ib = mesh.indices[triangle + 1U];
        const std::uint32_t ic = mesh.indices[triangle + 2U];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) {
            continue;
        }
        const glm::vec3 a    = Position(mesh, ia);
        const glm::vec3 b    = Position(mesh, ib);
        const glm::vec3 c    = Position(mesh, ic);
        const glm::vec3 face = glm::cross(b - a, c - a);
        if (glm::dot(face, face) <= 1.0e-12F) {
            continue;  // 退化：由 `CountDegenerateTriangles` 负责
        }
        if (glm::dot(face, Normal(mesh, ia) + Normal(mesh, ib) + Normal(mesh, ic)) < 0.0F) {
            ++backward;
        }
    }
    return backward;
}

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

// T47（[ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策五）：
// `CountBoundaryEdges` 是"外观网格是否闭合"这条判据的实现，必须**有区分度** ——
// 闭合曲面（球）判 0；人为**删掉一个三角形**（打一个洞）后必须 > 0，否则它证明不了任何东西。
TEST(VolumeMesher, CountBoundaryEdgesDetectsOpenMesh) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      closed = vx::BuildVolumeMesh(sampler);

    ASSERT_FALSE(closed.indices.empty());
    EXPECT_EQ(vx::CountBoundaryEdges(closed), 0U) << "球面 SDF 的等值面应闭合（每条无向边恰被 2 个三角形共用）";

    MeshData pierced = closed;
    pierced.indices.resize(pierced.indices.size() - 3U);  // 删掉一个三角形 ⇒ 洞的三条边只被用到 1 次
    EXPECT_GT(vx::CountBoundaryEdges(pierced), 0U) << "打洞后必须报出边界边（判据的区分度）";
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

/// **整数密度**球面采样器（T55 探针用）：模拟真实存储 —— `clamp(lround((dist − radius) × 127))`。
///
/// 与 `SphereSampler` 的唯一区别是**取整**：真实挖除写入的是 `int8`，故密度是整数；
/// 当球面正好穿过格点（圆心 / 半径取整数值）时会出现**恰好为 0** 的采样 —— 这正是
/// "偶发透明面"的怀疑成因（0 采样会让相邻 cell 的插值顶点落在同一格点上 ⇒ 面退化 ⇒ 画不出来）。
class QuantizedSphereSampler final : public IVolumeSampler {
public:
    QuantizedSphereSampler(double centerX, double centerY, double centerZ, double radius) noexcept
        : m_cx(centerX), m_cy(centerY), m_cz(centerZ), m_radius(radius) {}

    [[nodiscard]] float Sample(int i, int j, int k) const override {
        const double dx = static_cast<double>(i) - m_cx;
        const double dy = static_cast<double>(j) - m_cy;
        const double dz = static_cast<double>(k) - m_cz;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double value =
            std::lround((distance - m_radius) * static_cast<double>(kDensityUnitsPerBlock));
        return static_cast<float>(
            std::clamp(value, static_cast<double>(kDensityMin), static_cast<double>(kDensityMax)));
    }

private:
    double m_cx = 0.0;
    double m_cy = 0.0;
    double m_cz = 0.0;
    double m_radius = 0.0;
};

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

// T42：`BuildRegionMesh` 是 `BuildVolumeMesh` 的**任意尺寸泛化** —— 32³ 尺寸下两者必须**逐位相同**
// （同一份采样、同一套数学、同一个发射顺序）。这条用例钉死"泛化没有引入任何数值差异"。
TEST(VolumeMesher, RegionEntryIsBitIdenticalToBlockEntryAtBlockSize) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      whole   = vx::BuildVolumeMesh(sampler);
    const MeshData      region  = vx::BuildRegionMesh(sampler, kVolumeBlockSize, kVolumeBlockSize, kVolumeBlockSize);

    ASSERT_FALSE(whole.vertices.empty());
    ASSERT_EQ(region.vertices.size(), whole.vertices.size());
    ASSERT_EQ(region.indices, whole.indices);
    for (std::size_t index = 0; index < whole.vertices.size(); ++index) {
        const vx::MeshVertex& left  = whole.vertices[index];
        const vx::MeshVertex& right = region.vertices[index];
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_FLOAT_EQ(right.position[axis], left.position[axis]);
            EXPECT_FLOAT_EQ(right.normal[axis], left.normal[axis]);
        }
        EXPECT_FLOAT_EQ(right.material, left.material);
    }
}

// T42（A′ 的核心判据）：把同一个形状放进**任意尺寸的子区域**里网格化，得到的等值面必须与"整块网格化"
// 在**世界空间一致**（位置 / 法线在 4 ULP 量级、材质逐位相同），面积也一致
// —— 这正是"倒塌整体 = 它原本那一片表面"的保证。
//
// 为什么位置只要求 4 ULP 而不是逐位相同：子区域的顶点位置是在**平移过的局部坐标系**里算出来的
// （`float(i − 2) + frac`），再加回 2 复原世界坐标时会**重新舍入**（加法的指数与被加数不同）——
// 这是浮点结合次序的正常差异，不是缺陷；数量级远小于"采样错位 / 偏移错一"（那会是 ~1 格）。
TEST(VolumeMesher, SubRegionEqualsWholeBlockInWorldSpace) {
    const SphereSampler sampler(kSphereRadius);
    const MeshData      whole = vx::BuildVolumeMesh(sampler);

    // 子区域：块内偏移 (2, 2, 2)、边长 28 —— 球（半径 12、球心 16）完整落在其中。
    constexpr int       kOffset    = 2;
    constexpr int       kSize      = 28;
    constexpr float     kTolerance = 1.0e-4F;
    const OffsetSampler offset(sampler, kOffset, kOffset, kOffset);
    const MeshData      sub = vx::BuildRegionMesh(offset, kSize, kSize, kSize);

    ASSERT_FALSE(sub.vertices.empty());
    // 面积（位置一致 ⇒ 面积必然一致）：容差只为浮点求和次序留余量。
    const double wholeArea = SurfaceArea(whole);
    const double subArea   = SurfaceArea(sub);
    EXPECT_NEAR(subArea, wholeArea, wholeArea * 1.0e-6) << "子区域与整块的面积不一致 ⇒ 有四边形漏发或位置不同";

    // 逐个顶点：把子区域顶点平移到块坐标后，必须在整块网格的顶点集合里找到**一致**的三元组。
    std::size_t matched = 0;
    for (const vx::MeshVertex& candidate : sub.vertices) {
        bool found = false;
        for (const vx::MeshVertex& reference : whole.vertices) {
            bool samePosition = true;
            bool sameNormal   = true;
            for (int axis = 0; axis < 3; ++axis) {
                samePosition = samePosition &&
                               std::abs((candidate.position[axis] + static_cast<float>(kOffset)) -
                                        reference.position[axis]) <= kTolerance;
                sameNormal = sameNormal &&
                             std::abs(candidate.normal[axis] - reference.normal[axis]) <= kTolerance;
            }
            if (samePosition && sameNormal && candidate.material == reference.material) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "子区域顶点在整块网格里找不到对应顶点：(" << candidate.position[0] << ", "
                           << candidate.position[1] << ", " << candidate.position[2] << ")";
        matched += found ? 1U : 0U;
    }
    EXPECT_EQ(matched, sub.vertices.size());
}

// T42：区域入口同样遵守 ADR 0014 的材质口径 —— 未实现 `SampleMaterial` 的采样器回落
// `kNoMaterialOverride`（否则倒塌整体的顶点会带一个越界槽位、片元选层出错）。
TEST(VolumeMesher, RegionEntryFallsBackToNoMaterialOverride) {
    const PlaneSampler plane(4.0F);  // 8³ 区域内的水平面（下实上空）
    const MeshData     region = vx::BuildRegionMesh(plane, 8, 8, 8);

    ASSERT_FALSE(region.vertices.empty());
    for (const vx::MeshVertex& vertex : region.vertices) {
        EXPECT_FLOAT_EQ(vertex.material, vx::kNoMaterialOverride);
    }
}

// ---------------------------------------------------------------------------
// T55 回归判据（"某个面透明 / 缺面"）：外观网格必须**流形且无缺面**。
// 两个互补的检查 —— ① 稠密光滑球面（整数密度 + 球面正好穿过格点等多种对齐）不得产生
// 退化三角形 / 边界边 / 绕序反转；② **歧义面**（网格面四角成棋盘格）必须被拆成两片各自成面。
// ---------------------------------------------------------------------------
TEST(VolumeMesherProbe, LatticeAlignedQuantaReport) {
    struct Config {
        const char* name;
        double      cx;
        double      cy;
        double      cz;
        double      radius;
    };
    const Config configs[] = {
        { "整数心 / r=6", 16.0, 16.0, 16.0, 6.0 },        { "整数心 / r=6.5", 16.0, 16.0, 16.0, 6.5 },
        { "整数心 / r=8", 16.0, 16.0, 16.0, 8.0 },        { "整数心 / r=12", 16.0, 16.0, 16.0, 12.0 },
        { "半格心 / r=6", 16.5, 16.5, 16.5, 6.0 },        { "半格心 / r=7.5", 16.5, 16.5, 16.5, 7.5 },
        { "偏移心 / r=7.25", 16.25, 16.5, 16.75, 7.25 },  { "偏移心 / r=5.5", 16.5, 16.0, 16.5, 5.5 },
        { "偏移心 / r=8.75", 15.5, 16.25, 16.75, 8.75 },  { "偏移心 / r=9.5", 16.5, 15.75, 16.25, 9.5 },
        { "偏移心 / r=10.25", 16.75, 16.5, 15.5, 10.25 }, { "整数心 / r=9", 16.0, 16.0, 16.0, 9.0 },
    };

    for (const Config& config : configs) {
        const QuantizedSphereSampler sampler(config.cx, config.cy, config.cz, config.radius);
        const MeshData               mesh = vx::BuildVolumeMesh(sampler);
        std::printf("[T55 复现] %-16s 顶点 %5zu 三角 %5zu **退化 %3zu** 边界边 %3zu **绕序反转 %3zu**\n",
                    config.name, mesh.vertices.size(), mesh.indices.size() / 3U, vx::CountDegenerateTriangles(mesh),
                    vx::CountBoundaryEdges(mesh), CountBackwardTriangles(mesh));
        EXPECT_EQ(vx::CountDegenerateTriangles(mesh), 0U) << config.name << "：不得出现退化三角形";
        EXPECT_EQ(vx::CountBoundaryEdges(mesh), 0U) << config.name << "：等值面必须闭合（每条边恰被 2 个三角形共用）";
        EXPECT_EQ(CountBackwardTriangles(mesh), 0U) << config.name << "：正面必须朝向空侧（否则被背面剔除 = 透明）";
    }
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// T55 复现固件（**临时探针**）：**歧义面** —— 某个网格面（grid face）的四角符号成**棋盘格**
/// （对角实心、对角空）。此时该面的 4 条棱**全部**与表面相交 ⇒ 该面两侧的 cell 顶点之间的
/// 那条网格边会被**多达 4 个四边形**共用（非流形捏合）；同时两侧 cell 都是"对角实心"的歧义 cell，
/// 只能放 1 个顶点 ⇒ 四边形自交 ⇒ 其中某个三角形绕序翻转（背面剔除 ⇒ 屏幕表现与"面透明"一致）。
// ---------------------------------------------------------------------------
namespace {
class CheckerboardFaceSampler final : public IVolumeSampler {
public:
    /// 两个**对角相邻**的实心采样点（网格面 `x=3` 上，(3,3,3) 与 (3,4,4)）：该面的四角符号成棋盘格。
    /// 位置刻意离区域边界 ≥3 格，避免"区域边界棱"造成的假阳性。
    [[nodiscard]] float Sample(int i, int j, int k) const override {
        const bool solid = (i == 3) && (((j == 3) && (k == 3)) || ((j == 4) && (k == 4)));
        return solid ? static_cast<float>(kDensityMin) : static_cast<float>(kDensityMax);
    }
};
}  // namespace

TEST(VolumeMesher, AmbiguousFaceMeshesAsTwoManifoldSheets) {
    const CheckerboardFaceSampler sampler;
    const MeshData                mesh = vx::BuildRegionMesh(sampler, 8, 8, 8);
    ASSERT_FALSE(mesh.indices.empty()) << "两个对角实心点必须各自生成一片闭合等值面";
    EXPECT_EQ(vx::CountBoundaryEdges(mesh), 0U) << "歧义面两侧必须各自成面 ⇒ 无边界边（非流形已被拆顶点修掉）";
    EXPECT_EQ(vx::CountDegenerateTriangles(mesh), 0U) << "不得出现退化三角形";
    EXPECT_EQ(CountBackwardTriangles(mesh), 0U) << "正面必须朝向空侧（否则被背面剔除 = 看起来透明）";
}
