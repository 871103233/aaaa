#include "core/task_scheduler.hpp"

#include "core/log.hpp"

// enkits 只在本文件内出现（SKILL §2：需要预编译的第三方库只在平台层或引擎核心的薄封装中直接引用）。
// vcpkg 端口名 = `enkits`，CMake 目标 = `enkiTS::enkiTS`，头文件目录 = `include/enkiTS/`（实测 1.12）。
#include <enkiTS/TaskScheduler.h>

#include <algorithm>
#include <thread>
#include <utility>

namespace vx {

// ---------------------------------------------------------------------------
// ParallelTask
// ---------------------------------------------------------------------------

struct ParallelTask::Impl {
    /// 我们自己的任务体副本（**回退路径**直接调用它；不需要依赖 enkiTS 内部成员的可访问性）。
    std::function<void(std::size_t, std::size_t)> work;

    /// enkiTS 的分片任务。**不子类化**：直接用它的 `TaskSet` 构造重载
    /// （`TaskSet(setSize, TaskSetFunction)`，`TaskSetFunction = std::function<void(TaskSetPartition, uint32_t)>`），
    /// 把 `work(begin, end)` 适配成"区间 + 线程号"。
    /// 注意：`work` 必须先于 `taskSet` 声明（下面的 lambda 用 `this->work`，构造顺序即声明顺序）。
    enki::TaskSet taskSet;

    Impl(std::size_t items, std::function<void(std::size_t, std::size_t)> workFunction)
        : work(std::move(workFunction)),
          taskSet(static_cast<std::uint32_t>(items),
                  [this](enki::TaskSetPartition range, std::uint32_t threadnum) {
                      static_cast<void>(threadnum);  // 分区方式由调度器决定；调用方不得依赖线程号
                      work(static_cast<std::size_t>(range.start), static_cast<std::size_t>(range.end));
                  }) {}
};

ParallelTask::ParallelTask(std::size_t items, std::function<void(std::size_t, std::size_t)> work)
    : m_impl(std::make_unique<Impl>(std::max<std::size_t>(1U, items), std::move(work))) {}

ParallelTask::~ParallelTask() = default;

bool ParallelTask::IsComplete() const noexcept {
    return m_impl->taskSet.GetIsComplete();
}

// ---------------------------------------------------------------------------
// TaskScheduler
// ---------------------------------------------------------------------------

struct TaskScheduler::Impl {
    enki::TaskScheduler scheduler;
};

TaskScheduler::TaskScheduler(unsigned threadCount) {
    // 总线程数 = worker + 1（调用 `Initialize` 的线程 = thread 0 = 主 / 渲染线程）。
    unsigned total = (threadCount == 0U) ? std::thread::hardware_concurrency() : threadCount;
    if (total == 0U) {
        total = 1U;  // 硬件并发取不到 ⇒ 退化为"只有主线程"
    }
    if (total < 2U) {
        // 只有主线程 ⇒ 没有 worker 可用：**视为不可用**，调用方据此走同步回退并 WARN（不静默降级）。
        VX_LOG_WARN("任务调度器不可用：可用线程数 %u < 2 ⇒ 本进程不创建 worker（相关任务将同步执行）", total);
        return;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->scheduler.Initialize(total);
    m_workerThreads = total - 1U;
    m_valid         = true;
    VX_LOG_INFO("任务调度器已启用（enkits）：worker 线程 %u 个，主线程为 thread 0（不被征用跑长任务）",
                m_workerThreads);
}

TaskScheduler::~TaskScheduler() {
    if (!m_valid) {
        return;
    }
    // 安全停机：等全部任务结束，避免 worker 继续向即将析构的对象写数据。
    m_impl->scheduler.WaitforAll();
}

void TaskScheduler::Submit(ParallelTask& task) {
    if (!m_valid) {
        // **回退路径**：没有 worker ⇒ 在当前线程同步跑完（结果不变，只是不再在后台）。
        // 这里刻意不再 WARN（构造时已 WARN 过一次，逐任务刷屏没有新信息）。
        task.m_impl->work(0U, 1U);
        return;
    }
    m_impl->scheduler.AddTaskSetToPipe(&task.m_impl->taskSet);
}

void TaskScheduler::WaitFor(ParallelTask& task) {
    if (task.IsComplete()) {
        return;
    }
    if (!m_valid) {
        return;  // 回退路径下 `Submit` 是同步的 ⇒ 到这里必然已完成
    }
    // enkiTS 的 `WaitforAll` 会等待全部在飞任务（含本任务）——本接口只用于停机 / 单测，语义足够。
    m_impl->scheduler.WaitforAll();
}

void TaskScheduler::WaitForAll() {
    if (!m_valid) {
        return;
    }
    m_impl->scheduler.WaitforAll();
}

}  // namespace vx
