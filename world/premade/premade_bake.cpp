// 预制地图的离线烘焙（[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
//
// 烘焙必须与运行时**同源**：同一 `种子 + 地图编辑` ⇒ 同一世界（红线 7）。
// 因此这里直接调用运行时用的 `GenerateTerrainTile` / `ApplyMapEditsToTile`，不另写一套生成逻辑。

#include "premade/premade_bake.hpp"

#include "generation/map_preset.hpp"
#include "generation/terrain_noise.hpp"
#include "terrain/terrain_tile.hpp"

#include <stdexcept>
#include <string>

namespace vx {

std::vector<std::uint8_t> SerializeMacroHeightTile(const TerrainTile& tile) {
    std::vector<std::uint8_t> out(kMacroHeightTileBytes);
    std::size_t               index = 0;
    for (int j = 0; j < kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < kTerrainTileVertexCount; ++i) {
            // 高度是 `int16`；按**小端**原样写（`kHeightUnitsPerBlock` 定点，值域 0~8192，无符号歧义）。
            const std::uint16_t raw = static_cast<std::uint16_t>(tile.At(i, j));
            out[index++]            = static_cast<std::uint8_t>(raw & 0xFFU);
            out[index++]            = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
        }
    }
    return out;
}

void DeserializeMacroHeightTile(const std::vector<std::uint8_t>& bytes, TerrainTile& outTile) {
    if (bytes.size() != kMacroHeightTileBytes) {
        throw std::runtime_error("宏地形 tile 字节数不符：期望 " + std::to_string(kMacroHeightTileBytes) + "，实际 " +
                                 std::to_string(bytes.size()));
    }
    std::size_t index = 0;
    for (int j = 0; j < kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < kTerrainTileVertexCount; ++i) {
            const std::uint16_t raw = static_cast<std::uint16_t>(bytes[index]) |
                                      (static_cast<std::uint16_t>(bytes[index + 1]) << 8U);
            index += 2;
            outTile.SetAt(i, j, static_cast<Height>(raw));
        }
    }
}

void BakeMacroHeightTilesIntoPremadeMap(const MapPreset& preset, const TerrainGenerationParams& params,
                                        const std::filesystem::path& path) {
    if (preset.tileRadiusX < 0 || preset.tileRadiusZ < 0) {
        throw std::runtime_error("预制地图烘焙：tile 半径必须非负（当前 " + std::to_string(preset.tileRadiusX) + ", " +
                                 std::to_string(preset.tileRadiusZ) + "）");
    }

    const TerrainNoiseGenerator noise(preset.seed, params);
    PremadeMapWriter            writer;
    writer.SetWorldInfo(preset.tileRadiusX, preset.tileRadiusZ, preset.seed);

    TerrainTile tile;  // 复用同一块缓冲（无热路径，仅离线工具使用）
    for (int tileZ = -preset.tileRadiusZ; tileZ <= preset.tileRadiusZ; ++tileZ) {
        for (int tileX = -preset.tileRadiusX; tileX <= preset.tileRadiusX; ++tileX) {
            tile.coord = TileCoord { tileX, tileZ };
            GenerateTerrainTile(tile, noise);       // 与运行时同一纯函数
            ApplyMapEditsToTile(preset.edits, tile);  // 噪声先行、编辑覆盖其上（同一顺序）
            writer.SetChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, tileX, 0, tileZ },
                            SerializeMacroHeightTile(tile));
        }
    }

    writer.WriteToFile(path);
}

}  // namespace vx
