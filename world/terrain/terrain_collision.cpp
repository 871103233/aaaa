#include "terrain/terrain_collision.hpp"

#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"

#include <set>

namespace vx {

void BuildHeightFieldSamples(const TerrainTile& tile, std::vector<float>& outSamples) {
    constexpr std::size_t kCount = static_cast<std::size_t>(kTerrainTileVertexCount);
    outSamples.resize(kCount * kCount);

    // 行主序：`y * N + x`，与 Jolt `HeightFieldShape` 的采样布局一致。
    for (int j = 0; j < kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < kTerrainTileVertexCount; ++i) {
            outSamples[static_cast<std::size_t>(j) * kCount + static_cast<std::size_t>(i)] =
                HeightToBlocks(tile.At(i, j));
        }
    }
}

TerrainCollision::TerrainCollision(PhysicsWorld& physics) : m_physics(physics) {
    m_scratch.reserve(static_cast<std::size_t>(kTerrainTileVertexCount) *
                      static_cast<std::size_t>(kTerrainTileVertexCount));
}

TerrainCollision::~TerrainCollision() {
    for (const auto& entry : m_bodies) {
        m_physics.RemoveBody(entry.second);
    }
    m_bodies.clear();
}

bool TerrainCollision::SyncTile(const TerrainWorld& world, int tileX, int tileZ) {
    const TerrainTile* tile = world.FindTile(tileX, tileZ);
    if (tile == nullptr) {
        return false;
    }

    BuildHeightFieldSamples(*tile, m_scratch);

    PhysicsWorld::HeightFieldDesc desc;
    desc.sampleCount = static_cast<std::uint32_t>(kTerrainTileVertexCount);
    desc.samples     = m_scratch.data();
    desc.originX     = static_cast<double>(TileOriginColumn(tileX));
    desc.originZ     = static_cast<double>(TileOriginColumn(tileZ));

    const TileCoord coord { tileX, tileZ };
    const auto      found = m_bodies.find(coord);
    if (found != m_bodies.end()) {
        return m_physics.UpdateHeightField(found->second, desc);
    }

    const PhysicsWorld::BodyHandle handle = m_physics.AddHeightField(desc);
    if (handle == 0) {
        return false;
    }
    m_bodies.emplace(coord, handle);
    return true;
}

std::size_t TerrainCollision::SyncTiles(const TerrainWorld& world, const std::vector<TileCoord>& tiles) {
    std::set<TileCoord> unique(tiles.begin(), tiles.end());

    std::size_t synced = 0;
    for (const TileCoord& coord : unique) {
        if (SyncTile(world, coord.x, coord.z)) {
            ++synced;
        }
    }
    return synced;
}

void TerrainCollision::RemoveTile(int tileX, int tileZ) noexcept {
    const auto found = m_bodies.find(TileCoord { tileX, tileZ });
    if (found == m_bodies.end()) {
        return;
    }
    m_physics.RemoveBody(found->second);
    m_bodies.erase(found);
}

bool TerrainCollision::HasTile(int tileX, int tileZ) const noexcept {
    return m_bodies.find(TileCoord { tileX, tileZ }) != m_bodies.end();
}

}  // namespace vx
