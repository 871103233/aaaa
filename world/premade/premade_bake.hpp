#pragma once

#include "generation/terrain_params.hpp"
#include "premade/premade_map.hpp"
#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace vx {

struct TerrainTile;
struct MapPreset;

/// 一个宏地形高度场 tile 的**原始字节数**：`65 × 65` 个 `int16`（1/16 格，行主序，`i` 变化最快）。
///
/// 与运行时 `TerrainTile::heights` 一一对应（含共享边界层），故**字节即数据**、无需转换精度。
inline constexpr std::size_t kMacroHeightTileBytes =
    static_cast<std::size_t>(kTerrainTileVertexCount) * static_cast<std::size_t>(kTerrainTileVertexCount) * 2U;

/// 把一个地表 tile 的高度序列化为预制块字节（**小端** `int16`，行主序）。
/// 纯函数、确定性（同一 tile ⇒ 同一字节）。
[[nodiscard]] std::vector<std::uint8_t> SerializeMacroHeightTile(const TerrainTile& tile);

/// 把预制块字节反序列化为地表 tile 的 `heights`（只写 `heights`，不动 `coord` / 缓存）。
/// 前置条件：`bytes.size() == kMacroHeightTileBytes`；否则抛 `std::runtime_error`。
void DeserializeMacroHeightTile(const std::vector<std::uint8_t>& bytes, TerrainTile& outTile);

/// **离线烘焙**：把一张地图预设的**宏地形高度场**全部 tile 写成预制地图文件。
///
/// 语义与运行时完全一致（同一 `种子 + 地形参数 + 地图编辑` ⇒ 同一世界，红线 7）：
/// 每个 tile 用 `GenerateTerrainTile`（纯函数）生成后按文件顺序叠加 `preset.edits`（`ApplyMapEditsToTile`）。
///
/// **确定性**：同一 `(preset, params)` 两次调用 ⇒ 输出文件**逐字节相同**。
/// **当前范围（W2-S2）**：只烘焙宏地形高度场；体积壳网格 / 水体在 W4~W6 追加（新增块类型）。
/// 失败（写盘 / 非法预设）抛 `std::runtime_error`。
void BakeMacroHeightTilesIntoPremadeMap(const MapPreset& preset, const TerrainGenerationParams& params,
                                        const std::filesystem::path& path);

}  // namespace vx
