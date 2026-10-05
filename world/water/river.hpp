#pragma once

#include "dig/volume_mesher.hpp"          // MeshData
#include "generation/terrain_params.hpp"  // TerrainRiverParams

#include <cstdint>
#include <vector>

namespace vx {

class TerrainNoiseGenerator;

/// 河道节点：路径上的一点 + 该断面的**静态水位**、**水面半宽**与**下切深度**。
struct RiverNode {
    float x                 = 0.0F;
    float z                 = 0.0F;
    float waterLevelBlocks  = 0.0F;  ///< 该断面的水面高度（世界格）
    float halfWidthBlocks   = 0.0F;  ///< 该断面的水面半宽（格）
    /// 该断面的**下切深度**（格）：河床 = 当地地表 − 本值。由水位反推 ⇒ 水**必定**在河床之上
    /// （地形回升时河道下切更深，形成峡谷，而不是"水落到河床之下"）。恒 `≥ TerrainRiverParams::channelDepthBlocks`。
    float carveDepthBlocks  = 0.0F;
};

/// 河道（样条折线）。`Empty()` = 无河。
struct RiverPath {
    std::vector<RiverNode> nodes;

    [[nodiscard]] bool Empty() const noexcept { return nodes.size() < 2; }
};

/// **确定性**生成河道（纯函数，红线 7）：从区域内**最高**列出发，沿**最陡下降**方向行进，
/// 方向叠加由 `noise.RiverJitterAt` 给出的确定性抖动量；水位 = 河床底 + 水深，
/// 并沿程**强制单调不升**（水往低处流，[ADR 0027](../../docs/adr/0027-water-representation.md)）。
///
/// 终止：走出区域、节点数达 `params.maxNodes`，或步长非正。
/// `params.enabled == false` ⇒ 返回空路径（⇒ 下切恒 0，与引入本层之前逐位一致）。
[[nodiscard]] RiverPath GenerateRiverPath(const TerrainNoiseGenerator& noise, const TerrainRiverParams& params,
                                          int minColumnX, int minColumnZ, int maxColumnX, int maxColumnZ);

/// 河道**下切场**：区域上 1 格分辨率的"应下切深度"（格），双线性查询。
///
/// 语义：`CarveAt(x, z)` = 该列地表应被**下切**的深度（`0` = 不刻蚀）。由"到河道折线的距离"给出 ——
/// `d ≤ channelHalfWidth` ⇒ 满值 `channelDepth`；`d ≤ channelHalfWidth + bankHalfWidth` ⇒ 线性衰减；
/// 更远 ⇒ 0（⇒ **河岸连续**，不会出现"悬空的水"）。
///
/// 空场（无河 / 未启用）恒返回 0 ⇒ 地表壳输出与引入本场之前**逐位一致**。
/// 纯查询、无分配；构造期一次性建表。
class RiverCarveField {
public:
    RiverCarveField() = default;

    /// 前置条件：`sizeX > 0 && sizeZ > 0`；`path` / `params` 的生命周期只需覆盖构造。
    RiverCarveField(const RiverPath& path, const TerrainRiverParams& params, int minColumnX, int minColumnZ,
                    int sizeX, int sizeZ);

    /// 世界列 `(worldX, worldZ)` 处的下切深度（格，`≥ 0`；空场恒 0）。
    [[nodiscard]] float CarveAt(float worldX, float worldZ) const noexcept;

    [[nodiscard]] bool Empty() const noexcept { return m_values.empty(); }

private:
    int                m_minColumnX = 0;
    int                m_minColumnZ = 0;
    int                m_sizeX      = 0;
    int                m_sizeZ      = 0;
    std::vector<float> m_values;  ///< 行主序 `sizeX × sizeZ`（格）
};

/// 沿河道生成**水面**网格（ribbon）：每个断面横跨 `halfWidth`，顶点 y = 该断面水位。
///
/// 顶点是**区域相对**坐标（网格原点 = `(minColumnX, 0, minColumnZ)`）；法线恒为 +Y（水面近平，
/// 由 `water.frag` 再按 flow 扰动）。纯函数；空路径返回空网格。
[[nodiscard]] MeshData BuildRiverWaterMesh(const RiverPath& path, int minColumnX, int minColumnZ);

}  // namespace vx
