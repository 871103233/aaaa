#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace vx {

/// 一个**可分片并行**的一次性任务（[ADR 0003](../../docs/adr/0003-task-scheduler-and-ecs.md) / T81）。
///
/// 语义：把 `[0, items)` 切成若干区间，`work(begin, end)` 在 worker 线程上被调用若干次（区间不重叠、
/// 并集恰好是 `[0, items)`）。**分片方式由调度器决定** ⇒ 调用方**不得**依赖"哪一段在哪个线程跑"，
/// 只允许依赖"每个元素恰好被处理一次"。
///
/// 线程约定（`references/concurrency.md` §1）：`work` 在 worker 上执行 ⇒
/// **禁止**触碰图形 API、禁止读写世界对象（只允许读入参、写自己独占的输出）。
///
/// 生命周期：**必须由调用方持有到 `IsComplete()` 为 true**（enkiTS 不拥有任务对象）。
/// 违反 ⇒ 未定义行为（调度器仍会向已析构的任务写计数）。
class ParallelTask final {
public:
    /// 前置条件：`items >= 1`；`work` 非空。`items == 1` ⇒ 单次调用 `work(0, 1)`。
    ParallelTask(std::size_t items, std::function<void(std::size_t, std::size_t)> work);

    /// 析构前**必须**已经完成（见生命周期说明）；析构本身不等待。
    ~ParallelTask();

    ParallelTask(const ParallelTask&) = delete;
    ParallelTask& operator=(const ParallelTask&) = delete;
    ParallelTask(ParallelTask&&) = delete;
    ParallelTask& operator=(ParallelTask&&) = delete;

    /// 是否已完成（**线程安全**：可在任意线程查询；由调度器内部的原子计数实现）。
    [[nodiscard]] bool IsComplete() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend class TaskScheduler;
};

/// **任务调度器**（[ADR 0003](../../docs/adr/0003-task-scheduler-and-ecs.md) 的 enkits 薄封装）。
///
/// 这是本工程**唯一**直接引用 enkits 的地方 —— 落点在引擎核心层（SKILL §2：需要预编译的第三方库
/// 只在平台层或引擎核心的薄封装中直接引用；`world/` / `game/` 只经本接口使用线程池）。
///
/// 线程模型（ADR 0003）：`Initialize` 创建 `threadCount − 1` 个 worker，**调用 `Initialize` 的线程
/// （= 主 / 渲染线程）是 thread 0**，它只在"显式等待"或"提交时的兜底执行"里跑任务
/// ⇒ 主线程不会被偷偷征用去跑长任务（这一点对"渲染帧不得被冻结"很重要）。
class TaskScheduler final {
public:
    /// `threadCount == 0` ⇒ 硬件并发 − 1（enkiTS 默认；下限 1）。`threadCount == 1` ⇒ 只有主线程
    /// （本类会把它视为**不可用**，调用方据此走同步回退）。
    explicit TaskScheduler(unsigned threadCount = 0);

    /// 析构会**等待全部任务完成**后再释放线程池（安全停机；不得在帧内调用）。
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    /// 线程池是否真的建起来了（`workerThreads >= 1`）。为 false 时调用方应走同步回退并 **WARN**。
    [[nodiscard]] bool Valid() const noexcept { return m_valid; }

    /// 实际创建的 worker 线程数（不含主线程）。
    [[nodiscard]] unsigned WorkerThreadCount() const noexcept { return m_workerThreads; }

    /// 提交一个任务（**非阻塞**）：立即返回，`task` 在 worker 上被执行。
    /// 前置条件：`task` 的生命周期覆盖到 `IsComplete()` 为 true（见 `ParallelTask`）。
    /// 若线程池不可用（`Valid() == false`），本函数**立即在当前线程同步执行**该任务（回退路径）。
    void Submit(ParallelTask& task);

    /// **阻塞**等待 `task` 完成（只在停机 / 单测路径调用；**不得**在渲染帧里调用）。
    void WaitFor(ParallelTask& task);

    /// **阻塞**等待全部在飞任务完成（停机路径专用）。析构前由调用方调用，保证没有任何任务还会
    /// 触碰即将销毁的对象（红线：异步任务持有对象时用引用计数 / 句柄保证生命周期）。
    void WaitForAll();

private:
    bool     m_valid         = false;
    unsigned m_workerThreads = 0;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
