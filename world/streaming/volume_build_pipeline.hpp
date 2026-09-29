#pragma once

#include "core/task_scheduler.hpp"
#include "dig/dig_volume.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace vx {

/// 可挖体积块的**构建任务池**（T81 / [ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md)）。
///
/// 形态（业界标准：**Sodium chunk builder** / **UE5 Task Graph** / Unity Job System）：
///   - **主线程** `Submit(BlockBuildInput)`：只入队 + 起任务（**非阻塞**；主线程在这里只花了"采快照"的钱）；
///   - **worker**：跑**纯函数** `BuildBlockFromInput`（不读世界对象、不碰图形 API、不加锁），
///     把结果 push 进**完成队列**；
///   - **主线程** `TakeCompleted(BlockBuildResult&)`：**非阻塞**取回一个结果（交给 `DigVolumeWorld` 安装）。
///
/// 并发契约（`references/concurrency.md`）：
///   - **单写者 + 快照**：worker 独占自己的 `BlockBuildInput` / 输出；`m_inFlight` 只由主线程触碰；
///   - **唯一的互斥量只保护完成队列**（几行 push / pop），**不保护任何世界数据**（红线 9）；
///   - **GPU 上传仍在渲染线程**（本模块完全不碰渲染器）。
///
/// 线程约定：`Submit` / `TakeCompleted` / `PendingCount` / `Stats` **只在主线程调用**；
/// `~VolumeBuildPipeline` 会先等待全部任务结束（安全停机）。
class VolumeBuildPipeline final {
public:
    /// `workerThreads == 0` ⇒ 自动（硬件并发 − 1）。线程池不可用时**自动回落**为"提交即同步算完"
    /// ——结果不变，只是尖峰回到引入本模块之前的样子（`TaskScheduler` 会 WARN 一次，不静默）。
    explicit VolumeBuildPipeline(unsigned workerThreads = 0);
    ~VolumeBuildPipeline();

    VolumeBuildPipeline(const VolumeBuildPipeline&) = delete;
    VolumeBuildPipeline& operator=(const VolumeBuildPipeline&) = delete;
    VolumeBuildPipeline(VolumeBuildPipeline&&) = delete;
    VolumeBuildPipeline& operator=(VolumeBuildPipeline&&) = delete;

    /// 是否有真实 worker（false ⇒ 同步回退路径）。供日志 / 面板显示。
    [[nodiscard]] bool     HasWorkers() const noexcept { return m_scheduler.Valid(); }
    [[nodiscard]] unsigned WorkerThreadCount() const noexcept { return m_scheduler.WorkerThreadCount(); }

    /// 提交一个块的构建（**非阻塞**）。`input` 由调用方 **move** 进来（其生命周期由本类负责）。
    void Submit(BlockBuildInput input);

    /// 取回一个**已完成**的结果；完成队列为空 ⇒ 返回 false（**不阻塞**）。
    [[nodiscard]] bool TakeCompleted(BlockBuildResult& out);

    /// **尚未回收**的任务数（含"已完成但结果还没被取走"的；调用时顺带回收已结束的任务对象）。
    [[nodiscard]] std::size_t PendingCount();

    /// 观测数据（面板 / 日志）：已完成块数 + worker 侧计算耗时（累计 / 峰值，毫秒）。
    struct Stats {
        std::size_t completed    = 0;
        double      computeMsSum = 0.0;
        double      computeMsMax = 0.0;
    };
    [[nodiscard]] Stats SnapshotStats() const;

private:
    /// 一个在飞的块构建：输入 + 任务句柄（任务必须存活到结束，见 `ParallelTask` 的生命周期约定）。
    struct Job {
        BlockBuildInput               input {};
        std::unique_ptr<ParallelTask> task;
    };

    /// 回收**已结束**的任务对象（任务结束 ⇒ 结果已经 push 或即将 push，输入不再被 worker 读取）。
    void ReapFinishedJobs();

    TaskScheduler m_scheduler;

    std::vector<std::unique_ptr<Job>> m_inFlight;  ///< 主线程独占（无需锁）

    std::mutex                  m_completedMutex;  ///< **只保护完成队列**（worker push / 主线程 pop）
    std::vector<BlockBuildResult> m_completed;     ///< worker 产出 → 主线程消费

    Stats m_stats {};  ///< 只在主线程更新（`TakeCompleted` 时累加）
};

}  // namespace vx
