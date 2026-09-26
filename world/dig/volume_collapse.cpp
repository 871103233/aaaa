#include "dig/volume_collapse.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {
namespace {

/// 支撑检查邻域的采样数上限（8³ 百万）：超过即放弃本次塌落并告警 —— 防止异常大的挖除把内存拉爆。
constexpr std::size_t kMaxRegionSamples = 8U * 1024U * 1024U;

/// 世界坐标（格）→ 所在块索引（向下取整，负数也正确）。
[[nodiscard]] int BlockIndexOfWorld(int coordinate) noexcept {
    return (coordinate >= 0) ? (coordinate / kVolumeBlockSize)
                             : -((-coordinate + kVolumeBlockSize - 1) / kVolumeBlockSize);
}

/// 确定性哈希（红线 7：禁止随机数；同一坐标 + 同一序号必须永远得到同一位）。
[[nodiscard]] std::uint32_t Hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    std::uint32_t hash = a * 0x9E3779B1U ^ b * 0x85EBCA6BU ^ c * 0xC2B2AE35U;
    hash ^= hash >> 15;
    hash *= 0x2545F491U;
    hash ^= hash >> 13;
    return hash;
}

/// 密度值是否表示"实心"（与 `DigVolumeWorld::IsSolid` 同口径：负 = 实心）。
[[nodiscard]] bool IsSolidValue(std::int8_t value) noexcept { return value < 0; }

}  // namespace

CollapseResult ApplyCollapse(DigVolumeWorld& volumes, const CollapseSeed& seed, const CollapseSpec& spec) {
    CollapseResult result;
    if (!spec.enabled || seed.bounds.Empty()) {
        return result;
    }

    // ---- 邻域（T30：围绕**被改动的采样**展开，而不是整个体积 / 整个块）----
    //
    // 水平：悬挑判定最多跨 `max_cantilever_blocks` 步，堆面摊开最远推 `pile_spread_blocks` 列，
    //       故外扩 (悬挑 + 摊开 + 1) 格即可覆盖所有可能被影响的列；`neighborhood_margin_blocks`
    //       仍保留为"在此之上再额外外扩的块数"（默认 0，需要更保守时可调大）。
    // 竖直：载荷通路是纵向的，但**整块实心**（内部没有空腔可落，且其上实心仍接在连续实心段上、
    //       不会失去支撑）与**整块空**（内部没有实心）的块都能整块排除 —— 故竖直 = 自种子块向下到
    //       "最低的非全实心块"、向上到"最高的非全空块"。被排除段紧邻邻域底面 / 顶面，于是
    //       "底面之下全实心 ⇒ 底面等价于地面""顶面之上全空 ⇒ 不可能有实心"，结论与原判据逐体素相同。
    const int reach = static_cast<int>(std::ceil(std::max(spec.maxCantileverBlocks, 0.0F))) +
                      std::max(spec.pileSpreadBlocks, 0) + 1;
    const int marginWorld = std::max(spec.neighborhoodMarginBlocks, 0) * kVolumeBlockSize;

    const int minX = seed.bounds.minX - reach - marginWorld;
    const int maxX = seed.bounds.maxX + reach + marginWorld;
    const int minZ = seed.bounds.minZ - reach - marginWorld;
    const int maxZ = seed.bounds.maxZ + reach + marginWorld;

    // 竖直范围由"块级填充分类"决定，故先求水平方向的块范围。
    const int horizontalMinX = BlockIndexOfWorld(minX);
    const int horizontalMaxX = BlockIndexOfWorld(maxX);
    const int horizontalMinZ = BlockIndexOfWorld(minZ);
    const int horizontalMaxZ = BlockIndexOfWorld(maxZ);

    int volumeMinBlockY = BlockIndexOfWorld(seed.bounds.minY);
    int volumeMaxBlockY = BlockIndexOfWorld(seed.bounds.maxY);
    for (const auto& entry : volumes.Blocks()) {
        volumeMinBlockY = std::min(volumeMinBlockY, entry.first.y);
        volumeMaxBlockY = std::max(volumeMaxBlockY, entry.first.y);
    }

    int lowBlockY  = BlockIndexOfWorld(seed.bounds.minY);
    int highBlockY = BlockIndexOfWorld(seed.bounds.maxY);
    for (int bz = horizontalMinZ; bz <= horizontalMaxZ; ++bz) {
        for (int bx = horizontalMinX; bx <= horizontalMaxX; ++bx) {
            for (int by = volumeMinBlockY; by <= volumeMaxBlockY; ++by) {
                const BlockFill fill = volumes.FillOf(BlockCoord { bx, by, bz });
                if (fill != BlockFill::Solid) {
                    lowBlockY = std::min(lowBlockY, by);
                }
                if (fill != BlockFill::Air) {
                    highBlockY = std::max(highBlockY, by);
                }
            }
        }
    }

    const int minY = BlockOriginBlocks(lowBlockY);
    const int maxY = BlockOriginBlocks(highBlockY) + kVolumeBlockSize;
    const int sizeX = maxX - minX + 1;
    const int sizeY = maxY - minY + 1;
    const int sizeZ = maxZ - minZ + 1;

    const std::size_t sampleCount = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY) *
                                    static_cast<std::size_t>(sizeZ);
    result.regionSamples = sampleCount;
    if (sampleCount > kMaxRegionSamples) {
        VX_LOG_WARN("塌落跳过：支撑检查邻域过大（%d × %d × %d = %zu 个采样，上限 %zu）", sizeX, sizeY, sizeZ,
                    sampleCount, kMaxRegionSamples);
        return result;
    }

    DensityRegion region = volumes.ReadDensityRegion(minX, minY, minZ, sizeX, sizeY, sizeZ);

    // ---- ① 支撑体素 = "载荷能沿实心一路传到地底"的实心体素 ----
    //
    // 先做**纵向**判据（逐列自下而上）：只有"从区域底面起连续实心"的那一段才算接地 ——
    // 区域底面视作地面（本项目的区域底面就是世界 y=0，地形处处实心）。
    // 再把"接地"沿**同一高度层的实心连通**横向传播 `floor(maxCantileverBlocks)` 步：
    // 这一步就是"悬挑 / 拱效应"——离接地处不超过该跨度的岩体仍算有支撑。
    std::vector<std::uint8_t> reached(region.values.size(), 0);
    std::vector<std::uint32_t> queue;
    // 接地体素通常占邻域的一半上下（地下那部分）；按此预留可免去多次扩容带来的整段拷贝。
    queue.reserve(region.values.size() / 2U + 16U);

    for (int z = 0; z < sizeZ; ++z) {
        for (int x = 0; x < sizeX; ++x) {
            bool chain = true;  // 该列自底向上是否仍"连着地面"
            for (int y = 0; y < sizeY; ++y) {
                const std::size_t index = region.Index(x, y, z);
                if (!IsSolidValue(region.values[index])) {
                    chain = false;  // 出现空腔 ⇒ 其上的一切都失去了这条路
                    continue;
                }
                if (chain) {
                    reached[index] = 1;
                    queue.push_back(static_cast<std::uint32_t>(index));
                }
            }
        }
    }

    // ---- ② 同层 4 邻域 BFS：最多走 floor(maxCantilever) 步 ----
    const int maxSteps = static_cast<int>(std::floor(std::max(spec.maxCantileverBlocks, 0.0F)));
    std::size_t head = 0;
    for (int step = 0; step < maxSteps; ++step) {
        const std::size_t levelEnd = queue.size();
        if (head >= levelEnd) {
            break;
        }
        for (; head < levelEnd; ++head) {
            const std::size_t index = queue[head];
            const int z = static_cast<int>(index / (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY)));
            const std::size_t rem = index % (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY));
            const int         y   = static_cast<int>(rem / static_cast<std::size_t>(sizeX));
            const int         x   = static_cast<int>(rem % static_cast<std::size_t>(sizeX));

            const int nx[4] = { x - 1, x + 1, x, x };
            const int nz[4] = { z, z, z - 1, z + 1 };
            for (int dir = 0; dir < 4; ++dir) {
                if (nx[dir] < 0 || nx[dir] >= sizeX || nz[dir] < 0 || nz[dir] >= sizeZ) {
                    continue;
                }
                const std::size_t neighbor = region.Index(nx[dir], y, nz[dir]);
                if (reached[neighbor] != 0 || !IsSolidValue(region.values[neighbor])) {
                    continue;
                }
                reached[neighbor] = 1;
                queue.push_back(static_cast<std::uint32_t>(neighbor));
            }
        }
    }

    // ---- ③ 逐列下落（质量守恒）+ 堆顶摊开 ----
    const std::size_t columnCount = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeZ);
    std::vector<int> pileBase(columnCount, -1);
    std::vector<int> pileTop(columnCount, -1);

    int changedMinX = maxX;
    int changedMinY = maxY;
    int changedMinZ = maxZ;
    int changedMaxX = minX;
    int changedMaxY = minY;
    int changedMaxZ = minZ;
    auto markChanged = [&](int x, int y, int z) {
        changedMinX = std::min(changedMinX, minX + x);
        changedMinY = std::min(changedMinY, minY + y);
        changedMinZ = std::min(changedMinZ, minZ + z);
        changedMaxX = std::max(changedMaxX, minX + x);
        changedMaxY = std::max(changedMaxY, minY + y);
        changedMaxZ = std::max(changedMaxZ, minZ + z);
    };

    std::vector<int> falling;
    // "该世界采样确实落在某个体积块内"——写回只会落到这些采样上，故塌落的落点 / 摊开目标都必须满足它。
    const auto& blocks  = volumes.Blocks();
    const auto  covered = [&blocks](int worldX, int worldY, int worldZ) {
        return blocks.find(BlockCoord { BlockIndexOfWorld(worldX), BlockIndexOfWorld(worldY),
                                       BlockIndexOfWorld(worldZ) }) != blocks.end();
    };
    for (int z = 0; z < sizeZ; ++z) {
        for (int x = 0; x < sizeX; ++x) {
            const std::size_t column = static_cast<std::size_t>(z) * static_cast<std::size_t>(sizeX) +
                                       static_cast<std::size_t>(x);
            falling.clear();
            for (int y = 0; y < sizeY; ++y) {
                const std::size_t index = region.Index(x, y, z);
                if (IsSolidValue(region.values[index]) && reached[index] == 0) {
                    falling.push_back(y);
                }
            }
            if (falling.empty()) {
                continue;
            }
            result.unsupportedVoxels += falling.size();

            // 本列的失支撑体素按 y **分成连续段**，逐段处理（从低到高）：
            // 段之间可能夹着本来就稳的实心（例如"贴底的基岩 + 更高的悬空石板"），
            // 若把整列当成一段，低段"无处可落"就会连带让高段也不塌。
            std::size_t runStart = 0;
            while (runStart < falling.size()) {
                std::size_t runEnd = runStart;
                while (runEnd + 1 < falling.size() && falling[runEnd + 1] == falling[runEnd] + 1) {
                    ++runEnd;
                }
                const std::size_t runCount = runEnd - runStart + 1;

                // 落点：从该段最低体素往下找第一个"空 + 其下方为实心 + 该采样确实落在体积块内"的体素。
                // 最后一个条件保证"落点写回得进去"——否则碎块会写进不存在的块里凭空消失（破坏质量守恒）。
                int base = -1;
                for (int y = falling[runStart] - 1; y >= 0; --y) {
                    const bool empty = !IsSolidValue(region.values[region.Index(x, y, z)]);
                    const bool belowSolid = (y == 0) || IsSolidValue(region.values[region.Index(x, y - 1, z)]);
                    if (empty && belowSolid && covered(minX + x, minY + y, minZ + z)) {
                        base = y;
                        break;
                    }
                }
                if (base < 0) {
                    runStart = runEnd + 1;  // 该段无处可落（已在体积块覆盖之外）：不动它，保证质量守恒
                    continue;
                }

                // 先**只数**落点槽位（不改数据）：槽位不足则本段不塌 —— 宁可"没塌"，也不能让碎块凭空消失。
                std::vector<int> slots;
                for (int y = base; y < sizeY && slots.size() < runCount; ++y) {
                    if (!IsSolidValue(region.values[region.Index(x, y, z)]) &&
                        covered(minX + x, minY + y, minZ + z)) {
                        slots.push_back(y);
                    }
                }
                if (slots.size() < runCount) {
                    runStart = runEnd + 1;
                    continue;
                }

                for (std::size_t i = runStart; i <= runEnd; ++i) {
                    region.values[region.Index(x, falling[i], z)] = static_cast<std::int8_t>(kDensityMax);
                    markChanged(x, falling[i], z);
                }
                for (const int y : slots) {
                    region.values[region.Index(x, y, z)] = static_cast<std::int8_t>(-kDensityUnitsPerBlock);
                    markChanged(x, y, z);
                }
                result.movedVoxels += runCount;
                pileBase[column] = (pileBase[column] < 0) ? base : std::min(pileBase[column], base);
                pileTop[column]  = std::max(pileTop[column] < 0 ? base : pileTop[column], slots.back());

                runStart = runEnd + 1;
            }
        }
    }

    // ---- 堆顶摊开：按确定性哈希把堆顶体素推给相邻列（仍守恒）----
    if (spec.pileSpreadBlocks > 0 && result.movedVoxels > 0) {
        for (int z = 0; z < sizeZ; ++z) {
            for (int x = 0; x < sizeX; ++x) {
                const std::size_t column = static_cast<std::size_t>(z) * static_cast<std::size_t>(sizeX) +
                                           static_cast<std::size_t>(x);
                for (int attempt = 0; attempt < spec.pileSpreadBlocks; ++attempt) {
                    if (pileTop[column] <= pileBase[column]) {
                        break;  // 堆只剩底面一层，不再摊
                    }
                    const std::uint32_t hash = Hash3(static_cast<std::uint32_t>(minX + x),
                                                     static_cast<std::uint32_t>(minZ + z),
                                                     static_cast<std::uint32_t>(attempt));
                    const int dx[4] = { -1, 1, 0, 0 };
                    const int dz[4] = { 0, 0, -1, 1 };
                    const int dir   = static_cast<int>(hash & 3U);
                    const int nx    = x + dx[dir];
                    const int nz    = z + dz[dir];
                    if (nx < 0 || nx >= sizeX || nz < 0 || nz >= sizeZ) {
                        continue;
                    }
                    const std::size_t neighbor = static_cast<std::size_t>(nz) * static_cast<std::size_t>(sizeX) +
                                                 static_cast<std::size_t>(nx);
                    if (pileTop[neighbor] < 0) {
                        continue;  // 邻列没有本次堆积的碎堆，不往外撒
                    }
                    const int targetY = pileTop[neighbor] + 1;
                    if (targetY >= sizeY || IsSolidValue(region.values[region.Index(nx, targetY, nz)])) {
                        continue;
                    }
                    if (!covered(minX + x, minY + pileTop[column], minZ + z) ||
                        !covered(minX + nx, minY + targetY, minZ + nz)) {
                        continue;  // 源头或目标不在体积块覆盖内 ⇒ 不动（保证质量守恒）
                    }
                    region.values[region.Index(x, pileTop[column], z)] = static_cast<std::int8_t>(kDensityMax);
                    region.values[region.Index(nx, targetY, nz)] =
                        static_cast<std::int8_t>(-kDensityUnitsPerBlock);
                    markChanged(x, pileTop[column], z);
                    markChanged(nx, targetY, nz);
                    --pileTop[column];
                    pileTop[neighbor] = targetY;
                    ++result.movedVoxels;
                }
            }
        }
    }

    if (result.movedVoxels == 0) {
        return result;
    }

    volumes.WriteDensityRegion(region);

    // 脏块 = 被改动样本所覆盖的块范围（不做整邻域重网格）。
    const BlockCoord dirtyMin { BlockIndexOfWorld(changedMinX), BlockIndexOfWorld(changedMinY),
                                BlockIndexOfWorld(changedMinZ) };
    const BlockCoord dirtyMax { BlockIndexOfWorld(changedMaxX), BlockIndexOfWorld(changedMaxY),
                                BlockIndexOfWorld(changedMaxZ) };
    for (int bz = dirtyMin.z; bz <= dirtyMax.z; ++bz) {
        for (int by = dirtyMin.y; by <= dirtyMax.y; ++by) {
            for (int bx = dirtyMin.x; bx <= dirtyMax.x; ++bx) {
                result.dirty.push_back(BlockCoord { bx, by, bz });
            }
        }
    }
    return result;
}

}  // namespace vx
