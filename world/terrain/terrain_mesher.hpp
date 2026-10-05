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

    /// 本网格所在的 **LOD 环**（W7-S3b；0 = 全细节，默认 = 与从前一致）。
    int lodLevel = 0;

    /// 本网格是否**没有任何可见面**（W7-S3b / ADR 0011 接管判据）。
    ///
    /// 为什么单列一个标记而不是继续靠 `mesh.indices.empty()`：GPU 上传后本类会**释放 CPU 侧网格**
    /// （`TerrainWorld::ReleaseTileMeshCpu`，把顶点 / 索引 `clear()` + `shrink_to_fit()`），
    /// 释放后 `indices` **恒为空**，无法再表达"该 tile 是否被体积 / 地表壳接管"。
    /// 因此凡"靠空网格判断接管"的地方（碰撞体交还 / 交接）一律改读本标记；重网格后必须重写它。
    bool meshEmpty = false;

    /// 顶点每边的数量（= `TerrainLodVertexSide(lodLevel)`；LOD0 = `kTerrainTileVertexCount`）。
    int verticesPerSide = kTerrainTileVertexCount;

    /// 本地顶点 `(i, j)` 在 `mesh.vertices` 中的下标。
    ///
    /// `side` 为该 LOD 环的顶点每边数量；**默认实参** `kTerrainTileVertexCount` 使既有调用与单测不变
    /// （LOD0 与从前逐位一致）。非 LOD0 时必须显式传 `verticesPerSide`。
    [[nodiscard]] static constexpr std::size_t VertexIndex(int i, int j,
                                                          int side = kTerrainTileVertexCount) noexcept {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(side) +
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
/// **LOD**（W7-S3b，[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)「二、LOD 分环」）：
/// `lodLevel` 决定列步长（`TerrainLodStep`：1 / 2 / 4）。顶点局部列为 `gi = i * step`、`gj = j * step`
/// （`i, j ∈ [0, side)`），`position = (gi, 高度, gj)`；**法线仍用 ±1 相邻列算梯度**（`ComputeNormal`），
/// 故不同 LOD 在相同世界列上得到**相同法线** ⇒ 跨环不出现着色接缝。每个顶点额外携带 `MeshVertex::morph`
/// = 该顶点在**父级网格**对应列（`floor(列 / TerrainLodSnapStep) × snapStep`）上的采样高度（格），
/// 供顶点着色器做 CDLOD 顶点过渡消接缝。
/// **`lodLevel == 0` 时除新增的 `morph` 字段外，顶点 / 索引与未传 `lodLevel` 的旧行为逐位一致。**
///
/// 前置条件：`tile` 已由 `GenerateTerrainTile` 填充完毕；`lodLevel ∈ [0, kTerrainLodLevelCount)`。
[[nodiscard]] TerrainTileMesh BuildTerrainMesh(const TerrainTile& tile,
                                               const ITerrainQuadFilter* quadFilter = nullptr,
                                               int lodLevel = 0);

/// 把**未过滤**的全量索引按 `quadFilter` 过滤（W7-S3b / ADR 0011 层间交接）。
///
/// 前置条件：`meshInOut.indices` 是 `BuildTerrainMesh` 产出的**未过滤**索引列表 —— 按
/// 「`(j, i)` 升序、每格 6 个索引（`a, c, b, b, c, d`）」排列，长度 = `(side − 1)² × 6`。
///
/// 语义：按**同一顺序**遍历格子，用 `tile` 的四角世界列 / 高度构造 `TerrainQuad` 调
/// `quadFilter->SkipQuad(quad)`；命中则跳过该格的 6 个索引，否则把它从**未过滤列表的第 `quadIndex × 6` 位**
/// 整段拷入结果。`quadFilter == nullptr` ⇒ **无操作**（结果与未过滤列表逐位相同）。
///
/// 为什么单独抽出来：worker 侧不能做过滤（`ITerrainQuadFilter` 依赖**当前常驻集合**这一可变状态，
/// 放进 worker 必须快照 ⇒ 结果随快照陈旧而有歧义）。改为**主线程按当前过滤器应用**，
/// 且 `BuildTerrainMesh` 的同步路径也调用本函数 ⇒ 两条路径**共用同一份实现**，逐位一致（红线 7）。
void ApplyQuadFilterToMesh(const TerrainTile& tile, const ITerrainQuadFilter* quadFilter, int lodLevel,
                           MeshData& meshInOut);

}  // namespace vx
