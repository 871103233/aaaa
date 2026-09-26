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

/// 一个地表四边形的四角（层间交接判定用）：**世界列坐标（整数）**与四角**高度（格）**。
/// 角序固定为 `00, 10, 01, 11`（与 `BuildTerrainMesh` 内部的 A/B/C/D 一致，但**不承诺**具体顺序）。
struct TerrainQuad {
    int   columnX[4] = { 0, 0, 0, 0 };
    int   columnZ[4] = { 0, 0, 0, 0 };
    float height[4]  = { 0.0F, 0.0F, 0.0F, 0.0F };
};

/// 地表四边形过滤器：返回 true 表示**该四边形交给可挖体积网格渲染、地表网格跳过它**（层间交接）。
///
/// 为什么做成接口而不是让地表网格直接认识"可挖区域"：依赖方向保持 `dig → terrain`（ADR 0004 层
/// ①/② 都由世界层持有，但地表网格化不应反向依赖挖掘模块）。默认（不设置过滤器）行为与旧版**逐位一致**。
class ITerrainQuadFilter {
public:
    virtual ~ITerrainQuadFilter() = default;

    ITerrainQuadFilter(const ITerrainQuadFilter&) = delete;
    ITerrainQuadFilter& operator=(const ITerrainQuadFilter&) = delete;
    /// 允许**移动**（不可拷贝）：实现方（`world/dig/DigRegionTable`）需要按值返回。
    ITerrainQuadFilter(ITerrainQuadFilter&&) = default;
    ITerrainQuadFilter& operator=(ITerrainQuadFilter&&) = default;

    [[nodiscard]] virtual bool SkipQuad(const TerrainQuad& quad) const = 0;

protected:
    ITerrainQuadFilter() = default;
};

/// 把一个地表 tile 网格化为平滑曲面（**禁止**方块外观，方案 §4.1）。
///
/// - 顶点位置为块内定点坐标转 `float`；世界定位由 `TerrainTileMesh::coord` 承担；
/// - **法线由高度场梯度计算**并写入顶点属性（禁止面法线近似）；
/// - **不再**写入材质权重：ADR 0009 起权重由片元着色器**逐像素**按世界高度与坡度计算，
///   因此过渡带宽只受几何曲率限制，不受 1 格顶点间距摊开（顶点格式见 `MeshVertex`）；
/// - 三角形绕序在 +Y 视角下为逆时针（`(A,C,B)` 与 `(B,C,D)`，A/B/C/D 为每格四角）；
/// - `quadFilter` 非空时：**四角全部落在可挖区域内**的四边形被跳过（交给体积网格，见 `ITerrainQuadFilter`）。
///
/// 前置条件：`tile` 已由 `GenerateTerrainTile` 填充完毕。
[[nodiscard]] TerrainTileMesh BuildTerrainMesh(const TerrainTile& tile,
                                               const ITerrainQuadFilter* quadFilter = nullptr);

}  // namespace vx
