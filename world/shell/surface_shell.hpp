#pragma once

#include "dig/volume_mesher.hpp"
#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"

#include <cstdint>

namespace vx {

/// 河道下切场（W6；只以**指针**持有 ⇒ 此处仅前置声明，避免 `world/shell` 依赖 `world/water` 的实现）。
class RiverCarveField;

/// **地表壳的水平范围**（世界列坐标，半开区间 `[minColumnX, maxColumnX) × [minColumnZ, maxColumnZ)`）。
///
/// 为什么是有界区域：全图铺开需要流式调度（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)，W7）；
/// W4 先在**近场有界区域**落地"表示 + 网格 + 渲染 + 碰撞"，语义与全图一致。
struct SurfaceShellRegion {
    int minColumnX = 0;
    int maxColumnX = 0;
    int minColumnZ = 0;
    int maxColumnZ = 0;

    [[nodiscard]] bool ContainsColumn(int x, int z) const noexcept {
        return x >= minColumnX && x < maxColumnX && z >= minColumnZ && z < maxColumnZ;
    }
};

/// 地表壳参数（[ADR 0023](../../docs/adr/0023-world-representation-v2-hybrid-shell.md) 层②）。
struct SurfaceShellParams {
    /// 壳的竖向半厚（格）：体积只在 `[宏地表高度 − 半厚, 宏地表高度 + 半厚]` 内有意义。
    /// 用途 = 决定每列要建**哪些 Y 块**（几何本身由等值面给出，不受它影响）。
    float bandHalfThicknessBlocks = 24.0F;

    /// **边界淡出带宽**（格）：距区域边界小于该值时，悬垂幅度线性淡出到 0。
    /// 目的 = 区域边界处壳面**退化为宏高度场** ⇒ 与周围地表**无可见缝隙**（层间过渡）。
    float edgeFadeBlocks = 32.0F;
};

/// 某列块（`kVolumeBlockSize × kVolumeBlockSize` 列）需要建立的 **Y 块范围**（含两端）。
struct ShellBlockSpan {
    int minBlockY = 0;
    int maxBlockY = 0;

    [[nodiscard]] bool Empty() const noexcept { return minBlockY > maxBlockY; }
};

/// 该列块是否**与区域相交**（用于裁剪块集合；纯函数）。
[[nodiscard]] bool ShellBlockTouchesRegion(const SurfaceShellRegion& region, int blockX, int blockZ) noexcept;

/// 由宏地表高度推导该列块的 Y 块范围（纯函数）：采样块覆盖的 `33×33` 列取高度 min/max，再加半厚。
[[nodiscard]] ShellBlockSpan ComputeShellBlockSpanY(const TerrainNoiseGenerator& noise,
                                                    const SurfaceShellParams& params, int blockX,
                                                    int blockZ) noexcept;

/// **粗采样 Y 块范围的默认口径**（W7-S3b①）：每 `kShellSpanSampleStepBlocks` 列采一次高度，
/// 再对高度加 `kShellSpanMarginHeightBlocks` 的**保守余量**（单位：世界格，不是 Y 块）。
///
/// 为什么：`ComputeShellBlockSpanY` 逐列采 `34×34` 次高度 ⇒ 实测 ≈ `0.9 ms/列块`；壳流式按窗口规划
/// （上千列块）会在主线程造成秒级冻结。粗采样把单列块降到 ≈ `1/16`。
/// 余量取**高度**而非"Y 块"：`16 格` 余量把 Y 范围最多外扩 1 个块（`16/32 = 0.5`），
/// 而按"Y 块"余量（如 ±2 块）会把平坦区的块数翻数倍 —— 后者直接抬高枚举与建块量。
inline constexpr int kShellSpanSampleStepBlocks   = 4;
inline constexpr int kShellSpanMarginHeightBlocks = 16;

/// **粗采样 + 安全余量**的 Y 块范围（纯函数；W7-S3b① 性能口径）。
///
/// 语义（**保守**）：返回的范围 **⊇** `ComputeShellBlockSpanY` 的范围（`marginHeightBlocks` 覆盖采样点之间的
/// 高度变化）⇒ 只会**多**枚举若干（多半为空的）块，**不会漏**含表面的块。空块由 worker 网格化后返回空
/// `MeshData`（不产生 GPU 上传与碰撞体）⇒ 代价只落在 worker 侧。
[[nodiscard]] ShellBlockSpan ComputeShellBlockSpanYCoarse(const TerrainNoiseGenerator& noise,
                                                          const SurfaceShellParams& params, int blockX, int blockZ,
                                                          int sampleStepBlocks, int marginHeightBlocks) noexcept;

/// 地表壳的密度采样器：把"距地表的**有符号距离** + 悬垂 3D 噪声 + **洞穴隧道雕刻量**"量化成
/// `volume_mesher` 的密度口径。
///
/// 约定与既有体积块一致（[ADR 0004](../../docs/adr/0004-hybrid-layered-world-representation.md) 硬约束 3）：
/// `Sample(i,j,k)` 的索引**可越界** `[-1, kVolumeBlockSize]`，越界采样必须与相邻块给出**同一份密度**
/// ⇒ 块间共享边界采样、顶点逐位相同（无裂缝）。
///
/// 密度语义（[ADR 0007](../../docs/adr/0007-volume-meshing-algorithm.md)）：`< 0` 实心、`> 0` 空、`= 0` 表面。
/// 洞穴网络（W5）由 `TerrainCaveParams` 驱动；`caves.enabled == false` 时雕刻量恒 0 ⇒ 与 W4 逐位一致。
/// 河道下切（W6）由 `RiverCarveField` 驱动；传 `nullptr` 时不下切 ⇒ 与 W5 逐位一致。
class SurfaceShellSampler final : public IVolumeSampler {
public:
    /// 前置条件：四个引用对象的**生命周期覆盖本对象**；`block` 为待网格化的体积块坐标。
    /// `river`（W6，可空）= 河道下切场；`nullptr` 时**不下切**（与引入河道之前逐位一致）。
    SurfaceShellSampler(const TerrainNoiseGenerator& noise, const TerrainGenerationParams& generation,
                        const SurfaceShellParams& params, const SurfaceShellRegion& region, BlockCoord block,
                        const RiverCarveField* river = nullptr) noexcept;

    [[nodiscard]] float Sample(int i, int j, int k) const override;

private:
    const TerrainNoiseGenerator&   m_noise;
    const TerrainGenerationParams& m_generation;
    const SurfaceShellParams&      m_params;
    const SurfaceShellRegion&      m_region;
    BlockCoord                     m_block;
    const RiverCarveField*         m_river = nullptr;  ///< W6：河道下切场（可空）
};

/// 把一块地表壳网格化（Surface Nets，复用 `BuildVolumeMesh`）；**空块返回空 `MeshData`**。
/// 纯函数（只依赖入参）⇒ 渲染与碰撞**共用同一份输出**（"谁画谁挡"同源）。
/// `river`（W6，可空）= 河道下切场，需与 `params` 同源。
[[nodiscard]] MeshData BuildShellBlockMesh(const TerrainNoiseGenerator& noise,
                                           const TerrainGenerationParams& generation,
                                           const SurfaceShellParams& params, const SurfaceShellRegion& region,
                                           BlockCoord block, const RiverCarveField* river = nullptr);

}  // namespace vx
