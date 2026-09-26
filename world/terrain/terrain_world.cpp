#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace vx {
namespace {

/// 遮挡查询的采样步长（格）：越小越精确，越大越省。
constexpr float kObstructionStepBlocks = 0.25F;

}  // namespace

TerrainWorld::TerrainWorld(std::uint64_t worldSeed, TerrainMaterialTable materials)
    : m_seed(worldSeed), m_materials(std::move(materials)), m_noise(worldSeed) {}

void TerrainWorld::SetMapPreset(const MapPreset& preset) {
    m_mapEdits = preset.edits;
}

void TerrainWorld::GenerateTile(int tileX, int tileZ) {
    TerrainTile& tile = m_tiles[TileCoord { tileX, tileZ }];
    tile.coord        = TileCoord { tileX, tileZ };
    GenerateTerrainTile(tile, m_noise);
    // 预设地图：噪声先行，编辑按文件顺序覆盖其上（T11；纯函数，边界列逐位一致）。
    ApplyMapEditsToTile(m_mapEdits, tile);
}

void TerrainWorld::MeshTile(int tileX, int tileZ) {
    const TerrainTile* tile = FindTile(tileX, tileZ);
    if (tile == nullptr) {
        return;
    }
    m_meshes[TileCoord { tileX, tileZ }] = BuildTerrainMesh(*tile);
}

void TerrainWorld::LoadTile(int tileX, int tileZ) {
    GenerateTile(tileX, tileZ);
    MeshTile(tileX, tileZ);
}

bool TerrainWorld::HasTile(int tileX, int tileZ) const noexcept {
    return m_tiles.find(TileCoord { tileX, tileZ }) != m_tiles.end();
}

const TerrainTile* TerrainWorld::FindTile(int tileX, int tileZ) const noexcept {
    const auto found = m_tiles.find(TileCoord { tileX, tileZ });
    return (found != m_tiles.end()) ? &found->second : nullptr;
}

const TerrainTileMesh* TerrainWorld::FindMesh(int tileX, int tileZ) const noexcept {
    const auto found = m_meshes.find(TileCoord { tileX, tileZ });
    return (found != m_meshes.end()) ? &found->second : nullptr;
}

bool TerrainWorld::ReadColumnHeight(int worldX, int worldZ, Height& outHeight) const noexcept {
    for (const auto& entry : m_tiles) {
        const TerrainTile& tile = entry.second;
        if (!TileContainsColumn(tile, worldX, worldZ)) {
            continue;
        }
        outHeight = tile.At(worldX - TileOriginColumn(tile.coord.x), worldZ - TileOriginColumn(tile.coord.z));
        return true;
    }
    return false;
}

void TerrainWorld::WriteColumnHeight(int worldX, int worldZ, Height height, std::vector<TileCoord>& dirtyOut) {
    const int clamped = std::clamp(static_cast<int>(height), kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
    const Height value = static_cast<Height>(clamped);

    for (auto& entry : m_tiles) {
        TerrainTile& tile = entry.second;
        if (!TileContainsColumn(tile, worldX, worldZ)) {
            continue;
        }
        tile.SetAt(worldX - TileOriginColumn(tile.coord.x), worldZ - TileOriginColumn(tile.coord.z), value);
        dirtyOut.push_back(tile.coord);
    }
}

std::size_t TerrainWorld::RemeshDirtyTiles(const std::vector<TileCoord>& dirty) {
    std::set<TileCoord> uniqueTiles;
    std::size_t         remeshed = 0;
    for (const TileCoord& coord : dirty) {
        if (!uniqueTiles.insert(coord).second) {
            continue;
        }
        if (FindTile(coord.x, coord.z) == nullptr) {
            continue;
        }
        MeshTile(coord.x, coord.z);
        ++remeshed;
    }
    return remeshed;
}

float TerrainWorld::MaxSurfaceHeightBlocks() const noexcept {
    float maximum = 0.0F;
    for (const auto& entry : m_tiles) {
        const TerrainTile& tile = entry.second;
        for (const Height height : tile.heights) {
            const float blocks = HeightToBlocks(height);
            maximum            = (blocks > maximum) ? blocks : maximum;
        }
    }
    return maximum;
}

bool TerrainWorld::QueryHeight(float worldX, float worldZ, float& outHeight) const {
    const int columnX = static_cast<int>(std::floor(worldX));
    const int columnZ = static_cast<int>(std::floor(worldZ));

    Height height = 0;
    if (!ReadColumnHeight(columnX, columnZ, height)) {
        return false;
    }
    outHeight = HeightToBlocks(height);
    return true;
}

bool TerrainWorld::QueryObstruction(const glm::vec3& from, const glm::vec3& to, float& outSafeT) const {
    outSafeT = 1.0F;

    const glm::vec3 delta  = to - from;
    const float     length = glm::length(delta);

    if (length <= 0.0F) {
        float surface = 0.0F;
        if (QueryHeight(from.x, from.z, surface) && from.y < surface) {
            outSafeT = 0.0F;
            return true;
        }
        return false;
    }

    const int   sampleCount = std::max(1, static_cast<int>(std::ceil(length / kObstructionStepBlocks)));
    float       lastSafeT   = 0.0F;
    for (int step = 0; step <= sampleCount; ++step) {
        const float t     = static_cast<float>(step) / static_cast<float>(sampleCount);
        const glm::vec3 point = from + delta * t;

        float surface = 0.0F;
        if (!QueryHeight(point.x, point.z, surface)) {
            // 无地形数据：不做阻挡判定（流式层保证查询范围内的 tile 已就绪）。
            lastSafeT = t;
            continue;
        }
        if (point.y < surface) {
            outSafeT = lastSafeT;
            return true;
        }
        lastSafeT = t;
    }

    return false;
}

}  // namespace vx
