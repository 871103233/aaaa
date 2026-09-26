#pragma once

#include "terrain/material_table.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace vx {

/// 一条「带」的隶属度：带内为 1，带外经 `blend` 宽的**窄带**平滑阶跃归零（ADR 0009）。
///
/// 这是材质过渡的**唯一**曲线定义，CPU 与 GPU 共用同一公式：
///   - 实现内 `ComputeBlendWeights` 调用本函数；
///   - `assets/shaders/mesh.frag` 的 `bandFactor()` 逐字镜像本函数（逐像素调用）。
///
/// 过渡带宽由 `blend` 决定（两侧各 `blend` 宽），因此远窄于整段 [min, max]；
/// 片元着色器逐像素求值后，边界宽度只受几何局部曲率限制，不再受 1 格顶点间距摊开。
///
/// 前置条件：`min ≤ max`；`blend ≤ 0` 表示硬边界。
[[nodiscard]] float MaterialBandFactor(float value, float min, float max, float blend) noexcept;

/// 按「高度 + 坡度」计算 N 个材质槽位的 splat 权重，并归一化到和为 1（ADR 0004 / 0009 / 方案 §4.3）。
///
/// 纯函数：不依赖全局状态、不分配、不抛异常。
/// 模型：每个槽位的原始权重 = `heightFactor * slopeFactor`，两个因子分别是高度带与坡度带的
/// 平滑隶属度（见 `MaterialBandFactor`）；若总和过小（无槽位匹配），则整体退化为「槽位 0 权重 1」，
/// 保证权重非零且和为 1。
///
/// `assets/shaders/mesh.frag` 的 `computeWeights()` 镜像本函数；两边改动必须同步。
///
/// 前置条件：`slope` 应为 `1 - normal.y`；越界值会被钳制到 `[0, 1]`。
[[nodiscard]] std::array<float, static_cast<std::size_t>(kMaterialSlotCount)>
ComputeBlendWeights(const TerrainMaterialTable& table, float heightBlocks, float slope) noexcept;

/// 平面 ↔ 三平面的**自动混合权重**（C 项 / 陡壁 UV 拉伸修复）。
///
/// 输入 `slope = 1 - |N.y|`（0 = 水平面、1 = 竖直面，由**世界空间几何法线**逐像素得出）。
/// 返回 `smoothstep(slopeMin, slopeMax, slope)`，并受 `settings.enabled` 门控（false → 恒为 0）。
///   0   → 纯平面投影（平地路径，采样次数与旧版完全相同）；
///   1   → 完全三平面；中间为平滑过渡。
///
/// **为什么必须逐像素由法线算**：地形会被笔刷挖与堆。只有"权重随几何法线实时变化"才能做到
/// **法线一变、混合自动跟随**，无需任何 CPU 侧预烘焙 / 按 tile 分支 / 重建网格等额外动作。
///
/// 这是 CPU 侧纯函数（不读全局、不分配），与 `assets/shaders/mesh.frag` 的 `triplanarWeight()` 逐字镜像；
/// 两边改动必须同步。前置条件：`settings` 已通过校验（`0 ≤ slopeMin < slopeMax ≤ 1`、`sharpness > 0`）。
[[nodiscard]] float TriplanarBlendWeight(float slope, const TriplanarSettings& settings) noexcept;

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
