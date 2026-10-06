// 物件层（ADR 0004 层③）V0b 单测：程序化代形网格（盒 / 椭球 / 锥）的几何不变量
// （非空、索引合法、法线单位且朝外、底面在 y=0、尺寸与半尺寸一致）与绕 Y 旋转的语义。
// 见 docs/plans/v0.5.md §1.4。

#include "object/object_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::BuildObjectMesh;
using vx::BuildObjectMeshFromModel;
using vx::MeshData;
using vx::MeshVertex;
using vx::Model;
using vx::ModelMesh;
using vx::ModelVertex;
using vx::ObjectAssetKind;
using vx::ObjectMaterialSlot;
using vx::ObjectMeshSpec;
using vx::ObjectType;
using vx::RotateMeshAboutY;

constexpr float kEps = 1.0e-4F;

[[nodiscard]] ObjectType MakeType(ObjectAssetKind kind, float halfX, float halfY, float halfZ) {
    ObjectType type;
    type.id          = "test";
    type.kind        = kind;
    type.halfExtentX = halfX;
    type.halfExtentY = halfY;
    type.halfExtentZ = halfZ;
    return type;
}

/// 四种形态：土堆（锥）/ 石块（椭球）/ 木箱（盒）/ 传送门（立起来的门环），尺寸取自 `objects.toml` 的占位内容。
[[nodiscard]] std::vector<ObjectType> AllKinds() {
    return {
        MakeType(ObjectAssetKind::DirtPile, 1.0F, 0.75F, 1.0F),
        MakeType(ObjectAssetKind::Stone, 1.2F, 0.9F, 1.2F),
        MakeType(ObjectAssetKind::Crate, 0.5F, 0.5F, 0.5F),
        MakeType(ObjectAssetKind::Portal, 1.0F, 1.5F, 0.2F),
    };
}

TEST(ObjectMesh, AllKindsProduceNonEmptyIndexedMeshes) {
    for (const ObjectType& type : AllKinds()) {
        const MeshData mesh = BuildObjectMesh(type);
        ASSERT_FALSE(mesh.vertices.empty()) << "kind " << static_cast<int>(type.kind);
        ASSERT_FALSE(mesh.indices.empty()) << "kind " << static_cast<int>(type.kind);
        ASSERT_EQ(mesh.indices.size() % 3U, 0U) << "索引数必须是 3 的倍数";
        for (const std::uint32_t index : mesh.indices) {
            ASSERT_LT(index, mesh.vertices.size()) << "索引越界（必须 < 顶点数）";
        }
    }
}

TEST(ObjectMesh, NormalsAreUnitLengthAndFaceOutward) {
    for (const ObjectType& type : AllKinds()) {
        const MeshData mesh = BuildObjectMesh(type);
        for (const MeshVertex& vertex : mesh.vertices) {
            const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                                           vertex.normal[1] * vertex.normal[1] +
                                           vertex.normal[2] * vertex.normal[2]);
            ASSERT_NEAR(length, 1.0F, kEps) << "法线必须是单位向量";
        }
        for (std::size_t i = 0; i + 3U <= mesh.indices.size(); i += 3U) {
            const MeshVertex& a = mesh.vertices[mesh.indices[i + 0U]];
            const MeshVertex& b = mesh.vertices[mesh.indices[i + 1U]];
            const MeshVertex& c = mesh.vertices[mesh.indices[i + 2U]];

            const float e1[3] = { b.position[0] - a.position[0], b.position[1] - a.position[1],
                                  b.position[2] - a.position[2] };
            const float e2[3] = { c.position[0] - a.position[0], c.position[1] - a.position[1],
                                  c.position[2] - a.position[2] };
            const float face[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                    e1[0] * e2[1] - e1[1] * e2[0] };
            const float avg[3] = { a.normal[0] + b.normal[0] + c.normal[0],
                                   a.normal[1] + b.normal[1] + c.normal[1],
                                   a.normal[2] + b.normal[2] + c.normal[2] };
            const float dot = face[0] * avg[0] + face[1] * avg[1] + face[2] * avg[2];
            ASSERT_GT(dot, 0.0F) << "三角形绕序必须与向外法线一致（从外部看为逆时针）";
        }
    }
}

TEST(ObjectMesh, BaseSitsAtZeroAndBoundsMatchHalfExtents) {
    for (const ObjectType& type : AllKinds()) {
        const MeshData mesh = BuildObjectMesh(type);
        float minY = mesh.vertices.front().position[1];
        float maxY = minY;
        for (const MeshVertex& vertex : mesh.vertices) {
            minY = std::min(minY, vertex.position[1]);
            maxY = std::max(maxY, vertex.position[1]);
            ASSERT_LE(std::fabs(vertex.position[0]), type.halfExtentX + kEps);
            ASSERT_LE(std::fabs(vertex.position[2]), type.halfExtentZ + kEps);
        }
        EXPECT_NEAR(minY, 0.0F, kEps) << "底面必须在 y = 0（底面中心为原点）";
        EXPECT_NEAR(maxY, 2.0F * type.halfExtentY, kEps) << "顶面必须落在 y = 2 * half_extent_y";
    }
}

TEST(ObjectMesh, VerticesCarryTheKindMaterialSlot) {
    EXPECT_FLOAT_EQ(ObjectMaterialSlot(ObjectAssetKind::DirtPile), 1.0F);
    EXPECT_FLOAT_EQ(ObjectMaterialSlot(ObjectAssetKind::Stone), 2.0F);
    EXPECT_FLOAT_EQ(ObjectMaterialSlot(ObjectAssetKind::Crate), 1.0F);
    EXPECT_FLOAT_EQ(ObjectMaterialSlot(ObjectAssetKind::Portal), 2.0F);

    for (const ObjectType& type : AllKinds()) {
        const MeshData mesh      = BuildObjectMesh(type);
        const float    expected  = ObjectMaterialSlot(type.kind);
        for (const MeshVertex& vertex : mesh.vertices) {
            EXPECT_FLOAT_EQ(vertex.material, expected);
        }
    }
}

TEST(ObjectMesh, BuildIsDeterministic) {
    for (const ObjectType& type : AllKinds()) {
        const MeshData first  = BuildObjectMesh(type);
        const MeshData second = BuildObjectMesh(type);
        ASSERT_EQ(first.vertices.size(), second.vertices.size());
        ASSERT_EQ(first.indices, second.indices);
        for (std::size_t i = 0; i < first.vertices.size(); ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                EXPECT_FLOAT_EQ(first.vertices[i].position[axis], second.vertices[i].position[axis]);
                EXPECT_FLOAT_EQ(first.vertices[i].normal[axis], second.vertices[i].normal[axis]);
            }
        }
    }
}

TEST(ObjectMesh, RotateAboutYRotatesPositionsAndNormals) {
    MeshData mesh;
    MeshVertex vertex;
    vertex.position[0] = 1.0F;
    vertex.normal[0]   = 1.0F;
    mesh.vertices.push_back(vertex);

    const MeshData rotated = RotateMeshAboutY(mesh, 90.0F);
    ASSERT_EQ(rotated.vertices.size(), 1U);
    // 绕 +Y 右手旋转 90°：(1,0,0) → (0,0,-1)。
    EXPECT_NEAR(rotated.vertices[0].position[0], 0.0F, kEps);
    EXPECT_NEAR(rotated.vertices[0].position[2], -1.0F, kEps);
    EXPECT_NEAR(rotated.vertices[0].normal[0], 0.0F, kEps);
    EXPECT_NEAR(rotated.vertices[0].normal[2], -1.0F, kEps);

    // 旋转 0° ⇒ 恒等（逐位）。
    const MeshData identity = RotateMeshAboutY(mesh, 0.0F);
    EXPECT_FLOAT_EQ(identity.vertices[0].position[0], 1.0F);
    EXPECT_FLOAT_EQ(identity.vertices[0].position[2], 0.0F);

    // 互逆：+90° 再 −90° 回到原值。
    const MeshData restored = RotateMeshAboutY(rotated, -90.0F);
    EXPECT_NEAR(restored.vertices[0].position[0], 1.0F, kEps);
    EXPECT_NEAR(restored.vertices[0].position[2], 0.0F, kEps);
}

TEST(ObjectMesh, RotateAboutYSwapsFootprintForNonSquareBase) {
    const ObjectType type = MakeType(ObjectAssetKind::Stone, 2.0F, 0.5F, 1.0F);
    const MeshData   mesh = BuildObjectMesh(type);

    float maxX = 0.0F;
    float maxZ = 0.0F;
    for (const MeshVertex& vertex : mesh.vertices) {
        maxX = std::max(maxX, std::fabs(vertex.position[0]));
        maxZ = std::max(maxZ, std::fabs(vertex.position[2]));
    }

    const MeshData rotated = RotateMeshAboutY(mesh, 90.0F);
    float rotatedMaxX = 0.0F;
    float rotatedMaxZ = 0.0F;
    for (const MeshVertex& vertex : rotated.vertices) {
        rotatedMaxX = std::max(rotatedMaxX, std::fabs(vertex.position[0]));
        rotatedMaxZ = std::max(rotatedMaxZ, std::fabs(vertex.position[2]));
    }
    EXPECT_NEAR(rotatedMaxX, maxZ, kEps);
    EXPECT_NEAR(rotatedMaxZ, maxX, kEps);
}

/// 传送门：门环**留出可穿过的门洞**（环心不填实），洞宽 / 洞高与 `half_extent` 的解析关系一致。
TEST(ObjectMesh, PortalRingLeavesWalkThroughAperture) {
    const ObjectType type = MakeType(ObjectAssetKind::Portal, 1.0F, 1.5F, 0.2F);
    const MeshData   mesh = BuildObjectMesh(type);

    const float holeHalfWidth  = type.halfExtentX - 2.0F * type.halfExtentZ;  // 0.6
    const float holeHalfHeight = type.halfExtentY - 2.0F * type.halfExtentZ;  // 1.1
    ASSERT_GT(holeHalfWidth, 0.0F);
    ASSERT_GT(holeHalfHeight, 0.0F);

    // 管半径 = half_extent_z ⇒ x 轴上应当同时有**外缘**（halfX）与**内缘**（halfX − 2*halfZ）顶点。
    bool  hasOuterRim     = false;
    bool  hasInnerRim     = false;
    float nearestToCenter = std::numeric_limits<float>::max();
    for (const MeshVertex& vertex : mesh.vertices) {
        const float x = vertex.position[0];
        const float y = vertex.position[1];
        const float z = vertex.position[2];
        if (std::fabs(z) < kEps && std::fabs(y - type.halfExtentY) < kEps) {
            hasOuterRim = hasOuterRim || std::fabs(x - type.halfExtentX) < kEps;
            hasInnerRim = hasInnerRim || std::fabs(x - holeHalfWidth) < kEps;
        }
        const float dy  = y - type.halfExtentY;
        nearestToCenter = std::min(nearestToCenter, std::sqrt(x * x + dy * dy + z * z));
    }
    EXPECT_TRUE(hasOuterRim) << "缺少外缘顶点（x = halfX）";
    EXPECT_TRUE(hasInnerRim) << "缺少内缘顶点（x = halfX − 2*halfZ）";
    // 环心附近**无**顶点 ⇒ 门洞是**通的**（最近表面 = 内缘 ⇒ 等于门洞半宽）。
    EXPECT_NEAR(nearestToCenter, holeHalfWidth, kEps) << "门洞必须留空（不得填实）";
}

// ------------------------- 外部模型（V8）-------------------------

/// 造一个"外部模型"（模拟 GLB 载入结果）：原始尺寸 **0.5 × 2.0 × 1.0**，且**不在原点**（min = (1, 3, −0.5)）。
[[nodiscard]] Model MakeTriangleModel() {
    Model     model;
    ModelMesh mesh;
    mesh.name = "tri";
    const glm::vec3 positions[3] = { { 1.0F, 3.0F, -0.5F }, { 1.5F, 5.0F, -0.5F }, { 1.0F, 3.0F, 0.5F } };
    for (const glm::vec3& position : positions) {
        ModelVertex vertex;
        vertex.position = position;
        vertex.normal   = glm::vec3(0.0F, 1.0F, 0.0F);
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = { 0U, 1U, 2U };
    model.meshes.push_back(std::move(mesh));
    return model;
}

TEST(ObjectMesh, ModelScalesUniformlyIntoTheHalfExtentBoxAndSitsOnGround) {
    const Model model = MakeTriangleModel();
    // 目标盒 2 × 4 × 2（半尺寸 1 / 2 / 1）：各轴比例 4 / 2 / 2 ⇒ 等比因子取**最小** = 2（不拉伸）。
    const MeshData mesh = BuildObjectMeshFromModel(model, 1.0F, 2.0F, 1.0F, 2.0F);
    ASSERT_EQ(mesh.vertices.size(), 3U);
    ASSERT_EQ(mesh.indices.size(), 3U);

    float minP[3] = { mesh.vertices[0].position[0], mesh.vertices[0].position[1], mesh.vertices[0].position[2] };
    float maxP[3] = { minP[0], minP[1], minP[2] };
    for (const MeshVertex& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            minP[axis] = std::min(minP[axis], vertex.position[axis]);
            maxP[axis] = std::max(maxP[axis], vertex.position[axis]);
        }
        EXPECT_FLOAT_EQ(vertex.material, 2.0F);  // 材质槽位原样透传
    }
    EXPECT_NEAR(minP[1], 0.0F, kEps) << "底面必须贴 y = 0";
    EXPECT_NEAR(maxP[1], 4.0F, kEps) << "高度 = 2 * halfY（Y 轴是绑定轴）";
    EXPECT_NEAR((minP[0] + maxP[0]) * 0.5F, 0.0F, kEps) << "水平居中（X）";
    EXPECT_NEAR((minP[2] + maxP[2]) * 0.5F, 0.0F, kEps) << "水平居中（Z）";
    // **等比**：各轴尺寸之比与模型一致（0.5 : 2.0 : 1.0 ⇒ 缩放后 1.0 : 4.0 : 2.0）。
    EXPECT_NEAR(maxP[0] - minP[0], 1.0F, kEps);
    EXPECT_NEAR(maxP[2] - minP[2], 2.0F, kEps);
}

TEST(ObjectMesh, ModelBuildIsDeterministicAndSkipsEmptyOrDegenerate) {
    const Model    model  = MakeTriangleModel();
    const MeshData first  = BuildObjectMeshFromModel(model, 1.0F, 1.0F, 1.0F, 0.0F);
    const MeshData second = BuildObjectMeshFromModel(model, 1.0F, 1.0F, 1.0F, 0.0F);
    ASSERT_EQ(first.vertices.size(), second.vertices.size());
    ASSERT_EQ(first.indices, second.indices);
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_FLOAT_EQ(first.vertices[i].position[axis], second.vertices[i].position[axis]);
        }
    }

    // 空模型 ⇒ 空网格（调用方按"网格为空"跳过上传，与既有口径一致）。
    const Model empty;
    EXPECT_TRUE(BuildObjectMeshFromModel(empty, 1.0F, 1.0F, 1.0F, 0.0F).vertices.empty());

    // 退化模型（所有顶点重合）⇒ 空网格（**不产生除零 / NaN**）。
    Model     degenerate;
    ModelMesh mesh;
    for (int i = 0; i < 3; ++i) {
        ModelVertex vertex;
        vertex.position = glm::vec3(2.0F, 2.0F, 2.0F);
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = { 0U, 1U, 2U };
    degenerate.meshes.push_back(std::move(mesh));
    EXPECT_TRUE(BuildObjectMeshFromModel(degenerate, 1.0F, 1.0F, 1.0F, 0.0F).vertices.empty());
}

TEST(ObjectMesh, ModelWithFlatAxisUsesRemainingAxesForScale) {
    // 平面模型（Y 尺寸为 0）：缩放因子只能由 X / Z 决定，且不得产生 NaN。
    Model     model;
    ModelMesh mesh;
    const glm::vec3 positions[3] = { { 0.0F, 4.0F, 0.0F }, { 2.0F, 4.0F, 0.0F }, { 0.0F, 4.0F, 1.0F } };
    for (const glm::vec3& position : positions) {
        ModelVertex vertex;
        vertex.position = position;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = { 0U, 1U, 2U };
    model.meshes.push_back(std::move(mesh));

    const MeshData out = BuildObjectMeshFromModel(model, 1.0F, 1.0F, 1.0F, 1.0F);
    ASSERT_EQ(out.vertices.size(), 3U);
    for (const MeshVertex& vertex : out.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_TRUE(std::isfinite(vertex.position[axis]));
        }
        EXPECT_NEAR(vertex.position[1], 0.0F, kEps);  // 平面贴地
    }
}

}  // namespace
