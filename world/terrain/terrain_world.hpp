#pragma once

#include "generation/terrain_noise.hpp"
#include "render/camera.hpp"
#include "terrain/material_blender.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace vx {

/// 地表世界：持有已加载 tile 的高度数据与网格，并实现引擎的 `ITerrainQuery` 契约。
///
/// 职责边界（ADR 0004 层 ①）：
///   - 只管理**地表高度场**；可挖体积（层 ②）与物件 / 建造（层 ③）不在此处；
///   - 生成是纯函数；单 tile 的加载 / 卸载 / 流式调度由后续流式层驱动，本类只提供单 tile 操作；
///   - **不直接调用平台 API**，依赖方向为 world → engine。
///
/// 线程约定：生成 / 笔刷 / 重网格是**单写者**写操作，在逻辑线程；生成完成后 `const` 查询与
/// `BuildTerrainMesh` 可被后台网格化线程调用。写操作不得与查询 / 网格化并发。
class TerrainWorld final : public ITerrainQuery {
public:
    /// 前置条件：`worldSeed` 即全局世界种子；`materials` 已成功加载
    /// （见 `TerrainMaterialTable::LoadFromFile`）。
    TerrainWorld(std::uint64_t worldSeed, TerrainMaterialTable materials);

    TerrainWorld(const TerrainWorld&) = delete;
    TerrainWorld& operator=(const TerrainWorld&) = delete;
    TerrainWorld(TerrainWorld&&) = delete;
    TerrainWorld& operator=(TerrainWorld&&) = delete;

    // ---- 单 tile 生命周期（由流式层调用）----

    /// 生成一个 tile 的高度数据（纯函数）。该 tile 已存在时按生成结果覆盖。
    void GenerateTile(int tileX, int tileZ);

    /// 为已生成的 tile 构建网格并缓存，覆盖该 tile 之前的网格。
    /// 前置条件：该 tile 已 `GenerateTile`；未生成时为无操作。
    void MeshTile(int tileX, int tileZ);

    /// `GenerateTile` + `MeshTile`。
    void LoadTile(int tileX, int tileZ);

    [[nodiscard]] bool HasTile(int tileX, int tileZ) const noexcept;
    [[nodiscard]] const TerrainTile* FindTile(int tileX, int tileZ) const noexcept;
    [[nodiscard]] const TerrainTileMesh* FindMesh(int tileX, int tileZ) const noexcept;

    // ---- 列访问（笔刷 / 存档用）----

    /// 读取世界列 `(worldX, worldZ)` 的当前高度。
    /// 返回 false 表示该列未被任何已加载 tile 持有。
    [[nodiscard]] bool ReadColumnHeight(int worldX, int worldZ, Height& outHeight) const noexcept;

    /// 写入世界列高度：更新**所有**含该列的已加载 tile（含共享边界层），
    /// 并把被改动的 tile 坐标追加到 `dirtyOut`（可能重复，调用方负责去重）。
    /// 传入值会被钳制到世界垂直范围（ADR 0008）。
    void WriteColumnHeight(int worldX, int worldZ, Height height, std::vector<TileCoord>& dirtyOut);

    // ---- 重网格 ----

    /// 只重网格 `dirty` 中列出的 tile（按坐标去重），返回实际重网格的 tile 数。
    /// 未加载的 tile 跳过；**不触碰**其余 tile（red line：禁止整世界重网格）。
    [[nodiscard]] std::size_t RemeshDirtyTiles(const std::vector<TileCoord>& dirty);

    // ---- ITerrainQuery ----

    [[nodiscard]] bool QueryHeight(float worldX, float worldZ, float& outHeight) const override;
    [[nodiscard]] bool QueryObstruction(const glm::vec3& from, const glm::vec3& to, float& outSafeT) const override;

    [[nodiscard]] std::uint64_t Seed() const noexcept { return m_seed; }

private:
    std::uint64_t         m_seed = 0;
    TerrainMaterialTable  m_materials;
    TerrainNoiseGenerator m_noise;
    MaterialBlender       m_blender;

    std::map<TileCoord, TerrainTile>     m_tiles;
    std::map<TileCoord, TerrainTileMesh> m_meshes;
};

}  // namespace vx
