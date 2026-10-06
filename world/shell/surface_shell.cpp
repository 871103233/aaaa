// 地表壳（世界表示 v2 层②）的密度场与网格化实现（W4 / ADR 0023）。
//
// 复用 `world/dig/volume_mesher.*` 的**同一套** Surface Nets（ADR 0007 / 0019）—— 它在此是通用等值面工具，
// 因此 `world/shell/` 依赖 `world/dig/volume_mesher`（仅该文件；不依赖挖掘的其它部分）。

#include "shell/surface_shell.hpp"

#include "water/river.hpp"  // W6：河道下切场（只用于 CarveAt 查询）

#include <algorithm>
#include <cmath>

namespace vx {
namespace {

/// 壳密度的**每格单位数**（区别于 `volume_mesher` 的 `kDensityUnitsPerBlock = 127`）。
///
/// 为什么不用 127：那是**可挖体积的 `int8` 存储口径**（1 格即饱和），对"贴着地形的壳"太激进 ——
/// 折叠（悬垂）处大量采样被钳到 ±127，相邻 cell 的棱交点被**拉到同一位置** ⇒ 产生**退化三角形**。
/// 壳是运行时浮点计算、不经 int8 存储，故取更小的尺度（饱和远在 ±4 格之外）⇒ 等值面插值保持线性、
/// 顶点不重合。**注意**：本值只影响插值的数值范围，不影响**符号**（几何形状由符号场与悬垂噪声决定）。
constexpr float kShellDensityUnitsPerBlock = 32.0F;

/// 边界淡出系数（`0` = 完全无悬垂，`1` = 全幅悬垂）：按到区域四边的**最近列距**线性淡出。
///
/// 目的：区域边界处壳面**退化为宏高度场** ⇒ 与周围地表无可见缝隙（层间过渡，ADR 0011 的精神）。
[[nodiscard]] float EdgeFade(int worldX, int worldZ, const SurfaceShellRegion& region, float fadeBlocks) noexcept {
    if (!(fadeBlocks > 0.0F)) {
        return 1.0F;
    }
    const float left = static_cast<float>(worldX - region.minColumnX);
    const float right = static_cast<float>(region.maxColumnX - worldX);
    const float near = static_cast<float>(worldZ - region.minColumnZ);
    const float far = static_cast<float>(region.maxColumnZ - worldZ);
    const float distance = std::min(std::min(left, right), std::min(near, far));
    return std::clamp(distance / fadeBlocks, 0.0F, 1.0F);
}

}  // namespace

bool ShellBlockTouchesRegion(const SurfaceShellRegion& region, int blockX, int blockZ) noexcept {
    const int minX = BlockOriginBlocks(blockX);
    const int minZ = BlockOriginBlocks(blockZ);
    return minX + kVolumeBlockSize > region.minColumnX && minX < region.maxColumnX &&
           minZ + kVolumeBlockSize > region.minColumnZ && minZ < region.maxColumnZ;
}

ShellBlockSpan ComputeShellBlockSpanY(const TerrainNoiseGenerator& noise, const SurfaceShellParams& params,
                                      int blockX, int blockZ) noexcept {
    const int originX = BlockOriginBlocks(blockX);
    const int originZ = BlockOriginBlocks(blockZ);

    // 多采一层（`<= kVolumeBlockSize`）：与相邻列块共享那一列 ⇒ 相邻列的 Y 范围不会突变。
    float minHeight = 1.0e9F;
    float maxHeight = -1.0e9F;
    for (int dz = 0; dz <= kVolumeBlockSize; ++dz) {
        for (int dx = 0; dx <= kVolumeBlockSize; ++dx) {
            const float height = HeightToBlocks(noise.HeightUnits(originX + dx, originZ + dz));
            minHeight          = std::min(minHeight, height);
            maxHeight          = std::max(maxHeight, height);
        }
    }

    const float    band = params.bandHalfThicknessBlocks;
    ShellBlockSpan span;
    span.minBlockY = static_cast<int>(std::floor((minHeight - band) / static_cast<float>(kVolumeBlockSize)));
    span.maxBlockY = static_cast<int>(std::floor((maxHeight + band) / static_cast<float>(kVolumeBlockSize)));
    return span;
}

ShellBlockSpan ComputeShellBlockSpanYCoarse(const TerrainNoiseGenerator& noise, const SurfaceShellParams& params,
                                            int blockX, int blockZ, int sampleStepBlocks,
                                            int marginHeightBlocks) noexcept {
    const int originX = BlockOriginBlocks(blockX);
    const int originZ = BlockOriginBlocks(blockZ);
    const int step    = (sampleStepBlocks > 0) ? sampleStepBlocks : 1;
    const float margin = (marginHeightBlocks > 0) ? static_cast<float>(marginHeightBlocks) : 0.0F;

    float minHeight = 1.0e9F;
    float maxHeight = -1.0e9F;
    // 采 `0, step, 2*step, ...` 并**始终**包含 `kVolumeBlockSize`（块右 / 上边界列）。
    for (int dz = 0; dz <= kVolumeBlockSize; dz += step) {
        for (int dx = 0; dx <= kVolumeBlockSize; dx += step) {
            const float height = HeightToBlocks(noise.HeightUnits(originX + dx, originZ + dz));
            minHeight          = std::min(minHeight, height);
            maxHeight          = std::max(maxHeight, height);
        }
    }
    const float heightRight = HeightToBlocks(noise.HeightUnits(originX + kVolumeBlockSize, originZ));
    const float heightTop   = HeightToBlocks(noise.HeightUnits(originX, originZ + kVolumeBlockSize));
    minHeight               = std::min(minHeight, std::min(heightRight, heightTop));
    maxHeight               = std::max(maxHeight, std::max(heightRight, heightTop));

    const float band = params.bandHalfThicknessBlocks;
    ShellBlockSpan span;
    span.minBlockY =
        static_cast<int>(std::floor((minHeight - band - margin) / static_cast<float>(kVolumeBlockSize)));
    span.maxBlockY =
        static_cast<int>(std::floor((maxHeight + band + margin) / static_cast<float>(kVolumeBlockSize)));
    return span;
}

SurfaceShellSampler::SurfaceShellSampler(const TerrainNoiseGenerator& noise,
                                         const TerrainGenerationParams& generation, const SurfaceShellParams& params,
                                         const SurfaceShellRegion& region, BlockCoord block,
                                         const RiverCarveField* river) noexcept
    : m_noise(noise),
      m_generation(generation),
      m_params(params),
      m_region(region),
      m_block(block),
      m_river(river) {}

float SurfaceShellSampler::Sample(int i, int j, int k) const {
    const int worldX = BlockOriginBlocks(m_block.x) + i;
    const int worldY = BlockOriginBlocks(m_block.y) + j;
    const int worldZ = BlockOriginBlocks(m_block.z) + k;

    // 有符号距离（格）：`< 0` = 地表以下（实心）。用**宏地表高度**（与高度场完全同源）。
    const float macroHeight    = HeightToBlocks(m_noise.HeightUnits(worldX, worldZ));
    const float signedDistance = static_cast<float>(worldY) - macroHeight;

    // 悬垂：3D 噪声按边界淡出后叠加；幅度 × 2π × 频率 > 1 时等值面折叠 ⇒ 悬垂 / 洞穴。
    const float fade = EdgeFade(worldX, worldZ, m_region, m_params.edgeFadeBlocks);
    const float overhang =
        (fade > 0.0F) ? m_noise.OverhangAt(static_cast<float>(worldX), static_cast<float>(worldY),
                                          static_cast<float>(worldZ)) *
                            m_generation.overhang.amplitudeBlocks * fade
                      : 0.0F;

    // 洞穴网络（W5）：隧道雕刻量按**边界淡出**（与悬垂同源 ⇒ 区域边界处不出现半截洞口）与
    // **深度淡出**（壳底附近衰减到 0 ⇒ 隧道不穿出壳的可视边界）后叠加。
    // `CaveCarveAt` 在 `caves.enabled == false` 时恒 0 ⇒ 本行不改变 W4 的输出（逐位一致）。
    float carve = 0.0F;
    if (fade > 0.0F) {
        const float depthFadeBlocks = m_generation.caves.depthFadeBlocks;
        const float depthFade =
            (depthFadeBlocks > 0.0F)
                ? std::clamp((signedDistance + m_params.bandHalfThicknessBlocks) / depthFadeBlocks, 0.0F, 1.0F)
                : 1.0F;
        carve = m_noise.CaveCarveAt(static_cast<float>(worldX), static_cast<float>(worldY),
                                    static_cast<float>(worldZ)) *
                fade * depthFade;
    }

    // **不取整**：整数化会让相邻 cell 的棱交点**精确重合** ⇒ 退化三角形（壳是浮点路径，无需 int8 存储口径）。
    // W6 河道：河床**下切**（把地表压低）——同样按边界淡出，避免区域边界出现半截河岸。
    float riverCarve = 0.0F;
    if (m_river != nullptr && fade > 0.0F) {
        riverCarve = m_river->CarveAt(static_cast<float>(worldX), static_cast<float>(worldZ)) * fade;
    }

    const float value = (signedDistance + overhang + carve + riverCarve) * kShellDensityUnitsPerBlock;
    return std::clamp(value, static_cast<float>(kDensityMin), static_cast<float>(kDensityMax));
}

MeshData BuildShellBlockMesh(const TerrainNoiseGenerator& noise, const TerrainGenerationParams& generation,
                             const SurfaceShellParams& params, const SurfaceShellRegion& region, BlockCoord block,
                             const RiverCarveField* river) {
    const SurfaceShellSampler sampler(noise, generation, params, region, block, river);
    return BuildVolumeMesh(sampler);
}

}  // namespace vx
