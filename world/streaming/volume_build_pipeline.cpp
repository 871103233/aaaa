#include "streaming/volume_build_pipeline.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <utility>

namespace vx {

VolumeBuildPipeline::VolumeBuildPipeline(unsigned workerThreads) : m_scheduler(workerThreads) {}

VolumeBuildPipeline::~VolumeBuildPipeline() {
    // 安全停机：先等全部任务结束，再析构 `Job`（否则 worker 可能仍在读被释放的 `input`）。
    m_scheduler.WaitForAll();
    m_inFlight.clear();
}

void VolumeBuildPipeline::Submit(BlockBuildInput input) {
    auto job   = std::make_unique<Job>();
    job->input = std::move(input);
    Job* raw   = job.get();

    // 任务体：**纯函数** + 结果入队（唯一的临界区只保护队列）。
    job->task = std::make_unique<ParallelTask>(1U, [this, raw](std::size_t, std::size_t) {
        BlockBuildResult result = BuildBlockFromInput(raw->input);
        {
            const std::lock_guard<std::mutex> lock(m_completedMutex);
            m_completed.push_back(std::move(result));
        }
    });

    m_inFlight.push_back(std::move(job));
    // 线程池不可用时 `Submit` 会**同步**执行 ⇒ 结果已在完成队列里，主线程随后的收包会取到它。
    m_scheduler.Submit(*m_inFlight.back()->task);
}

void VolumeBuildPipeline::ReapFinishedJobs() {
    const auto finished = [](const std::unique_ptr<Job>& job) { return job->task->IsComplete(); };
    m_inFlight.erase(std::remove_if(m_inFlight.begin(), m_inFlight.end(), finished), m_inFlight.end());
}

bool VolumeBuildPipeline::TakeCompleted(BlockBuildResult& out) {
    // 先回收：任务结束 ⇒ 它的结果已经（或正在）push，输入不再被读取 ⇒ 可以释放 Job。
    ReapFinishedJobs();

    BlockBuildResult result;
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

std::size_t VolumeBuildPipeline::PendingCount() {
    ReapFinishedJobs();
    return m_inFlight.size();
}

VolumeBuildPipeline::Stats VolumeBuildPipeline::SnapshotStats() const {
    return m_stats;
}

}  // namespace vx
