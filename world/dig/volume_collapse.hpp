#pragma once

#include "dig/collapse_table.hpp"
#include "dig/dig_volume.hpp"

#include <cstddef>
#include <vector>

namespace vx {

/// 一次塌落的结果（T29）。
struct CollapseResult {
    std::size_t             movedVoxels       = 0;  ///< 被移动（下落 / 摊开）的实心体素数
    std::size_t             unsupportedVoxels = 0;  ///< 失去支撑的实心体素总数（含因"无处可落"而没动的那些）
    std::size_t             regionSamples     = 0;  ///< 支撑检查邻域的采样数（T30 计时 / 诊断口径）
    std::vector<BlockCoord> dirty;                  ///< 需要重网格的块（未去重；调用方交给 `RemeshDirtyBlocks`）
};

/// 塌落的**种子**（T30）：本次挖除改动过的采样范围。邻域**只**围绕它展开 ——
/// 挖一个 6 格半径的球，绝不该让"整段地下 + 几十格外的山体 / 塔"都进邻域。
struct CollapseSeed {
    /// 被改动采样的世界 AABB（闭区间）；默认值（`Empty()`）表示没有改动。
    VoxelBounds bounds;

    /// 由块范围构造"块对齐的种子"（保守：邻域会略大于真实改动范围）。
    /// 供测试与"只知道块、不知道采样"的调用方使用。
    [[nodiscard]] static CollapseSeed FromBlocks(const BlockCoord& minBlock, const BlockCoord& maxBlock) noexcept {
        return CollapseSeed { VoxelBounds { BlockOriginBlocks(minBlock.x), BlockOriginBlocks(minBlock.y),
                                            BlockOriginBlocks(minBlock.z),
                                            BlockOriginBlocks(maxBlock.x) + kVolumeBlockSize,
                                            BlockOriginBlocks(maxBlock.y) + kVolumeBlockSize,
                                            BlockOriginBlocks(maxBlock.z) + kVolumeBlockSize } };
    }
};

/// 在 `seed` 的邻域内做**一次**"支撑检查 + 塌落"，把改动写回 `volumes`。
/// [ADR 0012](../../docs/adr/0012-collision-takeover-by-volumes.md) 第二节。
///
/// 算法（三步，全部在**世界整数坐标**的体素场上做）：
///   ① **支撑体素** = "载荷能沿实心一路传到地底"的实心体素：
///      先逐列自下而上取**纵向**判据（只有"从邻域底面起连续实心"的那一段算接地；邻域底面视作地面 ——
///      它的下方已被证明全是整块实心）；
///      再把接地沿**同一高度层的实心连通**横向传播 `floor(maxCantileverBlocks)` 步（= 悬挑 / 拱效应）；
///   ② **失去支撑** = 实心但不在①里 —— 它的下方通路已被破坏，且离最近的接地处超出允许跨度；
///   ③ 失去支撑的体素按**连续段**沿本列**下落**到腔底（**质量守恒**：下落多少就堆多少），
///      再按**确定性哈希**把堆顶的体素摊给相邻列 `pileSpreadBlocks` 格，使堆面起伏像碎石。
///
/// 邻域（T30）：**水平** = 被改动采样的世界范围外扩 (悬挑 + 摊开 + 1) 格 —— 悬挑最多跨
/// `max_cantilever_blocks` 步、摊开最远推 `pile_spread_blocks` 列，再远的列不可能被本次挖除影响；
/// **竖直** = 自种子块向下到"最低的非全实心块"、向上到"最高的非全空块" —— 被排除的块要么**整块实心**
/// （内部没有空腔可落，其上实心也不会失去支撑）、要么**整块空**（内部没有实心），都不参与判定。
/// 两条合起来把邻域从"整个体积的块范围"收到"真正相关的十几格"，结论与全范围检查**逐体素相同**。
///
/// **不做连锁**（单次判定、不迭代）：迭代到稳定会让山体里一条 12 格宽的隧道把整座山连锁塌掉，
/// 与真实不符；代价是"本次未判定的部分保持原状"，已登记为后续项（ADR 0012「后果」）。
///
/// 前置条件：`seed.bounds` 若非空，则它必须来自"刚被挖除的采样"（通常是 `CarveSphere` 的 `boundsOut`）。
/// 线程约定：只在逻辑线程（主线程）调用，不得与体积的其它写操作并发。
[[nodiscard]] CollapseResult ApplyCollapse(DigVolumeWorld& volumes, const CollapseSeed& seed,
                                           const CollapseSpec& spec);

}  // namespace vx
