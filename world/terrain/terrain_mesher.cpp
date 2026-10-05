#include "terrain/terrain_mesher.hpp"

#include <algorithm>
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

TerrainTileMesh BuildTerrainMesh(const TerrainTile& tile, const ITerrainQuadFilter* quadFilter, int lodLevel) {
    TerrainTileMesh result;
    result.coord           = tile.coord;
    result.lodLevel        = lodLevel;
    result.verticesPerSide = TerrainLodVertexSide(lodLevel);

    const int step     = TerrainLodStep(lodLevel);
    const int snapStep = TerrainLodSnapStep(lodLevel);
    const int side     = result.verticesPerSide;

    // 父级网格对应列：把局部列向下对齐到父级网格间距（CDLOD，见 terrain_types.hpp 的 TerrainLodSnapStep）。
    const auto snapColumn = [snapStep](int column) noexcept {
        return std::min((column / snapStep) * snapStep, kTerrainTileSize);
    };

    const std::size_t vertexCount = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    result.mesh.vertices.resize(vertexCount);

    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const int gi = i * step;
            const int gj = j * step;
            // 法线**仍用 ±1 相邻列**算梯度（不是 ±step）：不同 LOD 在相同世界列上得到相同法线 ⇒ 跨环不着色接缝。
            const glm::vec3 normal = ComputeNormal(tile, gi, gj);

            MeshVertex& vertex = result.mesh.vertices[TerrainTileMesh::VertexIndex(i, j, side)];
            vertex.position[0] = static_cast<float>(gi);
            vertex.position[1] = HeightToBlocks(tile.At(gi, gj));
            vertex.position[2] = static_cast<float>(gj);
            vertex.normal[0]   = normal.x;
            vertex.normal[1]   = normal.y;
            vertex.normal[2]   = normal.z;
            // W7-S3b：morph 目标高度 = 该顶点在**父级网格**对应列上的采样高度（格）。LOD0 时 = 每 2 列。
            vertex.morph = HeightToBlocks(tile.At(snapColumn(gi), snapColumn(gj)));
        }
    }

    // 索引：每格两个三角形，绕序保证正面朝上。
    //
    // W7-S3b：**先建"未过滤"的全量索引**（`(j, i)` 升序、每格 `a, c, b, b, c, d`），
    // 再交给 `ApplyQuadFilterToMesh` 按 `quadFilter` 过滤 —— 复用同一个纯函数，
    // 保证"worker 产出 + 主线程过滤"与"主线程同步构建"两条路径**逐位一致**（红线 7）。
    const std::size_t quadCount = static_cast<std::size_t>(side - 1) * static_cast<std::size_t>(side - 1);
    result.mesh.indices.reserve(quadCount * 6);
    for (int j = 0; j < side - 1; ++j) {
        for (int i = 0; i < side - 1; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i, j, side));
            const std::uint32_t b = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i + 1, j, side));
            const std::uint32_t c = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i, j + 1, side));
            const std::uint32_t d = static_cast<std::uint32_t>(TerrainTileMesh::VertexIndex(i + 1, j + 1, side));

            result.mesh.indices.push_back(a);
            result.mesh.indices.push_back(c);
            result.mesh.indices.push_back(b);
            result.mesh.indices.push_back(b);
            result.mesh.indices.push_back(c);
            result.mesh.indices.push_back(d);
        }
    }
    ApplyQuadFilterToMesh(tile, quadFilter, lodLevel, result.mesh);

    // 接管判据（W7-S3b）：以过滤**之后**的空否为准 —— 上传后 CPU 侧网格会被释放，
    // 届时 `indices` 恒为空，故必须在网格化时把"本 tile 是否有可见面"固化到本标记。
    result.meshEmpty = result.mesh.indices.empty();
    return result;
}

void ApplyQuadFilterToMesh(const TerrainTile& tile, const ITerrainQuadFilter* quadFilter, int lodLevel,
                           MeshData& meshInOut) {
    if (quadFilter == nullptr) {
        return;  // 无过滤器 ⇒ 未过滤索引列表即结果（与旧行为逐位一致）
    }

    const int step            = TerrainLodStep(lodLevel);
    const int side            = TerrainLodVertexSide(lodLevel);
    const int originColumnX   = TileOriginColumn(tile.coord.x);
    const int originColumnZ   = TileOriginColumn(tile.coord.z);

    const std::size_t quadsPerSide = static_cast<std::size_t>(side - 1);
    const std::size_t quadCount    = quadsPerSide * quadsPerSide;

    std::vector<std::uint32_t> filtered;
    filtered.reserve(meshInOut.indices.size());

    // 顺序必须与 `BuildTerrainMesh` 建未过滤索引时**完全一致**（`(j, i)` 升序 ⇒ 行主序的格序号）。
    for (std::size_t quadIndex = 0; quadIndex < quadCount; ++quadIndex) {
        const int i  = static_cast<int>(quadIndex % quadsPerSide);
        const int j  = static_cast<int>(quadIndex / quadsPerSide);
        const int gi = i * step;
        const int gj = j * step;

        // 四角世界列 / 高度：与 `BuildTerrainMesh` 内的构造**逐字一致**（角序 00, 10, 01, 11）。
        TerrainQuad quad;
        quad.columnX[0] = originColumnX + gi;
        quad.columnZ[0] = originColumnZ + gj;
        quad.columnX[1] = originColumnX + gi + step;
        quad.columnZ[1] = originColumnZ + gj;
        quad.columnX[2] = originColumnX + gi;
        quad.columnZ[2] = originColumnZ + gj + step;
        quad.columnX[3] = originColumnX + gi + step;
        quad.columnZ[3] = originColumnZ + gj + step;
        quad.height[0]  = HeightAtBlocks(tile, gi, gj);
        quad.height[1]  = HeightAtBlocks(tile, gi + step, gj);
        quad.height[2]  = HeightAtBlocks(tile, gi, gj + step);
        quad.height[3]  = HeightAtBlocks(tile, gi + step, gj + step);
        if (quadFilter->SkipQuad(quad)) {
            continue;  // 命中 ⇒ 该格的 6 个索引整段丢弃
        }

        const std::size_t base = quadIndex * 6U;
        for (std::size_t k = 0; k < 6U; ++k) {
            filtered.push_back(meshInOut.indices[base + k]);
        }
    }

    meshInOut.indices = std::move(filtered);
}

}  // namespace vx
