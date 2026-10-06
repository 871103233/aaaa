#include "streaming/shell_block_build_pipeline.hpp"

#include "core/clock.hpp"
#include "physics/mesh_shape_prepare.hpp"
#include "shell/surface_shell.hpp"
#include "water/river.hpp"

#include <algorithm>
#include <utility>

namespace vx {

ShellBlockBuildPipeline::ShellBlockBuildPipeline(std::uint64_t worldSeed, TerrainGenerationParams params,
                                                 SurfaceShellParams shellParams, const RiverCarveField* river,
                                                 unsigned workerThreads)
    : m_noise(worldSeed, params),
      m_params(std::move(params)),
      m_shellParams(shellParams),
      m_river(river),
      m_scheduler(workerThreads) {}

ShellBlockBuildPipeline::~ShellBlockBuildPipeline() {
    // 安全停机：先等全部任务结束，再析构 `Job`（否则 worker 可能仍在读被释放的 `request`）。
    m_scheduler.WaitForAll();
    m_inFlight.clear();
}

void ShellBlockBuildPipeline::Submit(ShellBlockBuildRequest request) {
    auto job     = std::make_unique<Job>();
    job->request = std::move(request);
    Job* raw     = job.get();

    job->task = std::make_unique<ParallelTask>(1U, [this, raw](std::size_t, std::size_t) {
        Clock computeClock;

        // **纯函数**：只用请求里的区域快照 + 自持生成器 + 只读河道场 ⇒ 不读世界、不加锁。
        MeshData mesh = BuildShellBlockMesh(m_noise, m_params, m_shellParams, raw->request.region,
                                            raw->request.block, m_river);

        ShellBlockBuildResult result;
        result.block  = raw->request.block;
        result.region = raw->request.region;
        if (!mesh.indices.empty()) {
            // worker 侧预构建三角网形状（BVH ≈ 16 ms/块）⇒ 渲染帧上只做廉价的"加体"（ADR 0024 修订）。
            result.shape = PrepareMeshShape(mesh);
        }
        result.mesh      = std::move(mesh);
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

void ShellBlockBuildPipeline::ReapFinishedJobs() {
    const auto finished = [](const std::unique_ptr<Job>& job) { return job->task->IsComplete(); };
    m_inFlight.erase(std::remove_if(m_inFlight.begin(), m_inFlight.end(), finished), m_inFlight.end());
}

bool ShellBlockBuildPipeline::TakeCompleted(ShellBlockBuildResult& out) {
    ReapFinishedJobs();

    ShellBlockBuildResult result;
    {
        const std::lock_guard<std::mutex> lock(m_completedMutex);
        if (m_completed.empty()) {
            return false;
        }
        result = std::move(m_completed.front());
        m_completed.erase(m_completed.begin());
    }

    m_stats.completed += 1U;
    m_stats.computeMsSum += result.computeMs;
    m_stats.computeMsMax = std::max(m_stats.computeMsMax, result.computeMs);
    out = std::move(result);
    return true;
}

std::size_t ShellBlockBuildPipeline::PendingCount() {
    ReapFinishedJobs();
    return m_inFlight.size();
}

ShellBlockBuildPipeline::Stats ShellBlockBuildPipeline::SnapshotStats() const {
    return m_stats;
}

}  // namespace vx
