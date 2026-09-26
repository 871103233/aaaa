#include "terrain/world_bounds.hpp"

#include "terrain/terrain_types.hpp"

#include <algorithm>

namespace vx {
namespace {

/// 把 ADR 0008 的定点高度常量换算成「格」（地表高度精度为 1/16 格）。
constexpr double kMinHeightBlocks =
    static_cast<double>(kMinTerrainHeightUnits) / static_cast<double>(kHeightUnitsPerBlock);
constexpr double kMaxHeightBlocks =
    static_cast<double>(kMaxTerrainHeightUnits) / static_cast<double>(kHeightUnitsPerBlock);

}  // namespace

WorldBounds ComputeWorldBounds(int tileRadiusX, int tileRadiusZ) noexcept {
    const int radiusX = std::max(tileRadiusX, 0);
    const int radiusZ = std::max(tileRadiusZ, 0);

    WorldBounds bounds;
    bounds.min.x = static_cast<double>(-radiusX * kTerrainTileSize);
    bounds.max.x = static_cast<double>((radiusX + 1) * kTerrainTileSize);
    bounds.min.z = static_cast<double>(-radiusZ * kTerrainTileSize);
    bounds.max.z = static_cast<double>((radiusZ + 1) * kTerrainTileSize);
    bounds.min.y = kMinHeightBlocks - kWorldBoundsVerticalHeadroom;
    bounds.max.y = kMaxHeightBlocks + kWorldBoundsVerticalHeadroom;
    return bounds;
}

std::array<BoundaryWall, 4> ComputeBoundaryWalls(const WorldBounds& bounds, double thickness) noexcept {
    const double wallThickness = (thickness > 0.0) ? thickness : kBoundaryWallThickness;
    const double halfThickness = wallThickness * 0.5;

    const double centerX = (bounds.min.x + bounds.max.x) * 0.5;
    const double centerY = (bounds.min.y + bounds.max.y) * 0.5;
    const double centerZ = (bounds.min.z + bounds.max.z) * 0.5;
    const double halfX   = (bounds.max.x - bounds.min.x) * 0.5;
    const double halfY   = (bounds.max.y - bounds.min.y) * 0.5;
    const double halfZ   = (bounds.max.z - bounds.min.z) * 0.5;

    // 平行于 Z 轴的两堵墙（-X / +X）在 Z 向各外扩一个墙厚，与 -Z / +Z 两堵墙在四角交叠封口。
    // 平行于 X 轴的两堵墙（-Z / +Z）同理在 X 向外扩一个墙厚。
    std::array<BoundaryWall, 4> walls {};
    walls[0].center      = glm::dvec3(bounds.min.x - halfThickness, centerY, centerZ);
    walls[0].halfExtents = glm::dvec3(halfThickness, halfY, halfZ + wallThickness);
    walls[1].center      = glm::dvec3(bounds.max.x + halfThickness, centerY, centerZ);
    walls[1].halfExtents = glm::dvec3(halfThickness, halfY, halfZ + wallThickness);
    walls[2].center      = glm::dvec3(centerX, centerY, bounds.min.z - halfThickness);
    walls[2].halfExtents = glm::dvec3(halfX + wallThickness, halfY, halfThickness);
    walls[3].center      = glm::dvec3(centerX, centerY, bounds.max.z + halfThickness);
    walls[3].halfExtents = glm::dvec3(halfX + wallThickness, halfY, halfThickness);
    return walls;
}

}  // namespace vx
