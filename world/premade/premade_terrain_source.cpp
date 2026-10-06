#include "premade/premade_terrain_source.hpp"

#include "core/log.hpp"
#include "premade/premade_bake.hpp"  // DeserializeMacroHeightTile
#include "terrain/terrain_world.hpp"  // RefreshTileMaxSurfaceBlocks（契约：返回 true 即"可直接使用"）

#include <utility>

namespace vx {

PremadeTerrainTileSource::PremadeTerrainTileSource(PremadeMapReader reader) noexcept
    : m_reader(std::move(reader)) {}

bool PremadeTerrainTileSource::FillTileHeights(TerrainTile& tile) const {
    const PremadeChunkKey key { PremadeChunkKind::MacroHeightTile, tile.coord.x, 0, tile.coord.z };
    if (!m_reader.HasChunk(key)) {
        m_missingChunks.fetch_add(1U, std::memory_order_relaxed);
        if (!m_warnedMissing.exchange(true, std::memory_order_relaxed)) {
            VX_LOG_WARN("预制地图缺少 tile (%d, %d) 的宏高度块 ⇒ 该块**回退程序化生成**"
                        "（后续同类缺失只计数、不再逐条告警）",
                        tile.coord.x, tile.coord.z);
        }
        return false;
    }
    // `ReadChunk` 每次自开 `std::ifstream` ⇒ 并发安全；块损坏 / 解压失败 / 字节数不符 ⇒ **抛**（不静默）。
    DeserializeMacroHeightTile(m_reader.ReadChunk(key), tile);
    // 契约（见 `ITerrainTileSource`）：返回 true 即"可直接使用"⇒ 这里把 `maxSurfaceBlocks` 缓存一并刷新。
    RefreshTileMaxSurfaceBlocks(tile);
    return true;
}

}  // namespace vx
