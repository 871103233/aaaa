#include "streaming/dig_volume_residency.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace vx {
namespace {

/// 窗口中心 tile 的**确定性**淘汰排序：**远的优先淘汰**；同距按坐标升序（⇒ 结果只由输入决定）。
[[nodiscard]] bool IsFartherThan(const DigVolumeWindow& window, const BlockCoord& left,
                                 const BlockCoord& right) noexcept {
    const int leftDistance  = window.TileDistanceFromCenter(left);
    const int rightDistance = window.TileDistanceFromCenter(right);
    if (leftDistance != rightDistance) {
        return leftDistance > rightDistance;
    }
    return left < right;
}

}  // namespace

DigVolumeWindow WindowForPlayerBlocks(double worldX, double worldZ, int radiusTiles) noexcept {
    const int radius = (radiusTiles > 0) ? radiusTiles : 0;
    // 世界列 → tile：`floor(world / 64)`（用 `std::floor` 保证负坐标也向下取整）。
    const double tileSize = static_cast<double>(kTerrainTileSize);
    DigVolumeWindow window;
    window.centerTileX = static_cast<int>(std::floor(worldX / tileSize));
    window.centerTileZ = static_cast<int>(std::floor(worldZ / tileSize));
    window.radiusTiles = radius;
    return window;
}

DigVolumeResidencyPlan PlanDigVolumeResidency(const DigVolumeWindow& window,
                                             const std::vector<BlockCoord>& regionBlocks,
                                             const std::vector<BlockCoord>& resident,
                                             const std::function<bool(const BlockCoord&)>& isDirty,
                                             std::size_t maxKeptDirty) {
    DigVolumeResidencyPlan plan;

    // 期望集合 = 区域块 ∩ 窗口。`regionBlocks` 升序（`DigRegionTable` 保证）⇒ 结果升序、确定。
    std::vector<BlockCoord> desired;
    desired.reserve(regionBlocks.size());
    for (const BlockCoord& coord : regionBlocks) {
        if (window.ContainsBlock(coord)) {
            desired.push_back(coord);
        }
    }

    // 常驻集合先归一化排序：这样即便调用方给的是任意序，结果也只由**集合**决定（红线 7）。
    std::vector<BlockCoord> sortedResident = resident;
    std::sort(sortedResident.begin(), sortedResident.end());

    std::set_difference(desired.begin(), desired.end(), sortedResident.begin(), sortedResident.end(),
                        std::back_inserter(plan.toCreate));

    std::vector<BlockCoord> leaving;
    std::set_difference(sortedResident.begin(), sortedResident.end(), desired.begin(), desired.end(),
                        std::back_inserter(leaving));

    // ADR 0020 决策五：**已被玩家改动的块不得卸载**（否则玩家挖的洞会随走远而消失）。
    for (const BlockCoord& coord : leaving) {
        if (isDirty(coord)) {
            plan.keptDirty.push_back(coord);
        } else {
            plan.toUnload.push_back(coord);
        }
    }

    // 脏块超上限 ⇒ 淘汰最远者，并**交给调用方 WARN**（不静默降级）。
    if (plan.keptDirty.size() > maxKeptDirty) {
        std::vector<BlockCoord> byDistance = plan.keptDirty;
        std::sort(byDistance.begin(), byDistance.end(),
                  [&window](const BlockCoord& left, const BlockCoord& right) {
                      return IsFartherThan(window, left, right);
                  });
        const std::size_t evictCount = plan.keptDirty.size() - maxKeptDirty;
        plan.evictedDirty.assign(byDistance.begin(), byDistance.begin() + static_cast<std::ptrdiff_t>(evictCount));
        std::sort(plan.evictedDirty.begin(), plan.evictedDirty.end());

        // `keptDirty` 收敛为"留下的那些"。
        std::vector<BlockCoord> kept;
        kept.reserve(maxKeptDirty);
        std::set_difference(plan.keptDirty.begin(), plan.keptDirty.end(), plan.evictedDirty.begin(),
                            plan.evictedDirty.end(), std::back_inserter(kept));
        plan.keptDirty = std::move(kept);
    }

    return plan;
}

DigVolumeScheduler::DigVolumeScheduler(const DigRegionTable& regions, int radiusTiles)
    : m_regions(regions), m_radiusTiles((radiusTiles > 0) ? radiusTiles : 0) {}

bool DigVolumeScheduler::Update(const DigVolumeWorld& volumes, double playerX, double playerZ) {
    const DigVolumeWindow next = WindowForPlayerBlocks(playerX, playerZ, m_radiusTiles);
    if (m_windowValid && next.centerTileX == m_window.centerTileX && next.centerTileZ == m_window.centerTileZ) {
        return HasPendingWork();  // 幂等：窗口没动就别打乱正在分帧推进的待办
    }

    m_window      = next;
    m_windowValid = true;

    m_desiredCount = 0;
    for (const BlockCoord& coord : m_regions.Blocks()) {
        if (m_window.ContainsBlock(coord)) {
            ++m_desiredCount;
        }
    }

    const std::vector<BlockCoord> resident = volumes.ResidentBlocks();
    const DigVolumeResidencyPlan  plan     = PlanDigVolumeResidency(
        m_window, m_regions.Blocks(), resident,
        [&volumes](const BlockCoord& coord) { return volumes.IsBlockDirty(coord); }, kMaxKeptDirtyBlocks);

    m_pendingCreate = plan.toCreate;
    m_pendingUnload = plan.toUnload;
    m_keptDirty     = plan.keptDirty;
    m_createCursor  = 0;
    m_unloadCursor  = 0;

    // 淘汰清单单独存放：它们**已被玩家改动**，故不能走"拒绝卸载脏块"的普通通道（`UnloadBlock`），
    // 只能走 `EvictBlock`（ADR 0020 决策五的出口）。先 WARN，再由 `Step` 末尾动手（不静默降级）。
    m_pendingEvict = plan.evictedDirty;
    m_evictCursor  = 0;
    if (!plan.evictedDirty.empty()) {
        VX_LOG_WARN("可挖体积脏块常驻超上限（%zu > %zu）⇒ 淘汰最远的 %zu 块（ADR 0020 决策五）："
                    "**这些块里玩家挖过的洞会消失**；上限见 DigVolumeScheduler::kMaxKeptDirtyBlocks",
                    plan.keptDirty.size() + plan.evictedDirty.size(), kMaxKeptDirtyBlocks,
                    plan.evictedDirty.size());
    }

    return HasPendingWork();
}

bool DigVolumeScheduler::Step(DigVolumeWorld& volumes, std::size_t maxActions,
                              std::vector<BlockCoord>& changedOut) {
    std::size_t actions = 0;

    // **先建后卸**：先让窗口内该有的块存在（否则玩家会看到"脚下没体积"），再释放走远了的。
    while (actions < maxActions && m_createCursor < m_pendingCreate.size()) {
        const BlockCoord coord = m_pendingCreate[m_createCursor];
        ++m_createCursor;
        if (volumes.CreateBlock(coord)) {
            changedOut.push_back(coord);
        }
        ++actions;  // 失败（区域外 / 已存在）也计入动作，避免死循环
    }
    if (m_createCursor >= m_pendingCreate.size()) {
        m_pendingCreate.clear();
        m_createCursor = 0;
    }

    while (actions < maxActions && m_unloadCursor < m_pendingUnload.size()) {
        const BlockCoord coord = m_pendingUnload[m_unloadCursor];
        ++m_unloadCursor;
        if (volumes.UnloadBlock(coord)) {
            changedOut.push_back(coord);
        }
        ++actions;
    }
    if (m_unloadCursor >= m_pendingUnload.size()) {
        m_pendingUnload.clear();
        m_unloadCursor = 0;
    }

    // **淘汰放最后**：它丢改动，只有在前两者都做完后才动手（"先补上该有的，再丢最远的"）。
    while (actions < maxActions && m_evictCursor < m_pendingEvict.size()) {
        const BlockCoord coord = m_pendingEvict[m_evictCursor];
        ++m_evictCursor;
        if (volumes.EvictBlock(coord)) {
            changedOut.push_back(coord);
        }
        ++actions;
    }
    if (m_evictCursor >= m_pendingEvict.size()) {
        m_pendingEvict.clear();
        m_evictCursor = 0;
    }

    return !HasPendingWork();
}

}  // namespace vx
