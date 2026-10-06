// Ring 0 地表壳区域的常驻规划实现（W7-S3b① / ADR 0024 决策二）。
//
// 纯函数：三个清单只由入参唯一决定（红线 7）；不触碰任何世界状态、不做 IO。
// 复用 `surface_shell.*` 的 `ComputeShellBlockSpanY` / `ShellBlockTouchesRegion`（同一份 Y 范围口径）。

#include "shell/surface_shell_residency.hpp"

#include <algorithm>
#include <cmath>

namespace vx {
namespace {

/// 向下取整的整数除法（`b > 0`）：负坐标也按"数学 floor"处理（与 `TerrainWindowForPlayerBlocks` 同口径）。
/// 用 `double` 中转即可 —— 列坐标量级 ≪ 2^53，无精度损失。
[[nodiscard]] int FloorDiv(int a, int b) noexcept {
    return static_cast<int>(std::floor(static_cast<double>(a) / static_cast<double>(b)));
}

/// 升序去重（`std::set_difference` 要求有序输入）。
void SortUnique(std::vector<BlockCoord>& blocks) {
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
}

}  // namespace

SurfaceShellRegion MakeSurfaceShellRegion(const SurfaceShellWindow& window,
                                          const SurfaceShellRegion& worldBounds) noexcept {
    const int radius = (window.radiusTiles > 0) ? window.radiusTiles : 0;

    // 覆盖 tile `[center − r, center + r]` 的列区间（右开）。
    const int desiredMinX = (window.centerTileX - radius) * kTerrainTileSize;
    const int desiredMaxX = (window.centerTileX + radius + 1) * kTerrainTileSize;
    const int desiredMinZ = (window.centerTileZ - radius) * kTerrainTileSize;
    const int desiredMaxZ = (window.centerTileZ + radius + 1) * kTerrainTileSize;

    SurfaceShellRegion region;
    region.minColumnX = std::max(desiredMinX, worldBounds.minColumnX);
    region.maxColumnX = std::min(desiredMaxX, worldBounds.maxColumnX);
    region.minColumnZ = std::max(desiredMinZ, worldBounds.minColumnZ);
    region.maxColumnZ = std::min(desiredMaxZ, worldBounds.maxColumnZ);

    // 钳制后为空（世界范围为空 / 完全不相交）⇒ 塌成空区域（`ContainsColumn` 恒假）。
    if (region.maxColumnX < region.minColumnX) {
        region.maxColumnX = region.minColumnX;
    }
    if (region.maxColumnZ < region.minColumnZ) {
        region.maxColumnZ = region.minColumnZ;
    }
    return region;
}

int SurfaceShellRebuildRingThickness(const SurfaceShellParams& params) noexcept {
    if (!(params.edgeFadeBlocks > 0.0F)) {
        return 0;
    }
    return static_cast<int>(std::ceil(params.edgeFadeBlocks / static_cast<float>(kVolumeBlockSize)));
}

ShellBlockSpan SurfaceShellSpanCache::SpanOf(int blockX, int blockZ) {
    const std::pair<int, int> key { blockX, blockZ };
    const auto                found = m_spans.find(key);
    if (found != m_spans.end()) {
        return found->second;
    }
    // 粗采样 + 安全余量（⊇ 逐列口径）：逐列采 `34×34` 次 ≈ 0.9 ms/列块 ⇒ 主线程按窗口规划会超帧预算。
    const ShellBlockSpan span = ComputeShellBlockSpanYCoarse(*m_noise, *m_params, blockX, blockZ,
                                                              kShellSpanSampleStepBlocks,
                                                              kShellSpanMarginHeightBlocks);
    m_spans.emplace(key, span);
    return span;
}

namespace {

/// 规划核心（Y 范围经 `spans` 取 ⇒ 可跨调用复用；`noise` 版只是临时缓存的特例）。
[[nodiscard]] SurfaceShellResidencyPlan PlanWithSpanCache(SurfaceShellSpanCache& spans,
                                                          const SurfaceShellParams& params,
                                                          const SurfaceShellRegion& region, bool regionChanged,
                                                          const std::vector<BlockCoord>& resident) {
    SurfaceShellResidencyPlan plan;

    std::vector<BlockCoord> residentSorted = resident;
    SortUnique(residentSorted);

    // 空区域 ⇒ 目标为空（`toUnload` = 全部常驻）。
    if (region.maxColumnX <= region.minColumnX || region.maxColumnZ <= region.minColumnZ) {
        plan.toUnload     = residentSorted;
        plan.desiredCount = 0;
        return plan;
    }

    const int blockXMin = FloorDiv(region.minColumnX, kVolumeBlockSize);
    const int blockXMax = FloorDiv(region.maxColumnX - 1, kVolumeBlockSize);
    const int blockZMin = FloorDiv(region.minColumnZ, kVolumeBlockSize);
    const int blockZMax = FloorDiv(region.maxColumnZ - 1, kVolumeBlockSize);

    // 目标集合 = 区域内每个列块 × 其 Y 块范围。
    std::vector<BlockCoord> target;
    for (int blockX = blockXMin; blockX <= blockXMax; ++blockX) {
        for (int blockZ = blockZMin; blockZ <= blockZMax; ++blockZ) {
            if (!ShellBlockTouchesRegion(region, blockX, blockZ)) {
                continue;
            }
            const ShellBlockSpan span = spans.SpanOf(blockX, blockZ);
            for (int blockY = span.minBlockY; blockY <= span.maxBlockY; ++blockY) {
                target.push_back(BlockCoord { blockX, blockY, blockZ });
            }
        }
    }
    SortUnique(target);

    plan.desiredCount = target.size();

    // 集合差（两者均已升序）。
    std::set_difference(target.begin(), target.end(), residentSorted.begin(), residentSorted.end(),
                        std::back_inserter(plan.toLoad));
    std::set_difference(residentSorted.begin(), residentSorted.end(), target.begin(), target.end(),
                        std::back_inserter(plan.toUnload));

    // 区域边界移动 ⇒ 最外 `thickness` 圈列块的淡出系数变化 ⇒ 重建（∩ 常驻：新块走 `toLoad`，不重复列）。
    const int thickness = SurfaceShellRebuildRingThickness(params);
    if (regionChanged && thickness > 0) {
        for (int blockX = blockXMin; blockX <= blockXMax; ++blockX) {
            const bool outerX = (blockX - blockXMin < thickness) || (blockXMax - blockX < thickness);
            for (int blockZ = blockZMin; blockZ <= blockZMax; ++blockZ) {
                const bool outerZ = (blockZ - blockZMin < thickness) || (blockZMax - blockZ < thickness);
                if (!outerX && !outerZ) {
                    continue;
                }
                if (!ShellBlockTouchesRegion(region, blockX, blockZ)) {
                    continue;
                }
                const ShellBlockSpan span = spans.SpanOf(blockX, blockZ);
                for (int blockY = span.minBlockY; blockY <= span.maxBlockY; ++blockY) {
                    const BlockCoord block { blockX, blockY, blockZ };
                    if (std::binary_search(residentSorted.begin(), residentSorted.end(), block)) {
                        plan.toRebuild.push_back(block);
                    }
                }
            }
        }
        SortUnique(plan.toRebuild);
    }

    return plan;
}

}  // namespace

SurfaceShellResidencyPlan PlanSurfaceShellResidency(SurfaceShellSpanCache& spans, const SurfaceShellParams& params,
                                                    const SurfaceShellRegion& region, bool regionChanged,
                                                    const std::vector<BlockCoord>& resident) {
    return PlanWithSpanCache(spans, params, region, regionChanged, resident);
}

SurfaceShellResidencyPlan PlanSurfaceShellResidency(const TerrainNoiseGenerator& noise, const SurfaceShellParams& params,
                                                    const SurfaceShellRegion& region, bool regionChanged,
                                                    const std::vector<BlockCoord>& resident) {
    SurfaceShellSpanCache spans(noise, params);  // 临时缓存：不跨调用复用，但结果与带缓存版逐位一致
    return PlanWithSpanCache(spans, params, region, regionChanged, resident);
}

}  // namespace vx
