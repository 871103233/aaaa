#include "streaming/terrain_tile_residency.hpp"

#include "core/log.hpp"

// 仅用于：① 复用可挖体积已验证的**滞回实现**（口径唯一）；② 用 `static_assert` 把两条口径钉在一起，
// 防止"预取环宽 / 滞回带宽"在两个模块里各写一份而悄悄漂移（SKILL：同一结论只能有一个值）。
// 本头文件**不**暴露任何 `dig` 类型（`DigVolume*` 只出现在本 .cpp）。
#include "streaming/dig_volume_residency.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace vx {

static_assert(kTerrainResidencyPrefetchTiles == kResidencyPrefetchTiles,
              "地表 tile 与可挖体积的预取环宽必须同口径（ADR 0020 / 0024）");
static_assert(kTerrainWindowHysteresisBlocks == kWindowHysteresisBlocks,
              "地表 tile 与可挖体积的窗口滞回带宽必须同口径（ADR 0020 / 0024）");

namespace {

/// 编辑块的**确定性**淘汰排序：**远的优先淘汰**；同距按坐标升序（⇒ 结果只由输入决定，红线 7）。
[[nodiscard]] bool IsFartherThan(const TerrainTileWindow& window, const TileCoord& left,
                                 const TileCoord& right) noexcept {
    const int leftDistance  = window.TileDistanceFromCenter(left);
    const int rightDistance = window.TileDistanceFromCenter(right);
    if (leftDistance != rightDistance) {
        return leftDistance > rightDistance;
    }
    return left < right;
}

/// 规划核心（两个重载共用）：输入**已升序**的期望集合，输出建 / 卸 / 编辑块留驻 / 淘汰四清单。
[[nodiscard]] TerrainTileResidencyPlan PlanFromDesired(const TerrainTileWindow& window,
                                                       const std::vector<TileCoord>& desired,
                                                       const std::vector<TileCoord>& resident,
                                                       const std::function<bool(const TileCoord&)>& isEdited,
                                                       std::size_t maxKeptEdited) {
    TerrainTileResidencyPlan plan;
    plan.desiredCount = desired.size();

    std::vector<TileCoord> sortedResident = resident;
    std::sort(sortedResident.begin(), sortedResident.end());

    std::set_difference(desired.begin(), desired.end(), sortedResident.begin(), sortedResident.end(),
                        std::back_inserter(plan.toLoad));

    std::vector<TileCoord> leaving;
    std::set_difference(sortedResident.begin(), sortedResident.end(), desired.begin(), desired.end(),
                        std::back_inserter(leaving));

    // ADR 0020 决策五口径：**已被玩家改动的 tile 不得卸载**（否则玩家改过的地形会随走远而消失）。
    for (const TileCoord& coord : leaving) {
        if (isEdited(coord)) {
            plan.keptEdited.push_back(coord);
        } else {
            plan.toUnload.push_back(coord);
        }
    }

    // 编辑块超上限 ⇒ 淘汰"最远者"，并**交给调用方 WARN**（不静默降级）。
    if (plan.keptEdited.size() > maxKeptEdited) {
        std::vector<TileCoord> byDistance = plan.keptEdited;
        std::sort(byDistance.begin(), byDistance.end(),
                  [&window](const TileCoord& left, const TileCoord& right) {
                      return IsFartherThan(window, left, right);
                  });
        const std::size_t evictCount = plan.keptEdited.size() - maxKeptEdited;
        plan.evictedEdited.assign(byDistance.begin(), byDistance.begin() + static_cast<std::ptrdiff_t>(evictCount));
        std::sort(plan.evictedEdited.begin(), plan.evictedEdited.end());

        // `keptEdited` 收敛为"留下的那些"。
        std::vector<TileCoord> kept;
        kept.reserve(maxKeptEdited);
        std::set_difference(plan.keptEdited.begin(), plan.keptEdited.end(), plan.evictedEdited.begin(),
                            plan.evictedEdited.end(), std::back_inserter(kept));
        plan.keptEdited = std::move(kept);
    }

    return plan;
}

}  // namespace

TerrainTileWindow TerrainWindowForPlayerBlocks(double worldX, double worldZ, int radiusTiles) noexcept {
    const double tileSize = static_cast<double>(kTerrainTileSize);
    TerrainTileWindow window;
    window.centerTileX = static_cast<int>(std::floor(worldX / tileSize));
    window.centerTileZ = static_cast<int>(std::floor(worldZ / tileSize));
    window.radiusTiles = (radiusTiles > 0) ? radiusTiles : 0;
    return window;
}

TerrainTileWindow TerrainResidencyWindowForPlayerBlocks(double worldX, double worldZ, int radiusTiles,
                                                        int prefetchTiles) noexcept {
    const int prefetch = (prefetchTiles > 0) ? prefetchTiles : 0;
    return TerrainWindowForPlayerBlocks(worldX, worldZ, radiusTiles + prefetch);
}

int TerrainHysteresisCenterTile(int centerTile, int playerTile, double playerCoord,
                                double hysteresisBlocks) noexcept {
    return HysteresisCenterTile(centerTile, playerTile, playerCoord, hysteresisBlocks);
}

namespace {

/// LOD 环数（= `TerrainLodRings` 的固定环数，与 `kTerrainLodLevelCount` 同值）。
constexpr int kLodRingCount = 3;

}  // namespace

int TerrainLodLevelForTileDistance(const TerrainLodRings& rings, int tileDistance) noexcept {
    // 环 `i` 的距离区间 = `(radii[i-1], radii[i]]`（`radii` 严格递增，`radii[-1]` 视作 -∞）⇒ 第一个
    // `tileDistance <= radii[i]` 的环即为命中环。
    for (int i = 0; i < kLodRingCount; ++i) {
        if (tileDistance <= rings.radii[i]) {
            return rings.lodLevels[i];
        }
    }
    // 超出最外环 ⇒ 返回最外环的 LOD（调用方本就不该把它纳入常驻）。
    return rings.lodLevels[kLodRingCount - 1];
}

TerrainLodMorphRange TerrainLodMorphRangeForLevel(const TerrainLodRings& rings, int lodLevel) noexcept {
    TerrainLodMorphRange range;

    int ringIndex = -1;
    for (int i = 0; i < kLodRingCount; ++i) {
        if (rings.lodLevels[i] == lodLevel) {
            ringIndex = i;
            break;
        }
    }
    if (ringIndex < 0) {
        // 前置条件保证不会走到这里；未知档按"不 morph"处理（确定性，不越界）。
        return range;
    }

    const float tileSize = static_cast<float>(kTerrainTileSize);
    range.endDistance    = (static_cast<float>(rings.radii[ringIndex]) + 0.5F) * tileSize;
    range.startDistance  = range.endDistance - tileSize;
    // 最外环没有更粗的环可 morph ⇒ 不启用；其余取父级网格步长。
    range.morphStep = (ringIndex == kLodRingCount - 1) ? 0.0F : static_cast<float>(TerrainLodSnapStep(lodLevel));
    return range;
}

TerrainTileResidencyPlan PlanTerrainTileResidency(const TerrainTileWindow& window,
                                                  const std::function<bool(const TileCoord&)>& isAvailable,
                                                  const std::vector<TileCoord>& resident,
                                                  const std::function<bool(const TileCoord&)>& isEdited,
                                                  std::size_t maxKeptEdited) {
    std::vector<TileCoord> desired;
    // **只遍历窗口矩形**（O(窗口)），不扫描世界总量（ADR 0024 决策一的不变量）。
    // 循环按 x → z 升序 ⇒ `desired` 天然升序。
    for (int x = window.MinTileX(); x <= window.MaxTileX(); ++x) {
        for (int z = window.MinTileZ(); z <= window.MaxTileZ(); ++z) {
            const TileCoord coord { x, z };
            if (isAvailable(coord)) {
                desired.push_back(coord);
            }
        }
    }
    return PlanFromDesired(window, desired, resident, isEdited, maxKeptEdited);
}

TerrainTileResidencyPlan PlanTerrainTileResidency(const TerrainTileWindow& window,
                                                  const std::vector<TileCoord>& availableTiles,
                                                  const std::vector<TileCoord>& resident,
                                                  const std::function<bool(const TileCoord&)>& isEdited,
                                                  std::size_t maxKeptEdited) {
    std::vector<TileCoord> desired;
    desired.reserve(availableTiles.size());
    for (const TileCoord& coord : availableTiles) {
        if (window.Contains(coord)) {
            desired.push_back(coord);
        }
    }
    // **显式排序**：即便调用方给的顺序不同，结果也只由**集合**决定（红线 7）。
    std::sort(desired.begin(), desired.end());
    return PlanFromDesired(window, desired, resident, isEdited, maxKeptEdited);
}

TerrainTileScheduler::TerrainTileScheduler(TerrainTileRange range, int radiusTiles, int prefetchTiles,
                                           double hysteresisBlocks, std::function<bool(const TileCoord&)> isEdited)
    : m_range(range),
      m_radiusTiles((radiusTiles > 0) ? radiusTiles : 0),
      m_prefetchTiles((prefetchTiles > 0) ? prefetchTiles : 0),
      m_hysteresisBlocks((hysteresisBlocks > 0.0) ? hysteresisBlocks : 0.0),
      m_isEdited(std::move(isEdited)) {}

TerrainTileScheduler::TerrainTileScheduler(TerrainTileRange range, const TerrainLodRings& rings, int prefetchTiles,
                                           double hysteresisBlocks, std::function<bool(const TileCoord&)> isEdited)
    : m_range(range),
      m_rings(rings),
      m_hasRings(true),
      m_radiusTiles((rings.radii[kLodRingCount - 1] > 0) ? rings.radii[kLodRingCount - 1] : 0),
      m_prefetchTiles((prefetchTiles > 0) ? prefetchTiles : 0),
      m_hysteresisBlocks((hysteresisBlocks > 0.0) ? hysteresisBlocks : 0.0),
      m_isEdited(std::move(isEdited)) {}

int TerrainTileScheduler::LodLevelForTile(const TileCoord& coord) const noexcept {
    if (!m_hasRings) {
        return 0;
    }
    return TerrainLodLevelForTileDistance(m_rings, m_window.TileDistanceFromCenter(coord));
}

void TerrainTileScheduler::CollectPendingLoadTiles(std::vector<TileCoord>& out) const {
    // `m_pendingLoad` 已升序；只取 cursor 之后的切片（尚未被 `Step` 取走的部分），只读不改。
    out.insert(out.end(), m_pendingLoad.begin() + static_cast<std::ptrdiff_t>(m_loadCursor), m_pendingLoad.end());
}

bool TerrainTileScheduler::StepRelod(std::size_t maxActions, std::vector<TileCoord>& relodOut) {
    std::size_t actions = 0;
    while (actions < maxActions && m_relodCursor < m_pendingRelod.size()) {
        const TileCoord coord = m_pendingRelod[m_relodCursor++];
        m_residentLod[coord]  = LodLevelForTile(coord);  // 更新"上次告诉调用方的 LOD"
        relodOut.push_back(coord);
        ++actions;
    }
    return m_relodCursor >= m_pendingRelod.size();
}

bool TerrainTileScheduler::Update(const TerrainWorld& world, double playerX, double playerZ) {
    const double tileSize  = static_cast<double>(kTerrainTileSize);
    const int    playerTileX = static_cast<int>(std::floor(playerX / tileSize));
    const int    playerTileZ = static_cast<int>(std::floor(playerZ / tileSize));

    int centerX = playerTileX;
    int centerZ = playerTileZ;
    if (m_windowValid) {
        centerX = TerrainHysteresisCenterTile(m_window.centerTileX, playerTileX, playerX, m_hysteresisBlocks);
        centerZ = TerrainHysteresisCenterTile(m_window.centerTileZ, playerTileZ, playerZ, m_hysteresisBlocks);
        if (centerX == m_window.centerTileX && centerZ == m_window.centerTileZ) {
            return HasPendingWork();  // **幂等**：中心未变 ⇒ 不清空分帧推进中的待办
        }
    }

    m_window          = TerrainTileWindow { centerX, centerZ, m_radiusTiles };
    m_residencyWindow = TerrainTileWindow { centerX, centerZ, m_radiusTiles + m_prefetchTiles };
    m_windowValid     = true;

    const auto isAvailable = [this](const TileCoord& coord) { return m_range.Contains(coord); };
    const auto isEdited    = [this](const TileCoord& coord) {
        return m_isEdited ? m_isEdited(coord) : false;
    };

    const TerrainTileResidencyPlan plan = PlanTerrainTileResidency(m_residencyWindow, isAvailable,
                                                                  world.ResidentTiles(), isEdited,
                                                                  kMaxKeptEditedTiles);

    m_pendingLoad   = plan.toLoad;
    m_pendingUnload = plan.toUnload;
    m_keptEdited    = plan.keptEdited;
    m_evictedEdited = plan.evictedEdited;
    m_loadCursor    = 0;
    m_unloadCursor  = 0;
    m_desiredCount  = plan.desiredCount;

    // **待 relod**：常驻集合中目标 LOD 与"上次告诉调用方的 LOD"不同的 tile（升序；`ResidentTiles` 已升序）。
    // 新加载的 tile 不在常驻集合里 ⇒ **不进 relod 清单**（`Step` 加载时已按目标 LOD 建）。
    m_pendingRelod.clear();
    m_relodCursor = 0;
    if (m_hasRings) {
        for (const TileCoord& coord : world.ResidentTiles()) {
            const auto recorded = m_residentLod.find(coord);
            if (recorded == m_residentLod.end() || recorded->second != LodLevelForTile(coord)) {
                m_pendingRelod.push_back(coord);
            }
        }
    }

    if (!m_evictedEdited.empty()) {
        VX_LOG_WARN("地形流式：被编辑的 tile 超出常驻上限（%u），淘汰最远的 %u 块（**会丢改动**）",
                    static_cast<unsigned>(kMaxKeptEditedTiles), static_cast<unsigned>(m_evictedEdited.size()));
    }
    return HasPendingWork();
}

bool TerrainTileScheduler::Step(TerrainWorld& world, std::size_t maxActions, std::vector<TileCoord>& changedOut) {
    std::size_t actions = 0;

    // **先加载后卸载**（先保证新窗口可见，再释放旧窗口），各自按坐标升序（确定序）。
    while (actions < maxActions && m_loadCursor < m_pendingLoad.size()) {
        const TileCoord coord = m_pendingLoad[m_loadCursor++];
        const int      lod    = LodLevelForTile(coord);
        world.LoadTile(coord.x, coord.z, lod);  // 单半径构造下 `lod == 0` ⇒ 与从前逐位一致
        m_residentLod[coord] = lod;             // 记下"已按该 LOD 建好"，避免随后误报 relod
        changedOut.push_back(coord);
        ++actions;
    }
    while (actions < maxActions && m_unloadCursor < m_pendingUnload.size()) {
        const TileCoord coord       = m_pendingUnload[m_unloadCursor++];
        const bool      wasResident = world.UnloadTile(coord.x, coord.z);
        m_residentLod.erase(coord);
        if (wasResident) {
            changedOut.push_back(coord);
        }
        ++actions;
    }

    return !HasPendingWork();
}

}  // namespace vx
