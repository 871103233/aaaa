#pragma once

#include "dig/dig_volume.hpp"
#include "terrain/terrain_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// T37：爆炸产生的**延后工作队列**（重网格 / GPU 上传 / 碰撞体重建）。
///
/// 为什么存在（SKILL「不冻结画面」+ `references/performance-and-hitches.md` §1.1）：这三件事在 debug 下
/// 每块约 5~9 ms，一次爆炸波及 6~7 块时会在**渲染帧内**串行跑完 ~100 ms ⇒ 明显卡顿（实测 T30 数据）。
/// 故爆炸的**当帧**只做"挖除 + 塌落"（毫秒级），其余入队，由 `game/main.cpp` 按**每帧预算**推进。
/// 语义已与项目所有者确认：**接受"全部脏块分帧"**（洞口 / 塌落可在 1~N 帧内补齐）。
///
/// 确定性（红线 7 / 11）：入队即**升序排序 + 去重**，故"先做哪个单位"与线程 / 时序无关；
/// 队列排空后世界状态与"当帧一次性做完"**逐位相同**（分帧只改变"何时可见"）。
///
/// 阶段顺序：**先全部"重网格 + 上传"，再全部"重建碰撞体"** —— 避免出现"物理已经通了、画面还没洞"
/// 的中间态（玩家会走进看不见的洞）。
struct PendingDestruction {
    enum class UnitKind : std::uint8_t {
        VolumeRemesh,     ///< 可挖体积块：重网格 + GPU 上传
        TileRemesh,       ///< 地表 tile：重网格 + GPU 上传
        VolumeCollision,  ///< 可挖体积块：重建三角网碰撞体
        TileCollision,    ///< 地表 tile：重建高度场碰撞体
    };

    /// 一个待处理单位（体积块与地形块二选一，由 `kind` 决定哪个字段有效）。
    struct Unit {
        UnitKind   kind = UnitKind::VolumeRemesh;
        BlockCoord block {};
        TileCoord  tile {};
    };

    /// 把一批可挖体积块加入队列（同时排入"重网格 + 上传"与"重建碰撞体"两阶段）。
    void MergeVolumeBlocks(const std::vector<BlockCoord>& blocks);

    /// 把一批地表 tile 加入队列（同上）。
    void MergeTiles(const std::vector<TileCoord>& tiles);

    /// **只**排入"重建碰撞体"阶段（T61）：新建的可挖体积块在 `CreateBlock` 里**已经**填好密度并网格化过
    /// （网格也已上传），再排一次重网格纯属重复劳动（一块 ≈ 5~9 ms）；这里只补它缺的那一半 —— 碰撞体。
    void MergeVolumeBlockCollisions(const std::vector<BlockCoord>& blocks);

    /// 取下一个待处理单位；队列已空时返回 false。
    [[nodiscard]] bool TakeNext(Unit& out);

    [[nodiscard]] bool Empty() const noexcept {
        return m_remeshCursor >= m_remesh.size() && m_collisionCursor >= m_collision.size();
    }

    /// 尚未处理的单位数（面板 / 日志用）。
    [[nodiscard]] std::size_t PendingUnits() const noexcept {
        return (m_remesh.size() - m_remeshCursor) + (m_collision.size() - m_collisionCursor);
    }

private:
    /// 单位序：先按阶段内种类，再按坐标 —— 保证"取下一个"的次序完全确定。
    [[nodiscard]] static bool Less(const Unit& left, const Unit& right) noexcept;

    void SortAndDedup(std::vector<Unit>& units);

    /// 队列已排空时释放缓冲并把游标复位，使下一次入队从干净状态开始（避免缓冲无限增长）。
    void ResetIfDrained() noexcept;

    std::vector<Unit> m_remesh;     ///< 阶段 1：重网格 + 上传（升序、无重复）
    std::vector<Unit> m_collision;  ///< 阶段 2：重建碰撞体（升序、无重复）
    std::size_t       m_remeshCursor    = 0;
    std::size_t       m_collisionCursor = 0;
};

inline bool PendingDestruction::Less(const Unit& left, const Unit& right) noexcept {
    if (left.kind != right.kind) {
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    }
    if (!(left.block == right.block)) {
        return left.block < right.block;
    }
    return left.tile < right.tile;
}

inline void PendingDestruction::SortAndDedup(std::vector<Unit>& units) {
    std::sort(units.begin(), units.end(), [](const Unit& left, const Unit& right) { return Less(left, right); });
    // 用比较器判等（`!Less(a,b) && !Less(b,a)`），不依赖坐标类型是否提供 `operator==`。
    units.erase(std::unique(units.begin(), units.end(),
                            [](const Unit& left, const Unit& right) {
                                return !Less(left, right) && !Less(right, left);
                            }),
                units.end());
}

inline void PendingDestruction::ResetIfDrained() noexcept {
    if (!Empty()) {
        return;
    }
    m_remesh.clear();
    m_collision.clear();
    m_remeshCursor    = 0;
    m_collisionCursor = 0;
}

inline void PendingDestruction::MergeVolumeBlocks(const std::vector<BlockCoord>& blocks) {
    ResetIfDrained();
    for (const BlockCoord& block : blocks) {
        m_remesh.push_back(Unit { UnitKind::VolumeRemesh, block, TileCoord {} });
        m_collision.push_back(Unit { UnitKind::VolumeCollision, block, TileCoord {} });
    }
    SortAndDedup(m_remesh);
    SortAndDedup(m_collision);
}

inline void PendingDestruction::MergeTiles(const std::vector<TileCoord>& tiles) {
    ResetIfDrained();
    for (const TileCoord& tile : tiles) {
        m_remesh.push_back(Unit { UnitKind::TileRemesh, BlockCoord {}, tile });
        m_collision.push_back(Unit { UnitKind::TileCollision, BlockCoord {}, tile });
    }
    SortAndDedup(m_remesh);
    SortAndDedup(m_collision);
}

inline void PendingDestruction::MergeVolumeBlockCollisions(const std::vector<BlockCoord>& blocks) {
    ResetIfDrained();
    for (const BlockCoord& block : blocks) {
        m_collision.push_back(Unit { UnitKind::VolumeCollision, block, TileCoord {} });
    }
    SortAndDedup(m_collision);
}

inline bool PendingDestruction::TakeNext(Unit& out) {
    if (m_remeshCursor < m_remesh.size()) {
        out = m_remesh[m_remeshCursor];
        ++m_remeshCursor;
        return true;
    }
    if (m_collisionCursor < m_collision.size()) {
        out = m_collision[m_collisionCursor];
        ++m_collisionCursor;
        return true;
    }
    return false;
}

}  // namespace vx
