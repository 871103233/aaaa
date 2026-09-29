#pragma once

#include "dig/dig_region.hpp"
#include "dig/dig_volume.hpp"
#include "dig/volume_mesher.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace vx {

/// 块所属的**地表 tile** 索引（块 32 格、tile 64 格 ⇒ 一个块**完全落在**一个 tile 内，故这是精确映射）。
///
/// 向下取整语义（负坐标也正确）：`floor(blockIndex / 2)`。
[[nodiscard]] constexpr int TileOfBlockIndex(int blockIndex) noexcept {
    return (blockIndex >= 0) ? (blockIndex / 2) : -((-blockIndex + 1) / 2);
}

/// **玩家窗口**（[ADR 0020](../../docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二）：
/// 以玩家所在 tile 为中心、半径 `radiusTiles` 个 tile 的方形窗口。窗口外不做三维挖掘。
struct DigVolumeWindow {
    int centerTileX = 0;
    int centerTileZ = 0;
    int radiusTiles = 0;

    [[nodiscard]] constexpr int MinTileX() const noexcept { return centerTileX - radiusTiles; }
    [[nodiscard]] constexpr int MaxTileX() const noexcept { return centerTileX + radiusTiles; }
    [[nodiscard]] constexpr int MinTileZ() const noexcept { return centerTileZ - radiusTiles; }
    [[nodiscard]] constexpr int MaxTileZ() const noexcept { return centerTileZ + radiusTiles; }

    [[nodiscard]] constexpr bool ContainsTile(int tileX, int tileZ) const noexcept {
        return tileX >= MinTileX() && tileX <= MaxTileX() && tileZ >= MinTileZ() && tileZ <= MaxTileZ();
    }

    /// 块是否落在窗口内（块完全属于一个 tile ⇒ 判 tile 即可）。
    [[nodiscard]] constexpr bool ContainsBlock(const BlockCoord& coord) const noexcept {
        return ContainsTile(TileOfBlockIndex(coord.x), TileOfBlockIndex(coord.z));
    }

    /// 块到窗口中心的 **Chebyshev 距离**（单位：tile）—— 脏块超上限时按它淘汰"最远的"。
    [[nodiscard]] constexpr int TileDistanceFromCenter(const BlockCoord& coord) const noexcept {
        const int dx = TileOfBlockIndex(coord.x) - centerTileX;
        const int dz = TileOfBlockIndex(coord.z) - centerTileZ;
        const int ax = (dx >= 0) ? dx : -dx;
        const int az = (dz >= 0) ? dz : -dz;
        return (ax > az) ? ax : az;
    }
};

/// 由**世界坐标（格）**与窗口半径算出玩家窗口。**纯函数**（只依赖入参 ⇒ 红线 7）。
/// `radiusTiles == 0` = 只保留玩家所在的那一个 tile。
[[nodiscard]] DigVolumeWindow WindowForPlayerBlocks(double worldX, double worldZ, int radiusTiles) noexcept;

/// 一次"常驻集合调整"的目标清单（由 `PlanDigVolumeResidency` 算出，**纯数据**）。
///
/// 四个清单都**只由入参唯一决定**（不依赖容器迭代顺序 / 时间 / 线程序）⇒ 可复现（红线 7）。
/// `toCreate` / `toUnload` 按坐标**升序**。
struct DigVolumeResidencyPlan {
    std::vector<BlockCoord> toCreate;      ///< 新进入窗口且尚不常驻 ⇒ 要建
    std::vector<BlockCoord> toUnload;      ///< 离开窗口且**未被改动** ⇒ 要卸
    std::vector<BlockCoord> keptDirty;     ///< 离开窗口但**已被玩家改动** ⇒ 按 ADR 0020 决策五**必须常驻**
    std::vector<BlockCoord> evictedDirty;  ///< 脏块数超上限时被**淘汰**的"最远者"（升序；调用方须 WARN）
};

/// 计算常驻集合的目标变化（**纯函数**）。
///
/// 输入：
///   - `window` = 玩家窗口；
///   - `regionBlocks` = 可挖区域表的块集合（窗口只能从其中取，ADR 0004 硬约束 2；升序）；
///   - `resident` = 当前常驻集合（任意序，内部会排序）；
///   - `isDirty` = 该块是否**已被玩家改动**（⇒ 不得卸载，ADR 0020 决策五）；
///   - `maxKeptDirty` = 允许"离开窗口却仍常驻"的脏块上限；超出即淘汰最远者。
///
/// 语义要点：**脏块优先于卸载** —— 玩家挖过的洞不会因为走远而消失（「世界内一致性」硬要求）。
[[nodiscard]] DigVolumeResidencyPlan PlanDigVolumeResidency(const DigVolumeWindow& window,
                                                           const std::vector<BlockCoord>& regionBlocks,
                                                           const std::vector<BlockCoord>& resident,
                                                           const std::function<bool(const BlockCoord&)>& isDirty,
                                                           std::size_t maxKeptDirty);

/// 可挖体积的**常驻调度器**（ADR 0020 决策二 / 五）：把"玩家窗口"翻译成"每帧建 / 卸几个块"。
///
/// 为什么需要它而不是直接把块集合设成全部：静态全图在 1 km 下要 69~549 MB（超 CPU 预算 ≤ 150 MB），
/// 而窗口内只需 ≈ 7~10 MB，且**不随世界总量增长**。
///
/// 线程约定：只在逻辑线程（主线程）使用；`Step` 里做的写操作（填密度 / 网格化）与 `DigVolumeWorld`
/// 同属"单写者"。**调用方必须把它按帧推进**（一次只做几个块）——单块填充 + 网格化可达数毫秒，
/// 一次做完会让画面停下等待（SKILL「所有重活都必须离开渲染帧」）。
class DigVolumeScheduler final {
public:
    /// **脏块常驻上限**（ADR 0020 决策五）：超出即按"最远优先"淘汰并 **WARN**（不静默）。
    /// 取 256 的依据：一块 ≈ 34 KB 密度 ⇒ 上限约 8.5 MB，仍在 CPU 常驻预算的余量内。
    static constexpr std::size_t kMaxKeptDirtyBlocks = 256;

    /// 前置条件：`regions` 的生命周期覆盖本对象；`radiusTiles >= 0`。
    DigVolumeScheduler(const DigRegionTable& regions, int radiusTiles);

    DigVolumeScheduler(const DigVolumeScheduler&) = delete;
    DigVolumeScheduler& operator=(const DigVolumeScheduler&) = delete;
    DigVolumeScheduler(DigVolumeScheduler&&) = delete;
    DigVolumeScheduler& operator=(DigVolumeScheduler&&) = delete;

    /// 玩家位置变化后调用：重算窗口并生成"待建 / 待卸"清单。
    ///
    /// **幂等**：窗口中心未变时**无操作**（不清空分帧推进中的待办）。
    /// 返回是否**存在待办**（调用方据此决定要不要继续 `Step`）。
    bool Update(const DigVolumeWorld& volumes, double playerX, double playerZ);

    /// 按预算推进：**先建后卸**，各自按坐标升序取至多 `maxActions` 个动作（确定序）；
    /// **淘汰**（脏块超上限的那些）排在最后 —— 它会丢改动，只在前两者做完后才动手。
    /// 实际**发生了**建 / 卸 / 淘汰的块坐标追加到 `changedOut`（供调用方同步碰撞体与 GPU 网格）。
    /// 返回是否**已达成目标集合**（无待办）。
    bool Step(DigVolumeWorld& volumes, std::size_t maxActions, std::vector<BlockCoord>& changedOut);

    [[nodiscard]] const DigVolumeWindow& Window() const noexcept { return m_window; }

    /// 当前窗口内应当常驻的块数（目标集合大小；不随世界总量增长）。
    [[nodiscard]] std::size_t DesiredCount() const noexcept { return m_desiredCount; }

    [[nodiscard]] std::size_t PendingCreateCount() const noexcept {
        return m_pendingCreate.size() - m_createCursor;
    }
    [[nodiscard]] std::size_t PendingUnloadCount() const noexcept {
        return m_pendingUnload.size() - m_unloadCursor;
    }

    /// 待**淘汰**（超上限的脏块）数。
    [[nodiscard]] std::size_t PendingEvictCount() const noexcept { return m_pendingEvict.size() - m_evictCursor; }

    /// 离开窗口但因**已被改动**而继续常驻的脏块数（ADR 0020 决策五）。
    [[nodiscard]] std::size_t KeptDirtyCount() const noexcept { return m_keptDirty.size(); }

    /// 待办动作总数（建 + 卸 + 淘汰；面板显示用）。
    [[nodiscard]] std::size_t PendingActionCount() const noexcept {
        return PendingCreateCount() + PendingUnloadCount() + PendingEvictCount();
    }

    [[nodiscard]] bool HasPendingWork() const noexcept { return PendingActionCount() > 0U; }

private:
    const DigRegionTable& m_regions;
    int                   m_radiusTiles = 0;

    DigVolumeWindow m_window {};
    bool            m_windowValid = false;
    std::size_t     m_desiredCount = 0;

    std::vector<BlockCoord> m_pendingCreate;  ///< 升序
    std::vector<BlockCoord> m_pendingUnload;  ///< 升序
    std::vector<BlockCoord> m_pendingEvict;   ///< 升序（超上限的脏块；**会丢改动**，见 `Update` 的 WARN）
    std::vector<BlockCoord> m_keptDirty;      ///< 升序（诊断 / 面板）
    std::size_t             m_createCursor = 0;
    std::size_t             m_unloadCursor = 0;
    std::size_t             m_evictCursor  = 0;
};

}  // namespace vx
