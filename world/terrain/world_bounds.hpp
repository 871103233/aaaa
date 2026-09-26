#pragma once

#include <glm/vec3.hpp>

#include <array>

namespace vx {

/// 世界边界在高度方向的额外余量（格）。
///
/// 边界盒要**完全包住**地表（含最高可能的顶点与最低的坑底），因此竖直范围取 ADR 0008
/// 记载的 `0 ~ 512` 格再向外扩这一段余量：`[-余量, 512 + 余量]`。
inline constexpr double kWorldBoundsVerticalHeadroom = 8.0;

/// 世界边界盒（轴对齐，世界空间，单位：格）。
///
/// 由**已加载地图的范围**推导（见 `ComputeWorldBounds`），不得硬编码：换一张预设地图
/// （不同 `tile_radius`）或将来改由程序化决定大小时，边界随之自动跟随。
struct WorldBounds {
    glm::dvec3 min { 0.0 };  ///< 最小角（含）
    glm::dvec3 max { 0.0 };  ///< 最大角（含）
};

/// 由 tile 半径推导世界边界盒（纯函数）。
///
/// 口径：tile 坐标覆盖 `[-tileRadius, tileRadius]`（两轴各自），而 tile `t` 覆盖世界列
/// `[t * 64, (t + 1) * 64]`（含共享边界列），故两轴的世界列范围为
/// `[-tileRadius * 64, (tileRadius + 1) * 64]`；竖直方向取 ADR 0008 的 `0 ~ 512` 格外扩
/// `kWorldBoundsVerticalHeadroom`。用于"走到边缘被挡住"与"掉出世界救援"两处。
///
/// 前置条件：`tileRadiusX >= 0`、`tileRadiusZ >= 0`（`0` 即单 tile 的退化情形，两轴独立）。
[[nodiscard]] WorldBounds ComputeWorldBounds(int tileRadiusX, int tileRadiusZ) noexcept;

/// 一堵边界墙的轴对齐盒描述（世界空间，单位：格）。
///
/// 只描述**几何**（中心 + 半长），由 game 层转成引擎的通用盒体碰撞体；本类型不依赖物理层，
/// 因而可被纯函数单测覆盖。
struct BoundaryWall {
    glm::dvec3 center { 0.0 };       ///< 盒中心
    glm::dvec3 halfExtents { 0.0 };  ///< 各轴半长（均 > 0）
};

/// 边界墙厚度（格）：取 2 格，足以挡住 20 格/秒的冲刺而不产生隧穿。
inline constexpr double kBoundaryWallThickness = 2.0;

/// 由边界盒推导四周共 4 堵墙（纯函数）。顺序：`-X`、`+X`、`-Z`、`+Z`。
///
/// 墙贴在边界盒**外侧**：内表面与边界盒表面**齐平**（既不侵入可玩区域，也不留缝让角色挤出去），
/// 四角处相互交叠封口；竖直方向与边界盒等高（下探到最低地形之下、上探到最高地形之上）。
///
/// 前置条件：`bounds.min <= bounds.max`、`thickness > 0`。`thickness <= 0` 时按
/// `kBoundaryWallThickness` 处理。
[[nodiscard]] std::array<BoundaryWall, 4> ComputeBoundaryWalls(const WorldBounds& bounds,
                                                               double thickness) noexcept;

}  // namespace vx
