#include "terrain/terrain_mesher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vx {
namespace {

/// 顶点高度（格）。
[[nodiscard]] float HeightAtBlocks(const TerrainTile& tile, int i, int j) noexcept {
    return HeightToBlocks(tile.At(i, j));
}

/// 由高度场梯度求顶点法线：`n = normalize(-df/dx, 1, -df/dz)`。
/// 边界顶点退化为单侧差分（否则会越界访问相邻 tile 的数据）。
[[nodiscard]] glm::vec3 ComputeNormal(const TerrainTile& tile, int i, int j) noexcept {
    const int iPrev = (i > 0) ? i - 1 : i;
    const int iNext = (i < kTerrainTileSize) ? i + 1 : i;
    const int jPrev = (j > 0) ? j - 1 : j;
    const int jNext = (j < kTerrainTileSize) ? j + 1 : j;

    const float dx = static_cast<float>(iNext - iPrev);
    const float dz = static_cast<float>(jNext - jPrev);

    const float dfdx = (HeightAtBlocks(tile, iNext, j) - HeightAtBlocks(tile, iPrev, j)) / dx;
    const float dfdz = (HeightAtBlocks(tile, i, jNext) - HeightAtBlocks(tile, i, jPrev)) / dz;

    return glm::normalize(glm::vec3(-dfdx, 1.0F, -dfdz));
}

}  // namespace

glm::dvec3 TerrainTileMesh::WorldPosition(std::size_t vertexIndex) const noexcept {
    const MeshVertex& vertex = mesh.vertices[vertexIndex];
    return glm::dvec3(static_cast<double>(TileOriginColumn(coord.x)) + static_cast<double>(vertex.position[0]),
                      static_cast<double>(vertex.position[1]),
                      static_cast<double>(TileOriginColumn(coord.z)) + static_cast<double>(vertex.position[2]));
}

TerrainTileMesh BuildTerrainMesh(const TerrainTile& tile, const MaterialBlender& blender,
                                 const TerrainMaterialTable& table) {
    TerrainTileMesh result;
    result.coord = tile.coord;

    const std::size_t vertexCount =
        static_cast<std::size_t>(kTerrainTileVertexCount) * static_cast<std::size_t>(kTerrainTileVertexCount);
    result.mesh.vertices.resize(vertexCount);

    for (int j = 0; j < kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < kTerrainTileVertexCount; ++i) {
            const float heightBlocks = HeightToBlocks(tile.At(i, j));
            const glm::vec3 normal   = ComputeNormal(tile, i, j);
            const float slope        = std::clamp(1.0F - normal.y, 0.0F, 1.0F);

            const std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> weights = blender.WeightsAt(
                table, static_cast<float>(tile.WorldColumnX(i)), static_cast<float>(tile.WorldColumnZ(j)), heightBlocks,
                slope);

            MeshVertex& vertex  = result.mesh.vertices[TerrainTileMesh::VertexIndex(i, j)];
            vertex.position[0]  = static_cast<float>(i);
            vertex.position[1]  = heightBlocks;
            vertex.position[2]  = static_cast<float>(j);
            vertex.normal[0]    = normal.x;
            vertex.normal[1]    = normal.y;
            vertex.normal[2]    = normal.z;
            for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
                vertex.materialWeights[static_cast<std::size_t>(slot)] = weights[static_cast<std::size_t>(slot)];
            }
        }
    }

    // 索引：每格两个三角形，绕序保证正面朝上。
    const std::size_t quadCount = static_cast<std::size_t>(kTerrainTileSize) * static_cast<std::size_t>(kTerrainTileSize);
    result.mesh.indices.reserve(quadCount * 6);
    for (int j = 0; j < kTerrainTileSize; ++j) {
        for (int i = 0; i < kTerrainTileSize; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i, j));
            const std::uint32_t b = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i + 1, j));
            const std::uint32_t c = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i, j + 1));
            const std::uint32_t d = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i + 1, j + 1));

            result.mesh.indices.push_back(a);
            result.mesh.indices.push_back(c);
            result.mesh.indices.push_back(b);
            result.mesh.indices.push_back(b);
            result.mesh.indices.push_back(c);
            result.mesh.indices.push_back(d);
        }
    }

    return result;
}

}  // namespace vx
