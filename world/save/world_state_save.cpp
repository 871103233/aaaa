// 世界状态差量**会话**与**异步写盘器**的实现（阶段 V0.10 S4；
// [ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策四 / 五）。
//
// 口径（两处关键取舍）：
//   1. **会话只认字节**：`WorldStateSave` 存的是"逐块原始载荷"，不依赖地形 / 体积类型 ⇒ 可单测、与采集解耦；
//   2. **卸载不阻塞磁盘**（决策四 vs 决策五的交集）：脏单元**卸载前**先把差量采集进会话（改动已离开该单元、
//      **不会丢**），真正的写盘走 worker 异步 ⇒ 主线程**绝不**阻塞在磁盘上（SKILL 第四节）。

#include "save/world_state_save.hpp"

#include "core/task_scheduler.hpp"  // 异步写盘（world → engine，允许方向）

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace vx {

// ---------------------------------------------------------------------------
// WorldStateSave
// ---------------------------------------------------------------------------

void WorldStateSave::SetChunk(const WorldSaveChunkKey& key, std::vector<std::uint8_t> raw) {
    if (raw.empty()) {
        EraseChunk(key);
        return;
    }
    m_chunks[key] = std::move(raw);  // S1 的 `WriteToFile` 会拒空块 ⇒ 这里已挡住
}

void WorldStateSave::EraseChunk(const WorldSaveChunkKey& key) noexcept {
    (void)m_chunks.erase(key);
}

const std::vector<std::uint8_t>* WorldStateSave::FindChunk(const WorldSaveChunkKey& key) const noexcept {
    const auto found = m_chunks.find(key);
    return (found == m_chunks.end()) ? nullptr : &found->second;
}

void WorldStateSave::WriteToFile(const std::filesystem::path& path) const {
    WorldSaveWriter writer;
    writer.SetHeader(m_header);
    for (const auto& entry : m_chunks) {
        writer.SetChunk(entry.first, entry.second);
    }
    writer.WriteToFile(path);  // 临时文件 + rename 原子替换；失败抛（调用方报错并保留上一份好档）
}

WorldStateSave WorldStateSave::LoadFromFile(const std::filesystem::path& path, bool& found) {
    WorldStateSave save;
    std::error_code error;
    const bool      exists = std::filesystem::exists(path, error);
    if (error || !exists) {
        found = false;  // 首次运行 / 无可读档：**不报错**（与 `LoadWorldInstanceSave` 同口径）
        return save;
    }

    const WorldSaveReader reader = WorldSaveReader::Open(path);  // 校验失败（版本 / 校验和 / 索引）⇒ 抛
    save.m_header                = reader.Header();
    for (const WorldSaveChunkKey& key : reader.Keys()) {
        save.m_chunks.emplace(key, reader.ReadChunk(key));
    }
    found        = true;
    return save;
}

// ---------------------------------------------------------------------------
// WorldSaveFlusher
// ---------------------------------------------------------------------------

struct WorldSaveFlusher::Impl {
    /// 一次在飞写盘：快照 + 目标路径 + 任务句柄（任务必须存活到结束，见 `ParallelTask` 的生命周期约定）。
    struct Job {
        WorldStateSave                snapshot;
        std::filesystem::path         path;
        std::unique_ptr<ParallelTask> task;
        std::string                   error;  ///< 失败原因（空 = 成功）；由 worker 写、主线程在完成后读
    };

    explicit Impl(unsigned workerThreads) : scheduler(workerThreads) {}

    TaskScheduler        scheduler;
    std::unique_ptr<Job> job;  ///< **主线程独占**（不排队：同一时刻至多一个在飞）
};

WorldSaveFlusher::WorldSaveFlusher(unsigned workerThreads) : m_impl(std::make_unique<Impl>(workerThreads)) {}

WorldSaveFlusher::~WorldSaveFlusher() {
    // 安全停机：等在飞写盘结束（结果不重要 ⇒ 丢弃错误）。`TaskScheduler` 析构亦会等待，这里显式等一次。
    if (m_impl->job != nullptr) {
        m_impl->scheduler.WaitFor(*m_impl->job->task);
        m_impl->job.reset();
    }
}

bool WorldSaveFlusher::HasWorkers() const noexcept { return m_impl->scheduler.Valid(); }

bool WorldSaveFlusher::Submit(WorldStateSave snapshot, std::filesystem::path path) {
    if (m_impl->job != nullptr) {
        return false;  // 已有在飞 ⇒ 不入队（调用方下轮再试；避免快照堆积）
    }
    auto    job  = std::make_unique<Impl::Job>();
    Impl::Job* raw = job.get();
    job->snapshot  = std::move(snapshot);
    job->path      = std::move(path);
    job->task      = std::make_unique<ParallelTask>(1U, [raw](std::size_t, std::size_t) {
        try {
            raw->snapshot.WriteToFile(raw->path);
        } catch (const std::exception& failure) {
            raw->error = failure.what();
        } catch (...) {
            raw->error = "未知异常（写盘失败）";
        }
        // `TaskScheduler` 的任务体返回后以 `acq_rel` 递减完成计数 ⇒ 主线程在 `IsComplete() == true` 之后
        // 读到的 `error` 必然是本次写入的结果（与既有构建管线同口径）。
    });
    m_impl->job = std::move(job);
    // 线程池不可用时 `TaskScheduler::Submit` **同步执行**（此时 `IsComplete()` 恒为 true，见 `ICompletable`）
    // ⇒ `Busy()` / `Poll()` 语义在两个形态下一致。
    m_impl->scheduler.Submit(*m_impl->job->task);
    return true;
}

bool WorldSaveFlusher::Busy() const noexcept {
    return m_impl->job != nullptr && !m_impl->job->task->IsComplete();
}

bool WorldSaveFlusher::Poll(std::string& outError) {
    if (m_impl->job == nullptr || !m_impl->job->task->IsComplete()) {
        return false;
    }
    outError = m_impl->job->error;  // 空 = 成功
    m_impl->job.reset();            // 任务已完成 ⇒ 可安全回收（`ParallelTask` 的生命周期约定）
    return true;
}

void WorldSaveFlusher::WaitForIdle(std::string& outError) {
    if (m_impl->job == nullptr) {
        outError.clear();
        return;
    }
    m_impl->scheduler.WaitFor(*m_impl->job->task);
    outError = m_impl->job->error;  // 空 = 成功
    m_impl->job.reset();
}

}  // namespace vx
