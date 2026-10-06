#pragma once

#include "premade/premade_map.hpp"
#include "terrain/terrain_tile_source.hpp"

#include <atomic>
#include <cstddef>
#include <utility>

namespace vx {

/// 把**预制地图**当作地表数据来源（V4 / [ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
///
/// 语义：
///   - 按 `(tileX, 0, tileZ)` 取 `MacroHeightTile` 块 → **校验字节数** → 写入 `tile.heights`
///     （全分辨率、含共享边界层）⇒ 与 `GenerateTerrainTile` **同口径**
///     （下游的网格化 / 层间交接过滤 / 碰撞 / **世界指纹**无需知道数据从哪来）；
///   - **缺块**（预制文件里没有该 tile）⇒ 返回 `false`（调用方回退程序化生成），并**只告警一次**、累计计数
///     —— 既不静默成败，也不逐条刷屏。
///
/// 线程安全：`FillTileHeights` **可被 worker 并发调用**。实现只读 `PremadeMapReader`
/// （其 `ReadChunk` **每次自开** `std::ifstream`，不共享流）+ 两个 `std::atomic` 观测计数 ⇒ 无共享可变状态。
class PremadeTerrainTileSource final : public ITerrainTileSource {
public:
    /// 前置条件：`reader` 已由 `PremadeMapReader::Open` 成功打开。本类**按值持有**它（`std::move`）。
    explicit PremadeTerrainTileSource(PremadeMapReader reader) noexcept;

    [[nodiscard]] bool FillTileHeights(TerrainTile& tile) const override;

    /// 底层读取器（只读）——供上层做"半径 / 种子与清单是否一致"的校验与日志。
    [[nodiscard]] const PremadeMapReader& Reader() const noexcept { return m_reader; }

    /// **累计缺块次数**（观测；正常应为 0）。
    [[nodiscard]] std::size_t MissingChunkCount() const noexcept {
        return m_missingChunks.load(std::memory_order_relaxed);
    }

private:
    PremadeMapReader                 m_reader;
    mutable std::atomic<std::size_t> m_missingChunks { 0 };
    mutable std::atomic<bool>        m_warnedMissing { false };
};

}  // namespace vx
