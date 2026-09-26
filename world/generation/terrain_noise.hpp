#pragma once

#include "terrain/terrain_types.hpp"

#include <cstdint>
#include <memory>

namespace vx {

/// 多层噪声地表生成器：把 `(全局种子, 整数世界坐标)` 映射成一列高度。
///
/// 这是"世界重建"的**唯一**生成入口，必须是纯函数（方案 §3.2、红线 7 / 15）：
///   - 噪声库为 FastNoiseLite（`third_party/`，MIT，vendored）；
///   - 多层叠加：低频起伏 + 中频细节 + 高频粗糙，各自的种子由 `SplitMix64` 从全局种子派生；
///   - **禁止**依赖伪随机数发生器 / 时间 / 线程顺序 / 容器迭代顺序。
///
/// 交付精度：返回 `int16` 定点（1/16 格），已钳制到世界垂直范围 `0 ~ 512` 格（ADR 0008）。
/// 相邻 tile 在同一世界列上调用本函数会得到**同一个定点值**，这是拼接无裂缝的前提。
///
/// 前置条件：构造参数 `worldSeed` 即全局世界种子。
/// 线程约定：`HeightUnits` / `VariationAt` 为 `const` 且不写成员，可从多个生成线程并发调用。
class TerrainNoiseGenerator {
public:
    explicit TerrainNoiseGenerator(std::uint64_t worldSeed);
    ~TerrainNoiseGenerator();

    TerrainNoiseGenerator(const TerrainNoiseGenerator&) = delete;
    TerrainNoiseGenerator& operator=(const TerrainNoiseGenerator&) = delete;
    TerrainNoiseGenerator(TerrainNoiseGenerator&&) noexcept;
    TerrainNoiseGenerator& operator=(TerrainNoiseGenerator&&) noexcept;

    /// 世界整数列坐标 `(worldX, worldZ)` 处的一列高度（1/16 格定点，已钳制）。
    [[nodiscard]] Height HeightUnits(std::int64_t worldX, std::int64_t worldZ) const noexcept;

    /// 确定性噪声抖动，值域 `[-1, 1]`。
    /// 用途：让材质过渡带不是一条完美直线（T5）；与高度使用不同通道种子，故不受地形起伏直接影响。
    [[nodiscard]] float VariationAt(std::int64_t worldX, std::int64_t worldZ) const noexcept;

private:
    struct Impl;                        ///< 隐藏 FastNoiseLite 类型，避免第三方头文件进入公共头。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
