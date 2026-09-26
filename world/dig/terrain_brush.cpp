#include "dig/terrain_brush.hpp"

#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <cmath>

namespace vx {

BrushResult ApplyTerrainBrush(TerrainWorld& world, const BrushPose& brush, int deltaHeightUnits) {
    BrushResult result;
    if (brush.radius <= 0.0F) {
        return result;
    }

    const double centerX     = static_cast<double>(brush.centerX);
    const double centerZ     = static_cast<double>(brush.centerZ);
    const double radius      = static_cast<double>(brush.radius);
    const double radiusSq    = radius * radius;

    const int minX = static_cast<int>(std::floor(centerX - radius));
    const int maxX = static_cast<int>(std::ceil(centerX + radius));
    const int minZ = static_cast<int>(std::floor(centerZ - radius));
    const int maxZ = static_cast<int>(std::ceil(centerZ + radius));

    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            const double dx = static_cast<double>(x) - centerX;
            const double dz = static_cast<double>(z) - centerZ;
            if (dx * dx + dz * dz > radiusSq) {
                continue;  // 圆盘外：一律不动
            }

            Height current = 0;
            if (!world.ReadColumnHeight(x, z, current)) {
                continue;  // 该列未加载：按不存在处理
            }

            const int target = std::clamp(static_cast<int>(current) + deltaHeightUnits, kMinTerrainHeightUnits,
                                          kMaxTerrainHeightUnits);
            if (target == static_cast<int>(current)) {
                continue;  // 钳制后无变化：不计数，也不弄脏 tile
            }

            world.WriteColumnHeight(x, z, static_cast<Height>(target), result.dirtyTiles);
            ++result.changedColumns;
        }
    }

    std::sort(result.dirtyTiles.begin(), result.dirtyTiles.end());
    result.dirtyTiles.erase(std::unique(result.dirtyTiles.begin(), result.dirtyTiles.end()), result.dirtyTiles.end());
    return result;
}

}  // namespace vx
