#include "terrain/terrain_tile.hpp"

#include <cstdint>

namespace vx {

void GenerateTerrainTile(TerrainTile& tile, const TerrainNoiseGenerator& noise) noexcept {
    const int originX = TileOriginColumn(tile.coord.x);
    const int originZ = TileOriginColumn(tile.coord.z);

    for (int j = 0; j <= kTerrainTileSize; ++j) {
        for (int i = 0; i <= kTerrainTileSize; ++i) {
            tile.SetAt(i, j, noise.HeightUnits(static_cast<std::int64_t>(originX + i),
                                               static_cast<std::int64_t>(originZ + j)));
        }
    }
}

bool TileContainsColumn(const TerrainTile& tile, int worldX, int worldZ) noexcept {
    const int localX = worldX - TileOriginColumn(tile.coord.x);
    const int localZ = worldZ - TileOriginColumn(tile.coord.z);
    return localX >= 0 && localX <= kTerrainTileSize && localZ >= 0 && localZ <= kTerrainTileSize;
}

}  // namespace vx
