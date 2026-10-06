// 物件选择器预览单测（V0.5 E4；ADR 0032）：CPU 正交投影 + 朗伯明暗的**纯函数**语义 ——
// 空 / 退化网格无预览、背面剔除、归一化落在 [0,1]²、深度排序（画家算法）、确定性、转动时不越界。
// 见 docs/plans/v0.5.md §1.20。

#include "object_preview.hpp"

#include "object/object_layer.hpp"
#include "object/object_mesh.hpp"

#include <cstddef>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

namespace {

using vx::BuildObjectPreview;
using vx::MeshData;
using vx::MeshVertex;
using vx::ObjectAssetKind;
using vx::ObjectType;
using vx::PreviewTriangle;

constexpr float kTwoPi = 6.283185307179586F;

MeshVertex MakeVertex(float x, float y, float z, float nx, float ny, float nz) {
    MeshVertex vertex;
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.normal[0]   = nx;
    vertex.normal[1]   = ny;
    vertex.normal[2]   = nz;
    return vertex;
}

/// 单个三角形：顶点在 XY 平面、法线朝 `+Z`（相机在 `+Z` 侧 ⇒ 正面可见）。
MeshData MakeFrontFacingTriangle(float z) {
    MeshData mesh;
    mesh.vertices.push_back(MakeVertex(-1.0F, 0.0F, z, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(1.0F, 0.0F, z, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(0.0F, 1.0F, z, 0.0F, 0.0F, 1.0F));
    mesh.indices = { 0U, 1U, 2U };
    return mesh;
}

/// 一个程序化木箱（闭合盒 ⇒ 任意朝向都有可见面）。
MeshData MakeCrateMesh() {
    ObjectType type;
    type.id          = "crate";
    type.kind        = ObjectAssetKind::Crate;
    type.halfExtentX = 0.5F;
    type.halfExtentY = 0.5F;
    type.halfExtentZ = 0.5F;
    return vx::BuildObjectMesh(type);
}

/// 所有输出坐标都在单位盒内（含 1e-4 容差）。
void ExpectInsideUnitBox(const std::vector<PreviewTriangle>& triangles) {
    for (const PreviewTriangle& triangle : triangles) {
        for (const glm::vec2* point : { &triangle.a, &triangle.b, &triangle.c }) {
            EXPECT_GE(point->x, -1.0e-4F);
            EXPECT_GE(point->y, -1.0e-4F);
            EXPECT_LE(point->x, 1.0F + 1.0e-4F);
            EXPECT_LE(point->y, 1.0F + 1.0e-4F);
        }
        EXPECT_GE(triangle.shade, 0.0F);
        EXPECT_LE(triangle.shade, 1.0F);
    }
}

TEST(ObjectPreview, EmptyMeshYieldsNoPreview) {
    EXPECT_TRUE(BuildObjectPreview(MeshData {}, 0.0F, 0.0F).empty());
}

TEST(ObjectPreview, DegenerateMeshYieldsNoPreview) {
    MeshData mesh;  // 三个顶点重合 ⇒ 包围球半径 0
    mesh.vertices.push_back(MakeVertex(1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F));
    mesh.indices = { 0U, 1U, 2U };
    EXPECT_TRUE(BuildObjectPreview(mesh, 0.0F, 0.0F).empty());
}

TEST(ObjectPreview, BackFacingTriangleIsCulled) {
    MeshData mesh;
    mesh.vertices.push_back(MakeVertex(-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F));  // 法线朝 −Z ⇒ 背面
    mesh.vertices.push_back(MakeVertex(1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F));
    mesh.vertices.push_back(MakeVertex(0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F));
    mesh.indices = { 0U, 1U, 2U };
    EXPECT_TRUE(BuildObjectPreview(mesh, 0.0F, 0.0F).empty());
}

TEST(ObjectPreview, FrontFacingTriangleProjectsInsideUnitBox) {
    const std::vector<PreviewTriangle> triangles = BuildObjectPreview(MakeFrontFacingTriangle(0.0F), 0.0F, 0.0F);
    ASSERT_EQ(triangles.size(), 1U);
    ExpectInsideUnitBox(triangles);
}

TEST(ObjectPreview, EdgeOnFlatTriangleCullsEntirely) {
    // 绕 Y 转 90° ⇒ 平面变"侧对相机"，法线指向 ±X ⇒ `normal.z ≤ 0` ⇒ 整面被剔除。
    EXPECT_TRUE(BuildObjectPreview(MakeFrontFacingTriangle(0.0F), 0.5F * kTwoPi, 0.0F).empty());
}

TEST(ObjectPreview, FarTriangleIsDrawnFirst) {
    MeshData mesh = MakeFrontFacingTriangle(0.0F);  // 近（z = 0）：顶点 y = 0
    // 追加一个更远的三角形（z = −2）、顶点 y = 5（便于区分）
    mesh.vertices.push_back(MakeVertex(-1.0F, 5.0F, -2.0F, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(1.0F, 5.0F, -2.0F, 0.0F, 0.0F, 1.0F));
    mesh.vertices.push_back(MakeVertex(0.0F, 6.0F, -2.0F, 0.0F, 0.0F, 1.0F));
    mesh.indices.insert(mesh.indices.end(), { 3U, 4U, 5U });

    const std::vector<PreviewTriangle> triangles = BuildObjectPreview(mesh, 0.0F, 0.0F);
    ASSERT_EQ(triangles.size(), 2U);
    // 画家算法：远（z 小）先画 ⇒ 第一个输出应是 y 较大（y = 5/6 那一组）。
    EXPECT_LT(triangles[0].a.y, triangles[1].a.y);
    ExpectInsideUnitBox(triangles);
}

TEST(ObjectPreview, PreviewIsDeterministic) {
    const MeshData mesh = MakeCrateMesh();
    const std::vector<PreviewTriangle> first  = BuildObjectPreview(mesh, 1.0F, vx::kObjectPreviewPitchRadians);
    const std::vector<PreviewTriangle> second = BuildObjectPreview(mesh, 1.0F, vx::kObjectPreviewPitchRadians);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i].a.x, second[i].a.x);
        EXPECT_FLOAT_EQ(first[i].a.y, second[i].a.y);
        EXPECT_FLOAT_EQ(first[i].shade, second[i].shade);
    }
}

TEST(ObjectPreview, ClosedMeshStaysInsideUnitBoxAcrossYaw) {
    const MeshData mesh = MakeCrateMesh();
    ASSERT_FALSE(mesh.vertices.empty());
    for (int step = 0; step < 24; ++step) {
        const float yaw = kTwoPi * static_cast<float>(step) / 24.0F;
        const std::vector<PreviewTriangle> triangles =
            BuildObjectPreview(mesh, yaw, vx::kObjectPreviewPitchRadians);
        EXPECT_FALSE(triangles.empty()) << "yaw = " << yaw;  // 闭合盒任意朝向都有可见面
        ExpectInsideUnitBox(triangles);
    }
}

}  // namespace
