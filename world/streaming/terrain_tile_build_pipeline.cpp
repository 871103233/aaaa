#include "streaming/terrain_tile_build_pipeline.hpp"

#include "core/clock.hpp"
#include "terrain/terrain_world.hpp"  // `GenerateTerrainTileData`（纯自由函数）

#include <algorithm>
#include <utility>

namespace vx {

TerrainTileBuildPipeline::TerrainTileBuildPipeline(std::uint64_t worldSeed, TerrainGenerationParams params,
                                                   std::vector<MapEdit> edits, unsigned workerThreads,
                                                   const ITerrainTileSource* tileSource)
    : m_edits(std::move(edits)), m_noise(worldSeed, std::move(params)), m_tileSource(tileSource),
      m_scheduler(workerThreads) {}

TerrainTileBuildPipeline::~TerrainTileBuildPipeline() {
    // 安全停机：先等全部任务结束，再析构 `Job`（否则 worker 可能仍在读被释放的 `request`）。
    m_scheduler.WaitForAll();
    m_inFlight.clear();
}

void TerrainTileBuildPipeline::Submit(TerrainTileBuildRequest request) {
    auto  job   = std::make_unique<Job>();
    job->request = std::move(request);
    Job* raw    = job.get();

    // 任务体：**纯函数** + 结果入队（唯一的临界区只保护队列）。
    job->task = std::make_unique<ParallelTask>(1U, [this, raw](std::size_t, std::size_t) {
        Clock computeClock;  // 观测：worker 侧计算耗时（不影响任何判据）

        // 加载 = 生成 + 网格化；relod = 只用高度快照做网格化。两条都在 worker、都不碰世界。
        TerrainTile tile;
        if (raw->request.remeshOnly) {
            tile = raw->request.tile;
        } else {
            // V4：优先用**数据来源**（预制地图）；来源缺该 tile ⇒ 回退程序化生成（与主线程路径同一口径）。
            // 来源返回 true ⇒ tile 完整可用（含 `maxSurfaceBlocks`）⇒ 不再重复刷新。
            tile.coord                  = raw->request.coord;
            const bool filledFromSource = (m_tileSource != nullptr) && m_tileSource->FillTileHeights(tile);
            if (!filledFromSource) {
                tile = GenerateTerrainTileData(m_noise, m_edits, raw->request.coord.x, raw->request.coord.z);
            }
        }
        TerrainTileMesh mesh = BuildTerrainMesh(tile, /*quadFilter=*/nullptr, raw->request.lodLevel);

        TerrainTileBuildResult result;
        result.coord      = raw->request.coord;
        result.lodLevel   = raw->request.lodLevel;
        result.remeshOnly = raw->request.remeshOnly;
        result.mesh       = std::move(mesh);
        if (!raw->request.remeshOnly) {
            result.tile = std::move(tile);  // relod 不需要回传高度快照
        }
        result.computeMs = computeClock.Tick() * 1000.0;
        {
            const std::lock_guard<std::mutex> lock(m_completedMutex);
            m_completed.push_back(std::move(result));
        }
    });

    m_inFlight.push_back(std::move(job));
    // 线程池不可用时 `Submit` 会**同步**执行 ⇒ 结果已在完成队列里，主线程随后的收包会取到它。
    m_scheduler.Submit(*m_inFlight.back()->task);
}

void TerrainTileBuildPipeline::ReapFinishedJobs() {
    const auto finished = [](const std::unique_ptr<Job>& job) { return job->task->IsComplete(); };
    m_inFlight.erase(std::remove_if(m_inFlight.begin(), m_inFlight.end(), finished), m_inFlight.end());
}

bool TerrainTileBuildPipeline::TakeCompleted(TerrainTileBuildResult& out) {
    TerrainTileBuildResult result;
    bool                   have = false;
    {
        const std::lock_guard<std::mutex> lock(m_completedMutex);
        if (!m_completed.empty()) {
            result = std::move(m_completed.front());
            m_completed.pop_front();  // P6 收尾：O(1)（原先 `erase(begin())` 要搬移整队；元素含 8 KB `TerrainTile`）
            have = true;
        }
    }
    if (!have) {
        // P6 收尾（2026-10-06）：**只在"本轮收包已弹空"时回收一次**。
        // 为什么：`drainTerrainTileBuilds` 一帧内**多次**调用本函数把队列弹空，而原先**每次**都重扫在飞列表
        // ⇒ 成本 = O(队列长 × 在飞数)；预取提前量上限 192 ⇒ 该乘积可达 ≈3.7 万次扫描/帧（实测 `收包` 段
        // 均值 3.4 / 峰值 11.9 ms）。放到"弹空"这一刻 ⇒ 一次/帧。
        // 语义不变：任务结束 ⇒ 结果已 push ⇒ `Job` 可释放（延迟到同一帧的收包收尾，仍在本帧内）。
        // 刻意放在锁外：`ReapFinishedJobs` 只碰 `m_inFlight`（主线程独占）⇒ 不必占着完成队列的锁。
        ReapFinishedJobs();
        return false;
    }

    m_stats.completed += 1U;
    m_stats.computeMsSum += result.computeMs;
    m_stats.computeMsMax = std::max(m_stats.computeMsMax, result.computeMs);
    out = std::move(result);
    return true;
}

std::size_t TerrainTileBuildPipeline::PendingCount() {
    ReapFinishedJobs();
    return m_inFlight.size();
}

TerrainTileBuildPipeline::Stats TerrainTileBuildPipeline::SnapshotStats() const {
    return m_stats;
}

}  // namespace vx
