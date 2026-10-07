#pragma once

#include "generation/terrain_params.hpp"
#include "terrain/terrain_types.hpp"

#include <cstdint>
#include <memory>

namespace vx {

/// 多层噪声地表生成器：把 `(全局种子, 整数世界坐标)` 映射成一列高度。
///
/// 这是"世界重建"的**唯一**生成入口，必须是纯函数（方案 §3.2、红线 7 / 15）：
///   - 噪声库为 FastNoiseLite（`third_party/`，MIT，vendored）；
///   - 多层叠加：低频起伏 + 中频细节 + 高频粗糙，各自的种子由 `SplitMix64` 从全局种子派生；
///   - **W3 起**再叠加一层**低频地貌掩罩**（山川 / 平原 / 丘陵）：按掩罩分档缩放地形幅度、平移基线
///     （参数与纯函数见 `generation/terrain_params.hpp`）；`landform.enabled == false` 时**不进入该路径**，
///     输出与引入本层之前**逐位一致**；
///   - **禁止**依赖伪随机数发生器 / 时间 / 线程顺序 / 容器迭代顺序。
///
/// 交付精度：返回 `int16` 定点（1/16 格），已钳制到世界垂直范围 `0 ~ 512` 格（ADR 0008）。
/// 相邻 tile 在同一世界列上调用本函数会得到**同一个定点值**，这是拼接无裂缝的前提。
///
/// 前置条件：构造参数 `worldSeed` 即全局世界种子；`params` 默认 = 引入 W3 之前的三层噪声数值。
/// 线程约定：`HeightUnits` / `VariationAt` 为 `const` 且不写成员，可从多个生成线程并发调用。
class TerrainNoiseGenerator {
public:
    explicit TerrainNoiseGenerator(std::uint64_t worldSeed,
                                   TerrainGenerationParams params = TerrainGenerationParams::Default());
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

    /// 世界整数列处的**地貌掩罩值**（归一化到 `[0, 1]`，纯函数）。
    ///
    /// 用途：内容放置与 W3 的统计验收（按 `ClassifyLandform` 判类）。
    /// 注意：`landform.enabled == false` 时该值**不参与**高度生成 —— 函数仍返回噪声值，仅用于诊断。
    [[nodiscard]] float LandformMaskAt(std::int64_t worldX, std::int64_t worldZ) const noexcept;

    /// **悬垂 3D 噪声**（值域约 `[-1, 1]`，纯函数；W4 地表体积壳使用）。
    ///
    /// 与 `HeightUnits` 使用**不同通道**，故不受地形起伏直接影响；参数见 `TerrainOverhangParams`
    /// （`assets/config/terrain.toml` 的 `[overhang]`）。
    [[nodiscard]] float OverhangAt(float worldX, float worldY, float worldZ) const noexcept;

    /// **洞穴雕刻量**（W5 地表体积壳使用；单位 = 格，值域 `[0, carveStrengthBlocks]`，纯函数）。
    ///
    /// 由两条**互不相关**的 3D 噪声 `a`、`b` 构成隧道网络：`sqrt(a² + b²) < tunnelRadius` 处返回
    /// `(radius − d) / radius × carveStrengthBlocks`，其余返回 `0`。**`caves.enabled == false` 时恒返回 0**
    /// ⇒ 地表壳的输出与 W4 逐位一致（既有测试与既有世界不受影响）。
    ///
    /// 参数见 `TerrainCaveParams`（`assets/config/terrain.toml` 的 `[caves]`）。
    [[nodiscard]] float CaveCarveAt(float worldX, float worldY, float worldZ) const noexcept;

    /// **河流抖动噪声**（值域约 `[-1, 1]`，纯函数；W6 河道走向的确定性域扭曲）。
    ///
    /// 与其它噪声层使用**不同通道**（`params.river.seedChannel`），故河道走向与地形 / 材质抖动互不耦合。
    [[nodiscard]] float RiverJitterAt(float worldX, float worldZ) const noexcept;

    /// **温度**（V0.6 C7；归一化到 `[0, 1]`，纯函数）。
    ///
    /// 与高度 / 地貌使用**不同通道**（`params.climate.temperatureSeedChannel`）⇒ 与高度图**解耦**。
    /// 只供**内容放置判据**（`PlacementRule` 的温度区间）使用，不参与地形生成。
    [[nodiscard]] float TemperatureAt(std::int64_t worldX, std::int64_t worldZ) const noexcept;

    /// **湿度**（V0.6 C7；归一化到 `[0, 1]`，纯函数）。与温度使用不同通道（互不相关）。
    [[nodiscard]] float HumidityAt(std::int64_t worldX, std::int64_t worldZ) const noexcept;

private:
    struct Impl;                        ///< 隐藏 FastNoiseLite 类型，避免第三方头文件进入公共头。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
