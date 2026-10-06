#pragma once

#include "core/task_scheduler.hpp"
#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "shell/surface_shell.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vx {

class RiverCarveField;
class PreparedMeshShape;  ///< W7-S3b①：worker 侧预构建的三角网形状（不透明；见 `physics/mesh_shape_prepare.hpp`）

/// 一个地表壳块的**构建请求**（worker 任务的输入，纯数据；不含任何世界状态）。
///
/// `region` 是**快照**：壳密度依赖区域边界（`EdgeFade`）⇒ 区域一动，边界淡出圈的块内容就变，
/// 因此每次构建都必须带上"当时的目标区域"，由 worker 独立使用（不读任何可变状态）。
struct ShellBlockBuildRequest {
    BlockCoord        block {};
    SurfaceShellRegion region {};
};

/// 一个地表壳块的**构建结果**（worker 产出 → 主线程收包）。
struct ShellBlockBuildResult {
    BlockCoord         block {};
    SurfaceShellRegion region {};
    MeshData           mesh {};
    /// W7-S3b①：worker 侧**预构建**的三角网形状（`JPH::MeshShape` 的 BVH 构建 ≈ 16 ms/块）。
    /// 主线程只做廉价的"加体"。空网格 ⇒ `nullptr`（无碰撞体）。
    std::shared_ptr<PreparedMeshShape> shape;
    double                             computeMs = 0.0;
};

/// 地表壳块的**构建任务池**（W7-S3b① / [ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md) 形态）。
///
/// 形态与 `TerrainTileBuildPipeline` 同源（业界标准：**Sodium chunk builder** / **UE5 Task Graph** /
/// Unity Job System）：
///   - 主线程 `Submit(request)`：只入队 + 起任务（**非阻塞**）；
///   - worker：跑**纯函数** `BuildShellBlockMesh`（自持一份 `TerrainNoiseGenerator`，不读 `TerrainWorld`、
///     不碰图形 API、不加锁）；
///   - 主线程 `TakeCompleted(result)`：非阻塞取回，再做「GPU 上传 + 加碰撞体」。
///
/// **河道下切场（W6）**：以 `const RiverCarveField*` 传入（可为 `nullptr`）；`CarveAt` 是 `const noexcept`
/// 只读查询 ⇒ 多 worker 并发只读安全；其生命周期必须覆盖本对象。
///
/// 线程约定：`Submit` / `TakeCompleted` / `PendingCount` / `Stats` **只在主线程调用**；
/// 析构会先等待全部任务结束（安全停机）。
class ShellBlockBuildPipeline final {
public:
    /// `workerThreads == 0` ⇒ 自动（硬件并发 − 1）。线程池不可用时**自动回落**为"提交即同步算完"
    /// （结果不变，只是尖峰回到引入本模块之前；`TaskScheduler` 会 WARN 一次，不静默）。
    ShellBlockBuildPipeline(std::uint64_t worldSeed, TerrainGenerationParams params, SurfaceShellParams shellParams,
                            const RiverCarveField* river = nullptr, unsigned workerThreads = 0);
    ~ShellBlockBuildPipeline();

    ShellBlockBuildPipeline(const ShellBlockBuildPipeline&) = delete;
    ShellBlockBuildPipeline& operator=(const ShellBlockBuildPipeline&) = delete;
    ShellBlockBuildPipeline(ShellBlockBuildPipeline&&) = delete;
    ShellBlockBuildPipeline& operator=(ShellBlockBuildPipeline&&) = delete;

    [[nodiscard]] bool     HasWorkers() const noexcept { return m_scheduler.Valid(); }
    [[nodiscard]] unsigned WorkerThreadCount() const noexcept { return m_scheduler.WorkerThreadCount(); }

    /// 提交一个壳块构建（**非阻塞**）。
    void Submit(ShellBlockBuildRequest request);

    /// 取回一个**已完成**的结果；完成队列为空 ⇒ 返回 false（**不阻塞**）。
    [[nodiscard]] bool TakeCompleted(ShellBlockBuildResult& out);

    /// **尚未回收**的任务数（含"已完成但结果还没被取走"的）。
    [[nodiscard]] std::size_t PendingCount();

    /// 观测数据（面板 / 日志）：已完成块数 + worker 侧计算耗时（累计 / 峰值，毫秒）。
    struct Stats {
        std::size_t completed    = 0;
        double      computeMsSum = 0.0;
        double      computeMsMax = 0.0;
    };
    [[nodiscard]] Stats SnapshotStats() const;

private:
    struct Job {
        ShellBlockBuildRequest        request {};
        std::unique_ptr<ParallelTask> task;
    };

    void ReapFinishedJobs();

    TerrainNoiseGenerator    m_noise;       ///< worker 侧自持（不共用调用方的生成器）
    TerrainGenerationParams  m_params;
    SurfaceShellParams       m_shellParams;
    const RiverCarveField*   m_river = nullptr;

    TaskScheduler m_scheduler;

    std::vector<std::unique_ptr<Job>> m_inFlight;  ///< 主线程独占（无需锁）

    std::mutex                         m_completedMutex;  ///< **只保护完成队列**
    std::vector<ShellBlockBuildResult> m_completed;

    Stats m_stats {};  ///< 只在主线程更新（`TakeCompleted` 时累加）
};

}  // namespace vx
