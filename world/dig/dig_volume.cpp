#include "dig/dig_volume.hpp"

#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace vx {
namespace {

/// 挖除时把 CSG 距离场"铺开"的过渡带宽度（格）：球外这一圈内也写一次 `max`，使墙面附近的
/// 密度成为**真实距离**而不是饱和值，Surface Nets 的顶点插值才落在正确位置（否则墙面会整体偏厚）。
constexpr double kCarveSdfBandBlocks = 1.5;

/// 向下取整的整数除法（负数也正确）。
[[nodiscard]] int FloorDiv(int value, int divisor) noexcept {
    const int quotient  = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? (quotient - 1) : quotient;
}

/// 世界坐标（格）→ 所在块索引。
[[nodiscard]] int BlockIndexOf(double coordinate) noexcept {
    const double floored = std::floor(coordinate);
    return FloorDiv(static_cast<int>(floored), kVolumeBlockSize);
}

/// 密度数组下标（`i/j/k ∈ [0, kVolumeBlockSize]`）。
[[nodiscard]] inline std::size_t DensityIndex(int i, int j, int k) noexcept {
    const std::size_t extent = static_cast<std::size_t>(kVolumeSampleCount);
    return static_cast<std::size_t>(i) + extent * (static_cast<std::size_t>(j) + extent * static_cast<std::size_t>(k));
}

/// 分类一个块的填充（T30）：含共享边界在内的全部采样同号时可整块排除出塌落邻域。
[[nodiscard]] BlockFill ClassifyFill(const std::vector<std::int8_t>& density) noexcept {
    bool anySolid = false;
    bool anyAir   = false;
    for (const std::int8_t value : density) {
        (value < 0) ? (anySolid = true) : (anyAir = true);
        if (anySolid && anyAir) {
            return BlockFill::Mixed;
        }
    }
    return anySolid ? BlockFill::Solid : BlockFill::Air;
}

/// 把一个体积块接到 `IVolumeSampler`：块内**直读数组**（快路径），越界回落到世界采样
/// ⇒ 块边界与区域边界处取到的是同一份密度，等值面因此无接缝。
class BlockSampler final : public IVolumeSampler {
public:
    BlockSampler(const DigVolumeWorld& world, const VolumeBlock& block) noexcept
        : m_world(world), m_block(block) {}

    [[nodiscard]] float Sample(int i, int j, int k) const override {
        const bool inside = (i >= 0 && i <= kVolumeBlockSize) && (j >= 0 && j <= kVolumeBlockSize) &&
                            (k >= 0 && k <= kVolumeBlockSize);
        if (inside) {
            return static_cast<float>(m_block.density[DensityIndex(i, j, k)]);
        }
        return m_world.SampleDensity(static_cast<double>(BlockOriginBlocks(m_block.coord.x) + i),
                                     static_cast<double>(BlockOriginBlocks(m_block.coord.y) + j),
                                     static_cast<double>(BlockOriginBlocks(m_block.coord.z) + k));
    }

    /// 材质槽位（ADR 0014）：材质是"该列地表的可挖材质"这一**纯函数**（不随块存储），
    /// 故越界与否走同一条路径，无需回退分支。
    [[nodiscard]] std::uint8_t SampleMaterial(int i, int j, int k) const override {
        return m_world.SampleMaterialSlot(BlockOriginBlocks(m_block.coord.x) + i,
                                          BlockOriginBlocks(m_block.coord.y) + j,
                                          BlockOriginBlocks(m_block.coord.z) + k);
    }

private:
    const DigVolumeWorld& m_world;
    const VolumeBlock&    m_block;
};

}  // namespace

DigVolumeWorld::DigVolumeWorld(const TerrainWorld& terrain, const DigRegionTable& regions)
    : m_terrain(terrain), m_regions(regions) {}

bool DigVolumeWorld::IsInsideRegion(double x, double y, double z) const noexcept {
    return m_regions.IsDiggable(x, y, z);
}

std::uint8_t DigVolumeWorld::SampleMaterialSlot(int worldX, int /*worldY*/, int worldZ) const noexcept {
    std::uint8_t slot = kNoMaterialSlot;
    if (m_terrain.QueryDigMaterialSlot(static_cast<float>(worldX), static_cast<float>(worldZ), slot)) {
        return slot;
    }
    return kNoMaterialSlot;
}

void DigVolumeWorld::BuildSurfaceHeightCache() {
    m_surfaceCache.clear();
    m_cacheWidth = 0;
    m_cacheDepth = 0;

    const std::vector<BlockCoord>& blocks = m_regions.Blocks();
    if (blocks.empty()) {
        return;
    }

    int  minX  = 0;
    int  minZ  = 0;
    int  maxX  = 0;
    int  maxZ  = 0;
    bool first = true;
    for (const BlockCoord& coord : blocks) {
        const int blockMinX = BlockOriginBlocks(coord.x);
        const int blockMinZ = BlockOriginBlocks(coord.z);
        const int blockMaxX = blockMinX + kVolumeBlockSize;
        const int blockMaxZ = blockMinZ + kVolumeBlockSize;
        if (first) {
            minX  = blockMinX;
            maxX  = blockMaxX;
            minZ  = blockMinZ;
            maxZ  = blockMaxZ;
            first = false;
        } else {
            minX = std::min(minX, blockMinX);
            maxX = std::max(maxX, blockMaxX);
            minZ = std::min(minZ, blockMinZ);
            maxZ = std::max(maxZ, blockMaxZ);
        }
    }

    // 网格化会读取块外一层采样，故缓存各向外扩 1 格。
    m_cacheMinX = minX - 1;
    m_cacheMinZ = minZ - 1;
    m_cacheWidth  = (maxX + 1) - m_cacheMinX + 1;
    m_cacheDepth  = (maxZ + 1) - m_cacheMinZ + 1;
    m_surfaceCache.assign(static_cast<std::size_t>(m_cacheWidth) * static_cast<std::size_t>(m_cacheDepth),
                          std::numeric_limits<float>::quiet_NaN());

    for (int localZ = 0; localZ < m_cacheDepth; ++localZ) {
        for (int localX = 0; localX < m_cacheWidth; ++localX) {
            float surface = 0.0F;
            if (m_terrain.QueryHeight(static_cast<float>(m_cacheMinX + localX), static_cast<float>(m_cacheMinZ + localZ),
                                      surface)) {
                m_surfaceCache[static_cast<std::size_t>(localZ) * static_cast<std::size_t>(m_cacheWidth) +
                               static_cast<std::size_t>(localX)] = surface;
            }
        }
    }
}

bool DigVolumeWorld::SurfaceHeight(double x, double z, float& outHeight) const noexcept {
    const int columnX = static_cast<int>(std::floor(x));
    const int columnZ = static_cast<int>(std::floor(z));

    if (m_cacheWidth > 0 && m_cacheDepth > 0) {
        const int localX = columnX - m_cacheMinX;
        const int localZ = columnZ - m_cacheMinZ;
        if (localX >= 0 && localX < m_cacheWidth && localZ >= 0 && localZ < m_cacheDepth) {
            const float cached =
                m_surfaceCache[static_cast<std::size_t>(localZ) * static_cast<std::size_t>(m_cacheWidth) +
                               static_cast<std::size_t>(localX)];
            if (std::isnan(cached)) {
                return false;  // 该列无地形数据
            }
            outHeight = cached;
            return true;
        }
    }
    return m_terrain.QueryHeight(static_cast<float>(x), static_cast<float>(z), outHeight);
}

float DigVolumeWorld::TerrainDerivedDensity(double x, double y, double z) const noexcept {
    float surface = 0.0F;
    if (!SurfaceHeight(x, z, surface)) {
        return static_cast<float>(kDensityMax);  // 无地形数据 ⇒ 视为空
    }
    const double delta = (y - static_cast<double>(surface)) * static_cast<double>(kDensityUnitsPerBlock);
    return static_cast<float>(
        std::clamp(delta, static_cast<double>(kDensityMin), static_cast<double>(kDensityMax)));
}

void DigVolumeWorld::InitFromHeightField() {
    m_blocks.clear();
    BuildSurfaceHeightCache();

    for (const BlockCoord& coord : m_regions.Blocks()) {
        const int originX = BlockOriginBlocks(coord.x);
        const int originY = BlockOriginBlocks(coord.y);
        const int originZ = BlockOriginBlocks(coord.z);

        VolumeBlock block;
        block.coord = coord;
        block.density.assign(static_cast<std::size_t>(kVolumeSampleCount) * static_cast<std::size_t>(kVolumeSampleCount) *
                                 static_cast<std::size_t>(kVolumeSampleCount),
                             0);
        for (int k = 0; k < kVolumeSampleCount; ++k) {
            for (int j = 0; j < kVolumeSampleCount; ++j) {
                for (int i = 0; i < kVolumeSampleCount; ++i) {
                    const float density =
                        TerrainDerivedDensity(static_cast<double>(originX + i), static_cast<double>(originY + j),
                                              static_cast<double>(originZ + k));
                    block.density[DensityIndex(i, j, k)] = static_cast<std::int8_t>(std::lround(density));
                }
            }
        }
        m_blocks.emplace(coord, std::move(block));
    }

    std::vector<BlockCoord> all;
    all.reserve(m_blocks.size());
    for (const auto& entry : m_blocks) {
        all.push_back(entry.first);
    }
    (void)RemeshDirtyBlocks(all);
}

float DigVolumeWorld::SampleDensity(double x, double y, double z) const noexcept {
    const BlockCoord coord { BlockIndexOf(x), BlockIndexOf(y), BlockIndexOf(z) };
    const auto       found = m_blocks.find(coord);
    if (found != m_blocks.end()) {
        const VolumeBlock& block   = found->second;
        const int          localX  = static_cast<int>(std::floor(x)) - BlockOriginBlocks(coord.x);
        const int          localY  = static_cast<int>(std::floor(y)) - BlockOriginBlocks(coord.y);
        const int          localZ  = static_cast<int>(std::floor(z)) - BlockOriginBlocks(coord.z);
        const bool         inside  = (localX >= 0 && localX <= kVolumeBlockSize) && (localY >= 0 && localY <= kVolumeBlockSize) &&
                            (localZ >= 0 && localZ <= kVolumeBlockSize);
        if (inside) {
            return static_cast<float>(block.density[DensityIndex(localX, localY, localZ)]);
        }
    }
    return TerrainDerivedDensity(x, y, z);
}

bool DigVolumeWorld::CarveSphere(const glm::dvec3& center, float radiusBlocks, std::vector<BlockCoord>& dirtyOut,
                                 VoxelBounds* boundsOut) {
    if (boundsOut != nullptr) {
        *boundsOut = VoxelBounds {};  // 先视为空；有改动时才写出范围
    }
    if (!(radiusBlocks > 0.0F) || m_blocks.empty()) {
        return false;
    }

    const double radius     = static_cast<double>(radiusBlocks);
    const double outer      = radius + kCarveSdfBandBlocks;
    const double outerSq    = outer * outer;
    bool         changedAny = false;

    for (auto& entry : m_blocks) {
        VolumeBlock& block   = entry.second;
        const int    originX = BlockOriginBlocks(block.coord.x);
        const int    originY = BlockOriginBlocks(block.coord.y);
        const int    originZ = BlockOriginBlocks(block.coord.z);
        const double maxX    = static_cast<double>(originX + kVolumeBlockSize);
        const double maxY    = static_cast<double>(originY + kVolumeBlockSize);
        const double maxZ    = static_cast<double>(originZ + kVolumeBlockSize);

        // 球（含过渡带）的 AABB 与该块的 AABB 不相交 ⇒ 完全不受影响。闭区间比较，避免"刚好相切"漏掉。
        if (center.x + outer < static_cast<double>(originX) || center.x - outer > maxX || center.y + outer < static_cast<double>(originY) ||
            center.y - outer > maxY || center.z + outer < static_cast<double>(originZ) || center.z - outer > maxZ) {
            continue;
        }

        bool blockChanged = false;
        for (int k = 0; k < kVolumeSampleCount; ++k) {
            for (int j = 0; j < kVolumeSampleCount; ++j) {
                for (int i = 0; i < kVolumeSampleCount; ++i) {
                    const double dx = static_cast<double>(originX + i) - center.x;
                    const double dy = static_cast<double>(originY + j) - center.y;
                    const double dz = static_cast<double>(originZ + k) - center.z;
                    const double distanceSq = dx * dx + dy * dy + dz * dz;
                    if (distanceSq >= outerSq) {
                        continue;  // 过渡带之外：保持原值（饱和实心）
                    }

                    // CSG 取 max：球内 `radius − dist` 为正（挖空），球外为负（保留原材质）。
                    const double signedDistance = radius - std::sqrt(distanceSq);
                    const int    carved = std::clamp(static_cast<int>(std::lround(signedDistance *
                                                                                 static_cast<double>(kDensityUnitsPerBlock))),
                                                     kDensityMin, kDensityMax);
                    std::int8_t& density = block.density[DensityIndex(i, j, k)];
                    if (carved > static_cast<int>(density)) {
                        density      = static_cast<std::int8_t>(carved);
                        blockChanged = true;
                    }
                }
            }
        }

        if (blockChanged) {
            block.carved = true;
            // T30：挖除只会把实心变空 ⇒ 该块至少是"混合"（原本全实心的块就此失去"可整块排除"的资格）。
            block.fill = BlockFill::Mixed;
            dirtyOut.push_back(block.coord);
            changedAny = true;
        }
    }

    // T30：被改动的采样必然落在"球 + 过渡带"的 AABB 内 —— 故直接把球的 AABB 作为改动范围写出
    //（比逐块求交集略保守，但更便宜，且对塌落邻域完全够用）。
    if (boundsOut != nullptr && changedAny) {
        const int loX = static_cast<int>(std::floor(center.x - outer));
        const int hiX = static_cast<int>(std::ceil(center.x + outer));
        const int loY = static_cast<int>(std::floor(center.y - outer));
        const int hiY = static_cast<int>(std::ceil(center.y + outer));
        const int loZ = static_cast<int>(std::floor(center.z - outer));
        const int hiZ = static_cast<int>(std::ceil(center.z + outer));
        *boundsOut = VoxelBounds { loX, loY, loZ, hiX, hiY, hiZ };
    }

    return changedAny;
}

std::size_t DigVolumeWorld::RemeshDirtyBlocks(const std::vector<BlockCoord>& dirty) {
    std::vector<BlockCoord> unique = dirty;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    std::size_t remeshed = 0;
    for (const BlockCoord& coord : unique) {
        const auto found = m_blocks.find(coord);
        if (found == m_blocks.end()) {
            continue;
        }
        VolumeBlock& block = found->second;
        const BlockSampler sampler(*this, block);
        block.mesh = BuildVolumeMesh(sampler);
        block.fill = ClassifyFill(block.density);  // T30：与密度同步（塌落邻域收缩依赖它）
        ++remeshed;
    }
    return remeshed;
}

DensityRegion DigVolumeWorld::ReadDensityRegion(int minX, int minY, int minZ, int sizeX, int sizeY, int sizeZ) const {
    DensityRegion region;
    region.minX  = minX;
    region.minY  = minY;
    region.minZ  = minZ;
    region.sizeX = (sizeX > 0) ? sizeX : 0;
    region.sizeY = (sizeY > 0) ? sizeY : 0;
    region.sizeZ = (sizeZ > 0) ? sizeZ : 0;
    region.values.assign(region.Index(region.sizeX - 1, region.sizeY - 1, region.sizeZ - 1) + 1,
                         static_cast<std::int8_t>(kDensityMax));
    if (region.values.empty() || m_blocks.empty()) {
        return region;
    }

    // 只遍历与该区域相交的块（按块索引范围），避免"每样本一次 map 查找"。
    const BlockCoord blockMin { BlockIndexOf(static_cast<double>(minX)), BlockIndexOf(static_cast<double>(minY)),
                               BlockIndexOf(static_cast<double>(minZ)) };
    const int        lastX = minX + region.sizeX - 1;
    const int        lastY = minY + region.sizeY - 1;
    const int        lastZ = minZ + region.sizeZ - 1;
    const BlockCoord blockMax { BlockIndexOf(static_cast<double>(lastX)), BlockIndexOf(static_cast<double>(lastY)),
                               BlockIndexOf(static_cast<double>(lastZ)) };

    for (int bz = blockMin.z; bz <= blockMax.z; ++bz) {
        for (int by = blockMin.y; by <= blockMax.y; ++by) {
            for (int bx = blockMin.x; bx <= blockMax.x; ++bx) {
                const auto found = m_blocks.find(BlockCoord { bx, by, bz });
                if (found == m_blocks.end()) {
                    continue;
                }
                const VolumeBlock& block   = found->second;
                const int          originX = BlockOriginBlocks(bx);
                const int          originY = BlockOriginBlocks(by);
                const int          originZ = BlockOriginBlocks(bz);
                for (int k = 0; k <= kVolumeBlockSize; ++k) {
                    for (int j = 0; j <= kVolumeBlockSize; ++j) {
                        for (int i = 0; i <= kVolumeBlockSize; ++i) {
                            const int rx = originX + i - minX;
                            const int ry = originY + j - minY;
                            const int rz = originZ + k - minZ;
                            if (!region.Contains(rx, ry, rz)) {
                                continue;
                            }
                            region.values[region.Index(rx, ry, rz)] = block.density[DensityIndex(i, j, k)];
                        }
                    }
                }
            }
        }
    }
    return region;
}

void DigVolumeWorld::WriteDensityRegion(const DensityRegion& region) {
    if (region.values.empty() || m_blocks.empty() || region.sizeX <= 0 || region.sizeY <= 0 || region.sizeZ <= 0) {
        return;
    }

    const int lastX = region.minX + region.sizeX - 1;
    const int lastY = region.minY + region.sizeY - 1;
    const int lastZ = region.minZ + region.sizeZ - 1;
    const BlockCoord blockMin { BlockIndexOf(static_cast<double>(region.minX)),
                                BlockIndexOf(static_cast<double>(region.minY)),
                                BlockIndexOf(static_cast<double>(region.minZ)) };
    const BlockCoord blockMax { BlockIndexOf(static_cast<double>(lastX)), BlockIndexOf(static_cast<double>(lastY)),
                                BlockIndexOf(static_cast<double>(lastZ)) };

    for (int bz = blockMin.z; bz <= blockMax.z; ++bz) {
        for (int by = blockMin.y; by <= blockMax.y; ++by) {
            for (int bx = blockMin.x; bx <= blockMax.x; ++bx) {
                const auto found = m_blocks.find(BlockCoord { bx, by, bz });
                if (found == m_blocks.end()) {
                    continue;
                }
                VolumeBlock& block   = found->second;
                const int    originX = BlockOriginBlocks(bx);
                const int    originY = BlockOriginBlocks(by);
                const int    originZ = BlockOriginBlocks(bz);
                bool         changed = false;
                for (int k = 0; k <= kVolumeBlockSize; ++k) {
                    for (int j = 0; j <= kVolumeBlockSize; ++j) {
                        for (int i = 0; i <= kVolumeBlockSize; ++i) {
                            const int rx = originX + i - region.minX;
                            const int ry = originY + j - region.minY;
                            const int rz = originZ + k - region.minZ;
                            if (!region.Contains(rx, ry, rz)) {
                                continue;
                            }
                            const std::int8_t value = region.values[region.Index(rx, ry, rz)];
                            std::int8_t&      density = block.density[DensityIndex(i, j, k)];
                            if (density != value) {
                                density = value;
                                changed = true;
                            }
                        }
                    }
                }
                if (changed) {
                    block.carved = true;  // 面板的"已改动块数"同时统计挖除与塌落
                    block.fill   = ClassifyFill(block.density);  // T30：写回可能把空变实心，必须重算
                }
            }
        }
    }
}

BlockFill DigVolumeWorld::FillOf(const BlockCoord& coord) const noexcept {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return BlockFill::Air;  // 无块 = 空（与 `ReadDensityRegion` 的填充口径一致）
    }
    return found->second.fill;
}

const MeshData* DigVolumeWorld::FindMesh(const BlockCoord& coord) const noexcept {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return nullptr;
    }
    return &found->second.mesh;
}

std::size_t DigVolumeWorld::CarvedBlockCount() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : m_blocks) {
        if (entry.second.carved) {
            ++count;
        }
    }
    return count;
}

std::size_t DigVolumeWorld::VoxelBytes() const noexcept {
    return m_blocks.size() * static_cast<std::size_t>(kVolumeSampleCount) * static_cast<std::size_t>(kVolumeSampleCount) *
           static_cast<std::size_t>(kVolumeSampleCount);
}

}  // namespace vx
