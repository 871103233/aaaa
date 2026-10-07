#pragma once

#include "save/world_save.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vx {

/// **一次世界运行期内的世界状态差量会话**（阶段 V0.10 S4；[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策四）。
///
/// 形态 = **内存里的块表**（`WorldSaveChunkKey` → 逐块**原始未压缩载荷**），与 `.voxr` 的索引一一对应。
///
/// **为什么存"原始载荷字节"而不是地形 / 体积结构体**：本类**不依赖** `TerrainWorld` / `DigVolumeWorld`
/// ⇒ ① 可脱离 SDL / GPU 单测；② "世界层采集"与"落盘"彻底解耦（采集在调用方，落盘只认字节）。
///
/// **差量的内存表示就是它**（ADR 0037 决策四"不新开第二条记账"）：世界层的既有脏标记（地表脏 tile /
/// 体积脏块）在**卸载前**或**flush 时**被采集到这里；采集后即与单元解耦 ⇒ 单元可安全卸载。
///
/// 生命周期：**一个世界一轮**（`game/main.cpp` 的世界装载循环内创建 / 销毁）。
class WorldStateSave {
public:
    WorldStateSave() = default;

    void SetHeader(const WorldSaveHeader& header) noexcept { m_header = header; }
    [[nodiscard]] const WorldSaveHeader& Header() const noexcept { return m_header; }

    /// 记录 / 覆盖一个块。`raw` 为空 ⇒ 等价于 `EraseChunk`（"该块没有差量"）。
    void SetChunk(const WorldSaveChunkKey& key, std::vector<std::uint8_t> raw);

    /// 删除一个块（不存在 ⇒ 无操作，**不**算作未记录的变更 ⇒ 不置脏）。
    void EraseChunk(const WorldSaveChunkKey& key) noexcept;

    [[nodiscard]] const std::vector<std::uint8_t>* FindChunk(const WorldSaveChunkKey& key) const noexcept;
    [[nodiscard]] bool HasChunk(const WorldSaveChunkKey& key) const noexcept { return FindChunk(key) != nullptr; }
    [[nodiscard]] std::size_t ChunkCount() const noexcept { return m_chunks.size(); }
    [[nodiscard]] const std::map<WorldSaveChunkKey, std::vector<std::uint8_t>>& Chunks() const noexcept {
        return m_chunks;
    }

    /// 写盘（临时文件 + `rename` **原子替换**；失败抛）。
    void WriteToFile(const std::filesystem::path& path) const;

    /// 读盘。**文件不存在 ⇒ `found = false`**、返回空会话（首次运行，**不报错**）；
    /// 其余校验失败（魔数 / 版本 / 校验和 / 索引）由 `WorldSaveReader::Open` **抛**（不静默误读）。
    [[nodiscard]] static WorldStateSave LoadFromFile(const std::filesystem::path& path, bool& found);

private:
    WorldSaveHeader                                       m_header;
    std::map<WorldSaveChunkKey, std::vector<std::uint8_t>> m_chunks;
};

/// **异步写盘器**：把 `WorldStateSave` 的**快照**交给 worker，主线程只 `Poll` 收包
///（红线 2 / SKILL 第四节「主线程绝不阻塞在磁盘」）。
///
/// **为什么不复用既有构建池**：`TerrainTileBuildPipeline` / `VolumeBuildPipeline` 各自持有**私有**
/// `TaskScheduler`，且契约是"tile / 块构建" —— 借它们跑 IO 会让"任务归谁所有"变含糊。本类自持一个**最小池**
/// （缺省 1 个 worker）。线程池不可用时 `TaskScheduler::Submit` **同步执行**（结果不变，只是尖峰回到从前）。
///
/// 并发契约：同一时刻**只允许一个在飞写盘**（`Submit` 忙则返回 false，调用方下轮再试，**不排队堆积**）。
/// 生命周期：`~WorldSaveFlusher` **等待并回收**在飞任务（安全停机）。
class WorldSaveFlusher {
public:
    /// `workerThreads` = **总线程数**（含调用线程）；`2` ⇒ 主线程 + 1 worker。
    explicit WorldSaveFlusher(unsigned workerThreads = 2);
    ~WorldSaveFlusher();

    WorldSaveFlusher(const WorldSaveFlusher&) = delete;
    WorldSaveFlusher& operator=(const WorldSaveFlusher&) = delete;
    WorldSaveFlusher(WorldSaveFlusher&&) = delete;
    WorldSaveFlusher& operator=(WorldSaveFlusher&&) = delete;

    [[nodiscard]] bool HasWorkers() const noexcept;

    /// 提交一次写盘（**非阻塞**）。已有在飞 ⇒ 返回 false（**不**入队）。
    /// `snapshot` 由调用方 **move** 进来（须已在主线程拷好 ⇒ worker 只读、不碰世界 / 图形）。
    bool Submit(WorldStateSave snapshot, std::filesystem::path path);

    [[nodiscard]] bool Busy() const noexcept;

    /// **非阻塞**收包：有已完成的写盘 ⇒ 回收它、`outError` 写出失败原因（空 = 成功）并返回 true；否则 false。
    bool Poll(std::string& outError);

    /// **阻塞**等待在飞写盘结束（**只允许停机 / 退出前**调用；渲染帧内禁止）。语义同 `Poll` 的收包。
    void WaitForIdle(std::string& outError);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
