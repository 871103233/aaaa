#pragma once

#include "dig/dig_volume.hpp"
#include "physics/physics_world.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace vx {

/// 可挖体积 → 物理层的**三角网静态碰撞体**提供者（[ADR 0012](../../docs/adr/0012-collision-takeover-by-volumes.md)）。
///
/// 这是"碰撞接管"的胶水层：凡是**可见地表已由可挖体积绘制**的地方，地表高度场不再提供碰撞
/// （Jolt 的 `HeightFieldShape` 是整块 tile 一个形状，无法局部开洞），碰撞改由本类按**体积块**
/// 提供 —— 每个有网格的块一个 `MeshShape`，形状直接取 Surface Nets 的顶点与索引。
///
/// 与 `TerrainCollision` 同构（依赖方向 `world → engine`，engine 层不认识"体积块"）：
///   - 挖除 / 塌落之后，调用方对受影响的块再调一次 `SyncBlock` 即完成重建；
///   - 块变空（挖穿 / 塌成空）时**移除**其碰撞体，避免留下隐形障碍。
///
/// 线程约定：只在逻辑线程（主线程）调用，且不得与物理推进并发。
class VolumeCollision final {
public:
    /// 前置条件：`physics` 的生命周期覆盖本对象。
    explicit VolumeCollision(PhysicsWorld& physics);
    ~VolumeCollision();

    VolumeCollision(const VolumeCollision&) = delete;
    VolumeCollision& operator=(const VolumeCollision&) = delete;
    VolumeCollision(VolumeCollision&&) = delete;
    VolumeCollision& operator=(VolumeCollision&&) = delete;

    /// 创建 / 重建一个体积块的碰撞体。该块不存在或网格为空时**移除**其碰撞体。
    /// 返回该块最终**是否拥有**碰撞体。
    bool SyncBlock(const DigVolumeWorld& volumes, const BlockCoord& coord);

    /// 对一组块坐标去重后逐个 `SyncBlock`，返回最终拥有碰撞体的块数。
    [[nodiscard]] std::size_t SyncBlocks(const DigVolumeWorld& volumes, const std::vector<BlockCoord>& blocks);

    /// 移除一个块的碰撞体；不存在为无操作。
    void RemoveBlock(const BlockCoord& coord) noexcept;

    /// 移除全部碰撞体。
    void RemoveAll() noexcept;

    [[nodiscard]] bool HasBlock(const BlockCoord& coord) const noexcept;

    /// 已建立碰撞体的块数（调试面板用）。
    [[nodiscard]] std::size_t BlockBodyCount() const noexcept { return m_bodies.size(); }

private:
    PhysicsWorld&                                  m_physics;
    std::map<BlockCoord, PhysicsWorld::BodyHandle> m_bodies;
    std::vector<float> m_scratch;  ///< 复用的顶点缓冲（块内局部坐标），避免每次重建堆分配
};

}  // namespace vx
