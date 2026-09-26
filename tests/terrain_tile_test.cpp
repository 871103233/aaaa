#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"

#include <glm/vec3.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {

using vx::Height;
using vx::HeightToBlocks;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainTileMesh;
using vx::TerrainWorld;

/// ADR 0008 §3 固定基准场景的世界种子。
constexpr std::uint64_t kSeed = 0x5EED0001ULL;

void LoadTiles(TerrainWorld& world, int minTileX, int maxTileX, int minTileZ, int maxTileZ) {
    for (int tileZ = minTileZ; tileZ <= maxTileZ; ++tileZ) {
        for (int tileX = minTileX; tileX <= maxTileX; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }
}

}  // namespace

// T4 ①：相邻 tile 的共享边界顶点必须**逐位相等** —— 位置不同即出现裂缝（red line 12）。
TEST(TerrainBoundary, SharedBoundaryVerticesAreIdentical) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    LoadTiles(world, 0, 1, 0, 0);

    const TerrainTileMesh* left  = world.FindMesh(0, 0);
    const TerrainTileMesh* right = world.FindMesh(1, 0);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        // 左 tile 的本地第 64 列 与 右 tile 的本地第 0 列 是同一个世界列。
        const std::size_t leftIndex  = TerrainTileMesh::VertexIndex(vx::kTerrainTileSize, j);
        const std::size_t rightIndex = TerrainTileMesh::VertexIndex(0, j);

        const glm::dvec3 leftPosition  = left->WorldPosition(leftIndex);
        const glm::dvec3 rightPosition = right->WorldPosition(rightIndex);

        ASSERT_DOUBLE_EQ(leftPosition.x, rightPosition.x) << "本地行 j=" << j;
        ASSERT_DOUBLE_EQ(leftPosition.y, rightPosition.y) << "本地行 j=" << j;
        ASSERT_DOUBLE_EQ(leftPosition.z, rightPosition.z) << "本地行 j=" << j;
    }
}

// T4 ②：生成是纯函数 —— 同种子 + 同坐标必然逐值一致；换种子必然至少一列不同。
TEST(TerrainGeneration, IsDeterministicForSameSeedAndCoords) {
    TerrainWorld first(kSeed, TerrainMaterialTable::Default());
    TerrainWorld second(kSeed, TerrainMaterialTable::Default());
    first.GenerateTile(-3, 2);
    second.GenerateTile(-3, 2);

    const TerrainTile* a = first.FindTile(-3, 2);
    const TerrainTile* b = second.FindTile(-3, 2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            ASSERT_EQ(a->At(i, j), b->At(i, j)) << "本地坐标 (" << i << ", " << j << ")";
        }
    }

    TerrainWorld other(kSeed + 1U, TerrainMaterialTable::Default());
    other.GenerateTile(-3, 2);
    const TerrainTile* c = other.FindTile(-3, 2);
    ASSERT_NE(c, nullptr);

    bool anyDifferent = false;
    for (int j = 0; j < vx::kTerrainTileVertexCount && !anyDifferent; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            if (a->At(i, j) != c->At(i, j)) {
                anyDifferent = true;
                break;
            }
        }
    }
    EXPECT_TRUE(anyDifferent);
}

// T4 ③：tile 网格是合法的索引缓冲 —— 顶点数、索引数正确且所有索引都在范围内。
TEST(TerrainMesh, IndexBufferIsValidAndInRange) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(2, -1);

    const TerrainTileMesh* mesh = world.FindMesh(2, -1);
    ASSERT_NE(mesh, nullptr);

    const std::size_t expectedVertices =
        static_cast<std::size_t>(vx::kTerrainTileVertexCount) * static_cast<std::size_t>(vx::kTerrainTileVertexCount);
    const std::size_t expectedIndices =
        static_cast<std::size_t>(vx::kTerrainTileSize) * static_cast<std::size_t>(vx::kTerrainTileSize) * 6U;

    EXPECT_EQ(mesh->mesh.vertices.size(), expectedVertices);
    EXPECT_EQ(mesh->mesh.indices.size(), expectedIndices);
    EXPECT_EQ(mesh->mesh.indices.size() % 3U, 0U);

    for (const std::uint32_t index : mesh->mesh.indices) {
        ASSERT_LT(index, expectedVertices);
    }
}

// ITerrainQuery 契约：已加载区域能报出高度与遮挡，未加载区域返回 false。
TEST(TerrainQuery, ReportsHeightAndObstructionWithinLoadedTiles) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);

    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);
    const Height columnHeight = tile->At(10, 20);

    float height = 0.0F;
    ASSERT_TRUE(world.QueryHeight(10.5F, 20.5F, height));
    EXPECT_NEAR(height, HeightToBlocks(columnHeight), 1e-5F);

    float missing = 0.0F;
    EXPECT_FALSE(world.QueryHeight(10000.0F, 10000.0F, missing));

    const float     surface = HeightToBlocks(columnHeight);
    const glm::vec3 below(10.5F, surface - 5.0F, 20.5F);
    const glm::vec3 above(10.5F, surface + 5.0F, 20.5F);

    float safeT = 1.0F;
    EXPECT_TRUE(world.QueryObstruction(below, above, safeT));
    EXPECT_GE(safeT, 0.0F);
    EXPECT_LT(safeT, 1.0F);

    EXPECT_FALSE(world.QueryObstruction(above, above + glm::vec3(1.0F, 0.0F, 0.0F), safeT));
    EXPECT_FLOAT_EQ(safeT, 1.0F);
}
