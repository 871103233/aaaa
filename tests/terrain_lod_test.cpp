// 地表 LOD 分环的核心几何测试（W7-S3b / ADR 0024「二、LOD 分环」）。
//
// 覆盖：① LOD 步长 / 顶点数 / 索引数口径；② LOD1/LOD2 顶点网格与局部列步长；③ morph 目标高度取自
// **父级网格**对应列；④ 相邻环在共享边界列上几何**逐位一致**（morph 消接缝的前提）；⑤ `lodLevel == 0`
// 与旧行为（不传 lod）**逐位一致**；⑥ 同输入两次调用确定性相同。

#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {

using vx::Height;
using vx::HeightToBlocks;
using vx::MeshVertex;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainTileMesh;
using vx::TerrainWorld;

/// ADR 0008 §3 固定基准场景的世界种子（与其它地表测试同源）。
constexpr std::uint64_t kSeed = 0x5EED0001ULL;

/// 人造高度：随世界列坐标变化，含奇偶差异 ⇒ 可区分"取本列"与"取父级网格列"。
Height ArtificialHeight(int columnX, int columnZ) {
    const int units = (columnX * 7 + columnZ * 13) % 200 + 10;  // 10 ~ 209 格
    return static_cast<Height>(units * vx::kHeightUnitsPerBlock);
}

/// 逐位比较两份网格（顶点全字段 + 索引）。
void ExpectMeshBitEqual(const TerrainTileMesh& a, const TerrainTileMesh& b) {
    ASSERT_EQ(a.mesh.vertices.size(), b.mesh.vertices.size());
    ASSERT_EQ(a.mesh.indices.size(), b.mesh.indices.size());
    for (std::size_t k = 0; k < a.mesh.vertices.size(); ++k) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(a.mesh.vertices[k].position[c], b.mesh.vertices[k].position[c]) << "顶点 " << k;
            EXPECT_FLOAT_EQ(a.mesh.vertices[k].normal[c], b.mesh.vertices[k].normal[c]) << "顶点 " << k;
        }
        EXPECT_FLOAT_EQ(a.mesh.vertices[k].material, b.mesh.vertices[k].material) << "顶点 " << k;
        EXPECT_FLOAT_EQ(a.mesh.vertices[k].morph, b.mesh.vertices[k].morph) << "顶点 " << k;
    }
    for (std::size_t k = 0; k < a.mesh.indices.size(); ++k) {
        EXPECT_EQ(a.mesh.indices[k], b.mesh.indices[k]) << "索引 " << k;
    }
}

}  // namespace

// ① LOD 步长 / 顶点数 / 索引数口径（ADR 0024：Ring 0 步长 1、Ring 1 步长 2、Ring 2 步长 4）。
TEST(TerrainLod, MetricsMatchAdr0024) {
    EXPECT_EQ(vx::kTerrainLodLevelCount, 3);

    EXPECT_EQ(vx::TerrainLodStep(0), 1);
    EXPECT_EQ(vx::TerrainLodStep(1), 2);
    EXPECT_EQ(vx::TerrainLodStep(2), 4);

    // 父级网格间距 = 2 × 步长。
    EXPECT_EQ(vx::TerrainLodSnapStep(0), 2);
    EXPECT_EQ(vx::TerrainLodSnapStep(1), 4);
    EXPECT_EQ(vx::TerrainLodSnapStep(2), 8);

    EXPECT_EQ(vx::TerrainLodVertexSide(0), 65);
    EXPECT_EQ(vx::TerrainLodVertexSide(1), 33);
    EXPECT_EQ(vx::TerrainLodVertexSide(2), 17);

    EXPECT_EQ(vx::TerrainLodVertexCount(0), 65 * 65);
    EXPECT_EQ(vx::TerrainLodVertexCount(1), 33 * 33);
    EXPECT_EQ(vx::TerrainLodVertexCount(2), 17 * 17);

    EXPECT_EQ(vx::TerrainLodIndexCount(0), 24576);
    EXPECT_EQ(vx::TerrainLodIndexCount(1), 6144);
    EXPECT_EQ(vx::TerrainLodIndexCount(2), 1536);
}

// ② LOD0 满地表网格：顶点 65²、索引 24576（与既有口径一致）。
TEST(TerrainLod, Lod0MeshCountsMatchFullDetail) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);

    const TerrainTileMesh mesh = vx::BuildTerrainMesh(*tile);
    EXPECT_EQ(mesh.lodLevel, 0);
    EXPECT_EQ(mesh.verticesPerSide, 65);
    EXPECT_EQ(mesh.mesh.vertices.size(), std::size_t { 65 } * 65U);
    EXPECT_EQ(mesh.mesh.indices.size(), std::size_t { 24576 });
}

// ② LOD1 顶点数 33²、边长 33；每个顶点局部坐标都是 2 的倍数，且高度取自对应世界列。
TEST(TerrainLod, Lod1VertexGridUsesStepTwo) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);

    const TerrainTileMesh mesh = vx::BuildTerrainMesh(*tile, nullptr, 1);
    EXPECT_EQ(mesh.lodLevel, 1);
    EXPECT_EQ(mesh.verticesPerSide, 33);
    ASSERT_EQ(mesh.mesh.vertices.size(), std::size_t { 33 } * 33U);
    EXPECT_EQ(mesh.mesh.indices.size(), std::size_t { 6144 });

    for (int j = 0; j < 33; ++j) {
        for (int i = 0; i < 33; ++i) {
            const MeshVertex& vertex = mesh.mesh.vertices[TerrainTileMesh::VertexIndex(i, j, 33)];
            EXPECT_FLOAT_EQ(vertex.position[0], static_cast<float>(2 * i));
            EXPECT_FLOAT_EQ(vertex.position[2], static_cast<float>(2 * j));
            EXPECT_FLOAT_EQ(vertex.position[1], HeightToBlocks(tile->At(2 * i, 2 * j)));
            EXPECT_EQ(static_cast<int>(vertex.position[0]) % 2, 0);
            EXPECT_EQ(static_cast<int>(vertex.position[2]) % 2, 0);
        }
    }
}

// ② LOD2 顶点数 17²、索引 1536。
TEST(TerrainLod, Lod2VertexGridIsSparse) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);

    const TerrainTileMesh mesh = vx::BuildTerrainMesh(*tile, nullptr, 2);
    EXPECT_EQ(mesh.lodLevel, 2);
    EXPECT_EQ(mesh.verticesPerSide, 17);
    EXPECT_EQ(mesh.mesh.vertices.size(), std::size_t { 17 } * 17U);
    EXPECT_EQ(mesh.mesh.indices.size(), std::size_t { 1536 });
}

// ③ morph 目标正确（LOD1）：顶点 (i, j) 的 morph == 父级网格列（2i / 4 × 4，2 的 2 次幂对齐）的采样高度。
//    `TerrainLodSnapStep(1) == 4` ⇒ 目标列 = `(2i / 4) * 4`（等价于 `(2i) & ~3`）。
TEST(TerrainLod, Lod1MorphTargetsParentGridColumn) {
    TerrainTile tile;
    tile.coord = vx::TileCoord { 0, 0 };
    for (int j = 0; j <= vx::kTerrainTileSize; ++j) {
        for (int i = 0; i <= vx::kTerrainTileSize; ++i) {
            tile.SetAt(i, j, ArtificialHeight(i, j));
        }
    }

    const TerrainTileMesh mesh = vx::BuildTerrainMesh(tile, nullptr, 1);
    for (int j = 0; j < 33; ++j) {
        for (int i = 0; i < 33; ++i) {
            const int targetX = (2 * i / 4) * 4;
            const int targetZ = (2 * j / 4) * 4;
            const MeshVertex& vertex = mesh.mesh.vertices[TerrainTileMesh::VertexIndex(i, j, 33)];
            EXPECT_FLOAT_EQ(vertex.morph, HeightToBlocks(tile.At(targetX, targetZ)))
                << "顶点 (" << i << ", " << j << ") 的 morph 目标列错误";
        }
    }

    // 几组人造高度的定点断言：
    EXPECT_FLOAT_EQ(mesh.mesh.vertices[TerrainTileMesh::VertexIndex(0, 0, 33)].morph,
                    HeightToBlocks(tile.At(0, 0)));
    EXPECT_FLOAT_EQ(mesh.mesh.vertices[TerrainTileMesh::VertexIndex(1, 0, 33)].morph,
                    HeightToBlocks(tile.At(0, 0))) << "局部列 2 向下对齐到父级网格列 0";
    EXPECT_FLOAT_EQ(mesh.mesh.vertices[TerrainTileMesh::VertexIndex(32, 32, 33)].morph,
                    HeightToBlocks(tile.At(64, 64))) << "边界列 64 必须 snap 回 64（不越界）";
}

// ④ 跨环接缝：相邻两块共享边界列，左块 LOD0、右块 LOD1；共享边界上的粗块顶点 (x, z, y) 与细块在
//    **同一世界列**的顶点逐位相同（高度由共享采样保证，位置由同一世界列保证）。
TEST(TerrainLod, AdjacentRingsShareBoundaryVertices) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);
    const TerrainTile* left  = world.FindTile(0, 0);
    const TerrainTile* right = world.FindTile(1, 0);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_EQ(left->At(vx::kTerrainTileSize, 0), right->At(0, 0)) << "共享边界列必须取同一采样";

    const TerrainTileMesh fineTile   = vx::BuildTerrainMesh(*left, nullptr, 0);   // 细块：步长 1
    const TerrainTileMesh coarseTile = vx::BuildTerrainMesh(*right, nullptr, 1);  // 粗块：步长 2

    // 共享世界列 x = 64：细块本地列 64、粗块本地列 0（tile 原点不同）⇒ 必须比较**世界位置**。
    // 粗块顶点 j 对应世界行 z = 2j。
    for (int j = 0; j < 33; ++j) {
        const std::size_t fineIndex =
            TerrainTileMesh::VertexIndex(vx::kTerrainTileSize, 2 * j, vx::kTerrainTileVertexCount);
        const std::size_t coarseIndex = TerrainTileMesh::VertexIndex(0, j, 33);

        const glm::dvec3 fine   = fineTile.WorldPosition(fineIndex);
        const glm::dvec3 coarse = coarseTile.WorldPosition(coarseIndex);

        EXPECT_DOUBLE_EQ(fine.x, coarse.x) << "世界列 x，j=" << j;
        EXPECT_DOUBLE_EQ(fine.y, coarse.y) << "共享边界高度，j=" << j;
        EXPECT_DOUBLE_EQ(fine.z, coarse.z) << "世界行 z，j=" << j;
    }
}

// ⑤/⑥ LOD0 与旧行为逐位一致，且同输入两次调用确定性相同。
TEST(TerrainLod, LodZeroMatchesLegacyAndIsDeterministic) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(2, -1);
    const TerrainTile* tile = world.FindTile(2, -1);
    ASSERT_NE(tile, nullptr);

    const TerrainTileMesh legacy       = vx::BuildTerrainMesh(*tile);          // 不传 lodLevel（旧调用形态）
    const TerrainTileMesh explicitZero = vx::BuildTerrainMesh(*tile, nullptr, 0);
    const TerrainTileMesh again        = vx::BuildTerrainMesh(*tile, nullptr, 0);

    EXPECT_EQ(legacy.lodLevel, 0);
    EXPECT_EQ(legacy.verticesPerSide, vx::kTerrainTileVertexCount);
    ExpectMeshBitEqual(legacy, explicitZero);
    ExpectMeshBitEqual(explicitZero, again);
}
