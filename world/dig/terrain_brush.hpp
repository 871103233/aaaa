#pragma once

#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <vector>

namespace vx {

class TerrainWorld;

/// 一次笔刷操作的姿态：水平圆心 + 半径（格）。
///
/// 语义：笔刷是**竖直轴与地表列对齐的球体**，其水平投影为一个圆盘；落在圆盘内的列被整体
/// 抬升 / 下沉一个固定增量。采用"半径内恒定增量"是为了让受影响列集合可精确预测、可测试；
/// 球体的起伏由地表本身的高低体现。
struct BrushPose {
    float centerX = 0.0F;  ///< 圆心世界 X（格）
    float centerZ = 0.0F;  ///< 圆心世界 Z（格）
    float radius  = 0.0F;  ///< 半径（格）；`<= 0` 时不做任何事
};

/// 一次笔刷操作的结果。
struct BrushResult {
    /// 高度**实际发生变化**的列数（因钳制到垂直范围而未变化的列不计入）。
    std::size_t changedColumns = 0;

    /// 受影响（需要重网格）的 tile 集合，已去重并按坐标升序排列。
    std::vector<TileCoord> dirtyTiles;
};

/// 在世界的地表上执行一次笔刷挖掘 / 堆建。
///
/// - 只改动圆盘内的列，且**只标记受影响 tile** 为脏（red line：禁止整世界重网格）；
/// - 高度钳制到世界垂直范围 `0 ~ 512` 格（ADR 0008）；
/// - 圆盘触及 tile 共享边界列时，相邻 tile 的边界几何同样改变，因此**一并**标记为脏；
/// - 完全落在单个 tile 内部的笔刷**不会**弄脏邻 tile。
///
/// 前置条件：目标列的 tile 已加载（未加载的列按"不存在"跳过）。
/// `deltaHeightUnits` 为定点增量：负 = 挖，正 = 堆。
[[nodiscard]] BrushResult ApplyTerrainBrush(TerrainWorld& world, const BrushPose& brush, int deltaHeightUnits);

}  // namespace vx
