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
    // 先回收：任务结束 ⇒ 它的结果已经（或正在）push，请求不再被读取 ⇒ 可以释放 Job。
    ReapFinishedJobs();

    TerrainTileBuildResult result;
    {
        const std::lock_guard<std::mutex> lock(m_completedMutex);
        if (m_completed.empty()) {
            return false;
        }
        result = std::move(m_completed.front());
        m_completed.erase(m_completed.begin());  // 队列长度 ≤ 在飞任务数（数十量级）⇒ 搬移成本可忽略
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
