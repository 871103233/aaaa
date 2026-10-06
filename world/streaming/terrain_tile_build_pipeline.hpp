#pragma once

#include "core/task_scheduler.hpp"
#include "generation/map_preset.hpp"
#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_tile_source.hpp"
#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace vx {

/// 一个地表 tile 的**构建请求**（worker 任务的输入，纯数据；不含任何 `TerrainWorld` 状态）。
struct TerrainTileBuildRequest {
    TileCoord coord {};
    int       lodLevel = 0;

    /// `remeshOnly == true` ⇒ `tile` 是**高度快照**，worker 只做网格化（relod：高度不变、只换 LOD）；
    /// `false` ⇒ 按 `(种子, 参数, 编辑, coord)` **生成 + 网格化**（加载）。
    bool        remeshOnly = false;

    /// relod 用高度快照（8 KB；由主线程在提交时从当前常驻 tile 拷一份 ⇒ worker 只读、不碰世界）。
    TerrainTile tile {};
};

/// 一个地表 tile 的**构建结果**（worker 产出 → 主线程收包）。
struct TerrainTileBuildResult {
    TileCoord       coord {};
    int             lodLevel   = 0;
    bool            remeshOnly = false;
    TerrainTile     tile {};  ///< 加载（`remeshOnly == false`）时为完整 tile；relod 时不使用
    TerrainTileMesh mesh {};  ///< **未过滤**网格（层间交接过滤由主线程安装时应用）
    double          computeMs = 0.0;
};

/// 地表 tile 的**构建任务池**（W7-S3b / [ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md) 形态，
/// 复用 [ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 的流式调度）。
///
/// 形态（业界标准：**Sodium chunk builder** / **UE5 Task Graph** / Unity Job System `MeshDataArray`）：
///   - **主线程** `Submit(request)`：只入队 + 起任务（**非阻塞**）；
///   - **worker**：跑**纯函数** `GenerateTerrainTileData`（加载）/ `BuildTerrainMesh`（relod）——
///     **不读 `TerrainWorld`、不碰图形 API、不加锁**，各自持有一份由 `(seed, params)` 构造的
///     `TerrainNoiseGenerator`（接口全 `const noexcept`、无共享可变状态 ⇒ 可并发只读）；
///   - **主线程** `TakeCompleted(result)`：**非阻塞**取回结果，再做"过滤 + 安装 + GPU 上传"。
///
/// **为什么 worker 不做四边形过滤**：`ITerrainQuadFilter` 依赖**当前常驻集合**这一可变状态，
/// 放进 worker 必须快照 ⇒ 结果随快照陈旧而有歧义。改为主线程安装时按当前过滤器应用，语义与同步路径逐位一致。
///
/// 并发契约（`references/concurrency.md`）：
///   - **单写者 + 快照**：worker 独占自己的 `request` / 输出；`m_inFlight` 只由主线程触碰；
///   - **唯一的互斥量只保护完成队列**（几行 push / pop），**不保护任何世界数据**（红线 9）；
///   - **GPU 上传仍在渲染线程**（本模块完全不碰渲染器）。
///
/// 线程约定：`Submit` / `TakeCompleted` / `PendingCount` / `Stats` **只在主线程调用**；
/// `~TerrainTileBuildPipeline` 会先等待全部任务结束（安全停机）。
class TerrainTileBuildPipeline final {
public:
    /// `workerThreads == 0` ⇒ 自动（硬件并发 − 1）。线程池不可用时**自动回落**为"提交即同步算完"
    /// ——结果不变，只是尖峰回到引入本模块之前的样子（`TaskScheduler` 会 WARN 一次，不静默）。
    /// `tileSource`（可选、**非拥有**）—— V4（[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）：
    /// **非空**时 worker 优先读它取 tile（如预制地图），来源缺该 tile ⇒ **回退程序化生成**；
    /// `nullptr`（缺省）= 与引入本能力之前**逐位一致**的程序化路径。
    /// 生命周期：`tileSource` 必须覆盖本对象（worker 会**并发**调用它，故实现须线程安全）。
    TerrainTileBuildPipeline(std::uint64_t worldSeed, TerrainGenerationParams params, std::vector<MapEdit> edits,
                             unsigned workerThreads = 0, const ITerrainTileSource* tileSource = nullptr);
    ~TerrainTileBuildPipeline();

    TerrainTileBuildPipeline(const TerrainTileBuildPipeline&) = delete;
    TerrainTileBuildPipeline& operator=(const TerrainTileBuildPipeline&) = delete;
    TerrainTileBuildPipeline(TerrainTileBuildPipeline&&) = delete;
    TerrainTileBuildPipeline& operator=(TerrainTileBuildPipeline&&) = delete;

    /// 是否有真实 worker（false ⇒ 同步回退路径）。供日志 / 面板显示。
    [[nodiscard]] bool     HasWorkers() const noexcept { return m_scheduler.Valid(); }
    [[nodiscard]] unsigned WorkerThreadCount() const noexcept { return m_scheduler.WorkerThreadCount(); }

    /// 提交一个 tile 构建（**非阻塞**）。`request` 由调用方 **move** 进来（其生命周期由本类负责）。
    void Submit(TerrainTileBuildRequest request);

    /// 取回一个**已完成**的结果；完成队列为空 ⇒ 返回 false（**不阻塞**）。
    /// 副作用（P6 收尾，2026-10-06）：在返回 false（= 本轮收包已弹空）的那一刻顺带回收已结束的任务对象，
    /// **一次/轮**（原先每次弹都重扫在飞列表 ⇒ O(队列长 × 在飞数)）。
    [[nodiscard]] bool TakeCompleted(TerrainTileBuildResult& out);

    /// **尚未回收**的任务数（含"已完成但结果还没被取走"的；调用时顺带回收已结束的任务对象）。
    [[nodiscard]] std::size_t PendingCount();

    /// 观测数据（面板 / 日志）：已完成 tile 数 + worker 侧计算耗时（累计 / 峰值，毫秒）。
    struct Stats {
        std::size_t completed    = 0;
        double      computeMsSum = 0.0;
        double      computeMsMax = 0.0;
    };
    [[nodiscard]] Stats SnapshotStats() const;

private:
    /// 一个在飞的构建：请求 + 任务句柄（任务必须存活到结束，见 `ParallelTask` 的生命周期约定）。
    struct Job {
        TerrainTileBuildRequest       request {};
        std::unique_ptr<ParallelTask> task;
    };

    /// 回收**已结束**的任务对象（任务结束 ⇒ 结果已经 push 或即将 push，请求不再被 worker 读取）。
    void ReapFinishedJobs();

    std::vector<MapEdit>    m_edits;    ///< 预设编辑的**副本**（worker 只读；主线程不再改动它）
    TerrainNoiseGenerator   m_noise;    ///< worker 侧自持的生成器（**不共用** `TerrainWorld::m_noise`）

    /// 地表数据来源（V4，非拥有；`nullptr` = 程序化生成）。worker **并发只读** ⇒ 实现须线程安全。
    const ITerrainTileSource* m_tileSource = nullptr;

    TaskScheduler m_scheduler;

    std::vector<std::unique_ptr<Job>> m_inFlight;  ///< 主线程独占（无需锁）

    std::mutex                     m_completedMutex;  ///< **只保护完成队列**（worker push / 主线程 pop）
    /// worker 产出 → 主线程消费。**用 `deque` 而不是 `vector`**（P6 收尾，2026-10-06）：主线程按 FIFO 弹空时
    /// `pop_front()` 是 O(1)，而 `vector::erase(begin())` 要搬移整队；元素含 8 KB `TerrainTile`，
    /// 且队列长度可达预取提前量上限（192）⇒ 搬移量按 MB 计。语义（FIFO）不变。
    std::deque<TerrainTileBuildResult> m_completed;

    Stats m_stats {};  ///< 只在主线程更新（`TakeCompleted` 时累加）
};

}  // namespace vx
