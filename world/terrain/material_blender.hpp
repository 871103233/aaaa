#pragma once

#include "terrain/material_table.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace vx {

/// 按「高度 + 坡度」计算 N 个材质槽位的 splat 权重，并归一化到和为 1（ADR 0004 / 方案 §4.3）。
///
/// 纯函数：不依赖全局状态、不分配、不抛异常。
/// 模型：每个槽位的原始权重 = `heightFactor * slopeFactor`，两个因子分别是高度带与坡度带的
/// 平滑隶属度；若总和过小（无槽位匹配），则整体退化为「槽位 0 权重 1」，保证权重非零且和为 1。
///
/// 前置条件：`slope` 应为 `1 - normal.y`；越界值会被钳制到 `[0, 1]`。
[[nodiscard]] std::array<float, static_cast<std::size_t>(kMaterialSlotCount)>
ComputeBlendWeights(const TerrainMaterialTable& table, float heightBlocks, float slope) noexcept;

/// 材质混合器：在纯函数之上叠加**确定性噪声抖动**，让材质过渡带不是一条完美直线。
///
/// 抖动只扰动参与高度带判断的高度值，不改变世界几何；噪声种子由全局种子派生（`SplitMix64`），
/// 因此同一构建 + 同一种子必然得到同样结果（red line 7）。
/// 线程约定：`WeightsAt` 为 `const` 且不写成员，可从多个网格化线程并发调用。
class MaterialBlender {
public:
    explicit MaterialBlender(std::uint64_t worldSeed);
    ~MaterialBlender();

    MaterialBlender(const MaterialBlender&) = delete;
    MaterialBlender& operator=(const MaterialBlender&) = delete;
    MaterialBlender(MaterialBlender&&) noexcept;
    MaterialBlender& operator=(MaterialBlender&&) noexcept;

    /// 计算 `(worldX, worldZ)` 处、地面高度 `heightBlocks`、坡度 `slope` 的归一化 splat 权重。
    [[nodiscard]] std::array<float, static_cast<std::size_t>(kMaterialSlotCount)>
    WeightsAt(const TerrainMaterialTable& table, float worldX, float worldZ, float heightBlocks,
              float slope) const noexcept;

private:
    struct Impl;  ///< 隐藏 FastNoiseLite 类型，避免第三方头文件进入公共头。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
