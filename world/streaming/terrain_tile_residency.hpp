#pragma once

#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <vector>

namespace vx {

/// 地表 tile 的**常驻窗口**（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策一）：
/// 以玩家所在 tile 为中心、半径 `radiusTiles` 个 tile 的方形窗口。
///
/// **不变量**：常驻集合只由本窗口决定，**与世界总大小无关**（每帧成本 O(窗口)，禁止 O(世界总量) 扫描）。
struct TerrainTileWindow {
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

    [[nodiscard]] constexpr bool Contains(const TileCoord& coord) const noexcept {
        return ContainsTile(coord.x, coord.z);
    }

    /// tile 到窗口中心的 **Chebyshev 距离**（单位：tile）—— 编辑块超上限时按它淘汰"最远的"。
    [[nodiscard]] constexpr int TileDistanceFromCenter(const TileCoord& coord) const noexcept {
        const int dx = coord.x - centerTileX;
        const int dz = coord.z - centerTileZ;
        const int ax = (dx >= 0) ? dx : -dx;
        const int az = (dz >= 0) ? dz : -dz;
        return (ax > az) ? ax : az;
    }

    /// 窗口内应有的 tile 数（=`(2R+1)²`）；用来核对"常驻量只随窗口变化"。
    [[nodiscard]] constexpr std::size_t TileCount() const noexcept {
        const std::size_t side = static_cast<std::size_t>(2 * radiusTiles + 1);
        return side * side;
    }
};

/// **世界内存在的 tile 范围**（矩形；预制地图头 / 世界边界推导）。空范围 = 一个 tile 都没有。
struct TerrainTileRange {
    int minTileX  = 0;
    int minTileZ  = 0;
    int tileCountX = 0;  ///< `<= 0` ⇒ 空范围
    int tileCountZ = 0;

    [[nodiscard]] constexpr bool Empty() const noexcept { return tileCountX <= 0 || tileCountZ <= 0; }
    [[nodiscard]] constexpr int MaxTileX() const noexcept { return minTileX + tileCountX - 1; }
    [[nodiscard]] constexpr int MaxTileZ() const noexcept { return minTileZ + tileCountZ - 1; }

    [[nodiscard]] constexpr bool Contains(const TileCoord& coord) const noexcept {
        return !Empty() && coord.x >= minTileX && coord.x <= MaxTileX() && coord.z >= minTileZ &&
               coord.z <= MaxTileZ();
    }

    [[nodiscard]] constexpr std::size_t TileCount() const noexcept {
        return Empty() ? 0U : (static_cast<std::size_t>(tileCountX) * static_cast<std::size_t>(tileCountZ));
    }
};

/// 构造一个 tile 范围（`tileCountX/Z <= 0` ⇒ 空范围）。纯函数。
[[nodiscard]] constexpr TerrainTileRange MakeTerrainTileRange(int minTileX, int minTileZ, int tileCountX,
                                                             int tileCountZ) noexcept {
    return TerrainTileRange { minTileX, minTileZ, tileCountX, tileCountZ };
}

/// 由**世界坐标（格）**与半径算出**活动窗口**（纯函数，红线 7：只依赖入参）。
///
/// 中心 tile = `floor(world / 64)`（`std::floor` 保证负坐标也向下取整）。`radiusTiles < 0` 按 0 处理。
[[nodiscard]] TerrainTileWindow TerrainWindowForPlayerBlocks(double worldX, double worldZ,
                                                             int radiusTiles) noexcept;

/// 由世界坐标、**活动半径**与**预取环宽**算出**常驻窗口**（纯函数，红线 7）。
///
/// 语义（复用 ADR 0020 的 T80 修订口径）：常驻半径 = `radiusTiles + prefetchTiles`，与活动窗口**共用同一个中心 tile**
/// ⇒ 常驻集合是活动集合的超集。预取环让"将要进入"的 tile 提前建好，跨越 tile 边界时不再集中建一整圈。
[[nodiscard]] TerrainTileWindow TerrainResidencyWindowForPlayerBlocks(double worldX, double worldZ, int radiusTiles,
                                                                     int prefetchTiles) noexcept;

/// **预取环宽度**（tile）。与可挖体积同口径（ADR 0020 的 `kResidencyPrefetchTiles`），
/// 由 `terrain_tile_residency.cpp` 的 `static_assert` 钉住一致性（防止两处漂移）。
inline constexpr int kTerrainResidencyPrefetchTiles = 1;

/// **窗口滞回带宽**（格）。与可挖体积同口径（ADR 0020 的 `kWindowHysteresisBlocks`，T73），同上由 `static_assert` 钉住。
/// 用途：出生点常压在 tile 边界上 ⇒ 亚格级抖动会让中心 tile 反复翻，每次都是一整圈的建 / 卸。
inline constexpr double kTerrainWindowHysteresisBlocks = 16.0;

/// 带**滞回**地推进窗口中心 tile（纯函数，红线 7）。口径与可挖体积一致（ADR 0020 决策二 / T73）：
///   - `playerTile == centerTile` ⇒ 不变；
///   - 相差 **≥ 2 格**（传送 / 越界救援后的远跳）⇒ **直接跳到 `playerTile`**（不逐格挪）；
///   - 相差 **恰好 1 格** ⇒ 仅当玩家沿该方向**越过边界 ≥ `hysteresisBlocks` 格**时才挪一格；
///   - `hysteresisBlocks <= 0` ⇒ 等价于无滞回（越过边界即挪一格）。
[[nodiscard]] int TerrainHysteresisCenterTile(int centerTile, int playerTile, double playerCoord,
                                              double hysteresisBlocks) noexcept;

/// 一次"常驻集合调整"的目标清单（由 `PlanTerrainTileResidency` 算出，**纯数据**）。
///
/// 四个清单都**只由入参唯一决定**（不依赖容器迭代顺序 / 时间 / 线程序）⇒ 可复现（红线 7）；均按坐标**升序**。
struct TerrainTileResidencyPlan {
    std::vector<TileCoord> toLoad;        ///< 新进入窗口且尚不常驻 ⇒ 要加载 / 生成
    std::vector<TileCoord> toUnload;      ///< 离开窗口且**未被编辑** ⇒ 要卸
    std::vector<TileCoord> keptEdited;    ///< 离开窗口但**已被编辑** ⇒ 按 ADR 0020 决策五**必须常驻**
    std::vector<TileCoord> evictedEdited;  ///< 编辑块数超上限时被**淘汰**的"最远者"（升序；调用方须 **WARN**）
    std::size_t            desiredCount = 0;  ///< 期望常驻的 tile 数（= 窗口 ∩ 世界存在；不随世界总量增长）
};

/// 计算常驻集合的目标变化（**范围版**，成本 **O(窗口)**）。
///
/// 输入：
///   - `window` = **常驻窗口**（活动 + 预取环）；
///   - `isAvailable` = 该 tile 在**世界内是否存在**（预制地图索引 / 世界边界内）；
///   - `resident` = 当前常驻集合（任意序，内部会排序）；
///   - `isEdited` = 该 tile 是否**已被玩家改动**（⇒ 不得卸载，ADR 0020 决策五口径）；
///   - `maxKeptEdited` = 允许"离开窗口却仍常驻"的编辑块上限；超出即淘汰**最远者**。
///
/// 语义要点：**编辑块优先于卸载** —— 玩家改过的地形不会因为走远而消失（「世界内一致性」硬要求）。
[[nodiscard]] TerrainTileResidencyPlan PlanTerrainTileResidency(
    const TerrainTileWindow& window, const std::function<bool(const TileCoord&)>& isAvailable,
    const std::vector<TileCoord>& resident, const std::function<bool(const TileCoord&)>& isEdited,
    std::size_t maxKeptEdited);

/// 计算常驻集合的目标变化（**清单版**；`availableTiles` 任意序）。语义与范围版完全一致，便于单测与离线核对。
[[nodiscard]] TerrainTileResidencyPlan PlanTerrainTileResidency(
    const TerrainTileWindow& window, const std::vector<TileCoord>& availableTiles,
    const std::vector<TileCoord>& resident, const std::function<bool(const TileCoord&)>& isEdited,
    std::size_t maxKeptEdited);

/// **LOD 分环**（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策二）：
/// 按 **Chebyshev tile 距离**（与 `TerrainTileWindow::TileDistanceFromCenter` 同源）把常驻集合切成 3 个环，
/// 环 `i` 的 tile 距离区间 = `(radii[i-1], radii[i]]`，使用 `lodLevels[i]`。
///
/// 前置条件：`radii` **严格递增**且 `>= 0`；`lodLevels` **严格递增**且 ∈ `[0, kTerrainLodLevelCount)`。
struct TerrainLodRings {
    int radii[3]     = { 8, 16, 32 };  ///< Ring 0 = 0..8 tile（≈512 m）、Ring 1 = 9..16、Ring 2 = 17..32（≈2048 m）
    int lodLevels[3] = { 0, 1, 2 };
};

/// 纯函数（红线 7）：某 tile 距离落在哪个环 ⇒ 该环的 LOD 档；**超出最外环** ⇒ 返回最外环的 LOD
///（调用方本就不该把它纳入常驻）。
[[nodiscard]] int TerrainLodLevelForTileDistance(const TerrainLodRings& rings, int tileDistance) noexcept;

/// **某 LOD 档的 CDLOD morph 参数**（与 `assets/shaders/mesh.vert` 的 morph 数学同源）。
struct TerrainLodMorphRange {
    float morphStep     = 0.0F;  ///< 父级网格步长 = `TerrainLodSnapStep(lodLevel)`；`0` = 不启用（最外环）
    float startDistance = 0.0F;  ///< morph 起点（格）= 环外边界 − `kTerrainTileSize`
    float endDistance   = 0.0F;  ///< morph 终点 / 环外边界（格）= `(radii[环] + 0.5) * kTerrainTileSize`
};

/// 纯函数（红线 7）：某 LOD 档的 CDLOD morph 参数。
///
/// 找到 `lodLevels` 里等于该 `lodLevel` 的环下标 `i`：
///   - `i` 是**最外环** ⇒ `morphStep = 0`（没有更粗的环可 morph）；
///   - 否则 `morphStep = TerrainLodSnapStep(lodLevel)`；
///   - `endDistance = (radii[i] + 0.5) * kTerrainTileSize`、`startDistance = endDistance - kTerrainTileSize`
///     （在环最外一圈 tile 内完成过渡 ⇒ morph 因子 `k` 恰在环边界取 1）。
[[nodiscard]] TerrainLodMorphRange TerrainLodMorphRangeForLevel(const TerrainLodRings& rings,
                                                                int lodLevel) noexcept;

/// 地表 tile 的**常驻调度器**（W7-S2 / [ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策一）：
/// 把"玩家窗口"翻译成"每帧加载 / 卸载几个 tile"。
///
/// 不变量：常驻集合 = 常驻窗口 ∩ 世界内存在的 tile，**与世界总大小无关**（`Update` 成本 O(窗口)）。
/// 建 / 卸一律**分帧**（`Step` 每帧只做 `maxActions` 个、确定序），**禁止**一次做完而让画面停下等待
///（SKILL 第四节「所有重活都必须离开渲染帧」）。S3 起"生成 / 网格化"将进一步下沉 worker（ADR 0022 形态）。
///
/// 线程约定：只在逻辑线程（主线程）使用（`Update` / `Step` 都读写 `TerrainWorld`）。
class TerrainTileScheduler final {
public:
    /// **编辑块常驻上限**（ADR 0020 决策五口径）：超出即按"最远优先"淘汰并 **WARN**（不静默）。
    /// 一块地表 tile ≈ 8 KB 高度 + 网格 ⇒ 上限约数 MB，仍在 CPU 常驻预算的余量内。
    static constexpr std::size_t kMaxKeptEditedTiles = 256;

    /// 前置条件：`radiusTiles >= 0`；`prefetchTiles >= 0`；`range` 为世界内存在的 tile 范围。
    /// `isEdited` 可为空（= 世界内没有被编辑过的 tile；破坏子系统休眠期间即此形态）。
    TerrainTileScheduler(TerrainTileRange range, int radiusTiles,
                         int prefetchTiles = kTerrainResidencyPrefetchTiles,
                         double hysteresisBlocks = kTerrainWindowHysteresisBlocks,
                         std::function<bool(const TileCoord&)> isEdited = {});

    /// **分环构造**（W7-S3b / ADR 0024 决策二）：活动半径 = `rings.radii[2]`；
    /// 常驻半径 = `rings.radii[2] + prefetchTiles`（与单半径构造同构）。
    ///
    /// 前置条件同 `TerrainLodRings`（`radii` 严格递增且 `>= 0`；`lodLevels` 严格递增且 ∈ `[0, kTerrainLodLevelCount)`）。
    /// 与单半径构造的区别：`HasLodRings() == true`，tile 按环取 LOD，且在**中心移动**时会报出待 relod。
    TerrainTileScheduler(TerrainTileRange range, const TerrainLodRings& rings,
                         int prefetchTiles = kTerrainResidencyPrefetchTiles,
                         double hysteresisBlocks = kTerrainWindowHysteresisBlocks,
                         std::function<bool(const TileCoord&)> isEdited = {});

    TerrainTileScheduler(const TerrainTileScheduler&) = delete;
    TerrainTileScheduler& operator=(const TerrainTileScheduler&) = delete;
    TerrainTileScheduler(TerrainTileScheduler&&) = delete;
    TerrainTileScheduler& operator=(TerrainTileScheduler&&) = delete;

    /// 玩家位置变化后调用：**滞回**推进中心 tile，并重算"待加载 / 待卸载"清单。
    /// **幂等**：中心 tile 未变时**无操作**（不清空分帧推进中的待办）。返回是否存在待办。
    bool Update(const TerrainWorld& world, double playerX, double playerZ);

    /// 按预算推进：**先加载后卸载**，各自按坐标升序取至多 `maxActions` 个动作（确定序）。
    /// 实际发生动作的 tile 坐标追加到 `changedOut`（加载必然发生；卸载仅在确实常驻时记录）。
    /// 返回是否**已达成目标集合**（无待办）。
    bool Step(TerrainWorld& world, std::size_t maxActions, std::vector<TileCoord>& changedOut);

    /// **活动窗口** = `center ± radiusTiles`。
    [[nodiscard]] const TerrainTileWindow& Window() const noexcept { return m_window; }

    /// **常驻窗口** = `center ± (radiusTiles + prefetchTiles)`（哪些 tile 应当常驻，含预取环）。
    [[nodiscard]] const TerrainTileWindow& ResidencyWindow() const noexcept { return m_residencyWindow; }

    /// 目标常驻集合大小（= 常驻窗口 ∩ 世界存在；**不随世界总量增长**）。
    [[nodiscard]] std::size_t DesiredCount() const noexcept { return m_desiredCount; }

    /// **尚未被 `Step` 取走**的首个待加载坐标；没有待加载 ⇒ `nullptr`。只读，不改调度状态。
    /// 用途：主线程据此判断"下一个要安装的 tile 是否已就绪"，从而**避免**在渲染帧内同步生成。
    [[nodiscard]] const TileCoord* NextPendingLoadTile() const noexcept {
        return (m_loadCursor < m_pendingLoad.size()) ? &m_pendingLoad[m_loadCursor] : nullptr;
    }

    /// 把**尚未被 `Step` 取走**的待加载坐标（`cursor` 起的切片，升序）**追加**到 `out`（`out` 不预清空）。
    /// 只读，不改调度状态。用途：主线程据它对尚未提交的坐标**提前提交 worker 任务**
    /// （预取必须在玩家看到之前完成，[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策一）。
    void CollectPendingLoadTiles(std::vector<TileCoord>& out) const;

    [[nodiscard]] std::size_t PendingLoadCount() const noexcept { return m_pendingLoad.size() - m_loadCursor; }
    [[nodiscard]] std::size_t PendingUnloadCount() const noexcept { return m_pendingUnload.size() - m_unloadCursor; }
    [[nodiscard]] std::size_t PendingActionCount() const noexcept {
        return PendingLoadCount() + PendingUnloadCount();
    }
    /// **待 relod 数**（LOD 变了的常驻 tile；单半径构造下恒为 0）。
    [[nodiscard]] std::size_t PendingRelodCount() const noexcept {
        return m_pendingRelod.size() - m_relodCursor;
    }
    [[nodiscard]] bool HasPendingWork() const noexcept {
        return PendingActionCount() > 0U || PendingRelodCount() > 0U;
    }

    /// 是否使用 **LOD 分环**构造（单半径构造 ⇒ `false`，所有 tile 的 LOD 恒为 0、永不 relod）。
    [[nodiscard]] bool HasLodRings() const noexcept { return m_hasRings; }

    /// 某 tile 的**目标 LOD 档**（按环的 Chebyshev 距离）。**无环 ⇒ 恒为 0**。
    [[nodiscard]] int LodLevelForTile(const TileCoord& coord) const noexcept;

    /// 按预算推进 **relod**：把 LOD 变了的常驻 tile 坐标（**升序**）追加到 `relodOut`，并更新内部记录。
    /// 返回是否**已无待办 relod**。relod **不触碰 `TerrainWorld`**（只改网格 LOD ⇒ 由调用方重新网格化 + 重传）。
    bool StepRelod(std::size_t maxActions, std::vector<TileCoord>& relodOut);

    /// 离开窗口但因**已被编辑**而继续常驻的 tile 数（ADR 0020 决策五）。
    [[nodiscard]] std::size_t KeptEditedCount() const noexcept { return m_keptEdited.size(); }

    /// 上一次 `Update` 中因超上限被**淘汰**的编辑块数（> 0 时 `Update` 已 **WARN**）。
    [[nodiscard]] std::size_t EvictedEditedCount() const noexcept { return m_evictedEdited.size(); }

private:
    TerrainTileRange                      m_range;
    TerrainLodRings                       m_rings {};         ///< 仅分环构造使用
    bool                                  m_hasRings         = false;
    int                                   m_radiusTiles      = 0;
    int                                   m_prefetchTiles    = 0;
    double                                m_hysteresisBlocks = 0.0;
    std::function<bool(const TileCoord&)> m_isEdited;

    TerrainTileWindow m_window {};           ///< 活动窗口
    TerrainTileWindow m_residencyWindow {};  ///< 常驻窗口
    bool              m_windowValid  = false;
    std::size_t       m_desiredCount = 0;

    std::vector<TileCoord> m_pendingLoad;    ///< 升序
    std::vector<TileCoord> m_pendingUnload;  ///< 升序
    std::vector<TileCoord> m_pendingRelod;   ///< 升序（LOD 变了的常驻 tile）
    std::vector<TileCoord> m_keptEdited;     ///< 升序（诊断 / 面板）
    std::vector<TileCoord> m_evictedEdited;  ///< 升序（超上限的编辑块；**会丢改动**，`Update` 已 WARN）
    std::size_t            m_loadCursor   = 0;
    std::size_t            m_unloadCursor = 0;
    std::size_t            m_relodCursor  = 0;

    /// "上次告诉调用方的 LOD"（`Step` 加载时记入、卸载时擦除；`StepRelod` 推进时更新）。
    std::map<TileCoord, int> m_residentLod;
};

}  // namespace vx
