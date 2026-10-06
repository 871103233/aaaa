#pragma once

#include "terrain/terrain_tile.hpp"

namespace vx {

/// 地表 tile 的**数据来源**抽象（V4 / [ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
///
/// **为什么需要它**：`TerrainWorld` 原本只会"按种子 + 噪声 + 预设编辑**生成**"（`GenerateTerrainTileData`）；
/// 而 `source = premade` 的世界必须改成"**读盘**"（离线烘焙产物）⇒ 把"一个 tile 的高度从哪来"从生成逻辑里解耦，
/// 使**同一条流式 / 网格化 / 碰撞路径**既能跑程序化世界、也能跑预制世界（零分叉）。
///
/// 三条硬约束：
///   1. **同口径**：填充的是**全分辨率 `heights`（含共享边界层）**，与 `GenerateTerrainTile` 完全一致
///      ⇒ 下游（网格化 / 交接过滤 / 碰撞 / 世界指纹）**不需要知道数据从哪来**；
///   2. **并发契约**：worker（`TerrainTileBuildPipeline`）会**并发**调用 ⇒ 实现必须**线程安全**
///      （要么无共享可变状态，要么内部自带同步）；
///   3. **不静默**：来源里没有该 tile（如预制文件缺块）⇒ 返回 `false`，由调用方决定"回退程序化并告警"，**不得**假装成功。
class ITerrainTileSource {
public:
    virtual ~ITerrainTileSource() = default;

    ITerrainTileSource(const ITerrainTileSource&) = delete;
    ITerrainTileSource& operator=(const ITerrainTileSource&) = delete;

    /// 用来源数据填充 `tile`。
    ///
    /// 前置条件：`tile.coord` **已设置**（实现按坐标取数据）；`tile.heights` 可以是任意初值（会被整体覆盖）。
    /// 后置：返回 `true` ⇒ `tile` 视为**可直接使用** —— `heights` 已按**与 `GenerateTerrainTile` 同口径**填满
    ///       （65×65，含共享边界层），**且** `maxSurfaceBlocks` 缓存已刷新（`RefreshTileMaxSurfaceBlocks`）；
    ///       返回 `false` ⇒ `tile` 内容未定义，调用方须回退（例如改用程序化生成）。
    ///
    /// 为什么把"刷新缓存"归在**实现侧**：调用方（`TerrainWorld` / `TerrainTileBuildPipeline`）不应关心
    /// "数据从哪来"，只应得到一个**完整可用的 tile**；否则每个新来源都要记得补一次刷新，迟早在某条路径漏掉。
    [[nodiscard]] virtual bool FillTileHeights(TerrainTile& tile) const = 0;

protected:
    ITerrainTileSource() = default;
};

}  // namespace vx
