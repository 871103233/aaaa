#pragma once

#include "physics/physics_world.hpp"
#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace vx {

struct TerrainTile;
class TerrainWorld;

/// 把地表 tile 的高度场换算为物理层的**通用**高度场采样（行主序、绝对高度、单位格）。
///
/// 输出会 resize 为 `kTerrainTileVertexCount²`，且 `outSamples[j * N + i] == HeightToBlocks(tile.At(i, j))`。
/// 单独抽出为自由函数，是为了在**不接触 Jolt / 物理系统**的前提下可测（见 tests/physics_test.cpp）。
void BuildHeightFieldSamples(const TerrainTile& tile, std::vector<float>& outSamples);

/// 地表 tile → 物理高度场碰撞体的**提供者**。
///
/// 这是方案 §5.1「每个地表 tile 一个 `HeightFieldShape`」的**地形专有胶水**，
/// 由 world 层持有；engine 层只认识通用的高度场描述（`PhysicsWorld::HeightFieldDesc`），
/// 不认识 `TerrainTile`。依赖方向为 `world → engine`（SKILL §2）。
///
/// 线程约定：只在逻辑线程（主线程）调用，且不得与物理推进并发。
class TerrainCollision final {
public:
    /// 前置条件：`physics` 的生命周期覆盖本对象。
    explicit TerrainCollision(PhysicsWorld& physics);
    ~TerrainCollision();

    TerrainCollision(const TerrainCollision&) = delete;
    TerrainCollision& operator=(const TerrainCollision&) = delete;
    TerrainCollision(TerrainCollision&&) = delete;
    TerrainCollision& operator=(TerrainCollision&&) = delete;

    /// 创建或重建一个已生成 tile 的碰撞体（高度变化后调用即完成重建）。
    /// 返回 false 表示该 tile 不存在或物理层创建失败。
    bool SyncTile(const TerrainWorld& world, int tileX, int tileZ);

    /// 对一组 tile 坐标去重后逐个 `SyncTile`，返回成功数（笔刷挖 / 堆后调用）。
    [[nodiscard]] std::size_t SyncTiles(const TerrainWorld& world, const std::vector<TileCoord>& tiles);

    /// 移除一个 tile 的碰撞体；不存在为无操作。
    void RemoveTile(int tileX, int tileZ) noexcept;

    [[nodiscard]] bool HasTile(int tileX, int tileZ) const noexcept;

    /// 已建立碰撞体的 tile 数（调试面板用）。
    [[nodiscard]] std::size_t TileBodyCount() const noexcept { return m_bodies.size(); }

private:
    PhysicsWorld& m_physics;
    std::map<TileCoord, PhysicsWorld::BodyHandle> m_bodies;
    std::vector<float> m_scratch;  ///< 复用的采样缓冲（65² float），避免每次重建堆分配
};

}  // namespace vx
