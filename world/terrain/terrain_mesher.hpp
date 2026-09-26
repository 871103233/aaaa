#pragma once

#include "render/mesh_renderer.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"

#include <glm/vec3.hpp>

#include <cstddef>

namespace vx {

/// 一个地表 tile 的网格结果：`MeshData` + 整数 tile 坐标。
///
/// 顶点位置是**块内坐标**（本地 x/z ∈ `[0, 64]` 格，y 为高度格数）；世界定位由 `coord`
/// 以整数承担（red line 6）。渲染前由渲染线程做相机相对偏移。
struct TerrainTileMesh {
    TileCoord coord {};
    MeshData  mesh;

    /// 本地顶点 `(i, j)` 在 `mesh.vertices` 中的下标。
    [[nodiscard]] static constexpr std::size_t VertexIndex(int i, int j) noexcept {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(kTerrainTileVertexCount) +
               static_cast<std::size_t>(i);
    }

    /// 第 `vertexIndex` 个顶点的**世界**位置（`double`，供拼接 / 物理等精确定位；red line 6）。
    [[nodiscard]] glm::dvec3 WorldPosition(std::size_t vertexIndex) const noexcept;
};

/// 把一个地表 tile 网格化为平滑曲面（**禁止**方块外观，方案 §4.1）。
///
/// - 顶点位置为块内定点坐标转 `float`；世界定位由 `TerrainTileMesh::coord` 承担；
/// - **法线由高度场梯度计算**并写入顶点属性（禁止面法线近似）；
/// - **不再**写入材质权重：ADR 0009 起权重由片元着色器**逐像素**按世界高度与坡度计算，
///   因此过渡带宽只受几何曲率限制，不受 1 格顶点间距摊开（顶点格式见 `MeshVertex`）；
/// - 三角形绕序在 +Y 视角下为逆时针（`(A,C,B)` 与 `(B,C,D)`，A/B/C/D 为每格四角）。
///
/// 前置条件：`tile` 已由 `GenerateTerrainTile` 填充完毕。
[[nodiscard]] TerrainTileMesh BuildTerrainMesh(const TerrainTile& tile);

}  // namespace vx
