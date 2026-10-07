#include "dig/dig_volume.hpp"

#include "core/clock.hpp"
#include "streaming/volume_build_pipeline.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vx {
namespace {

/// 挖除时把 CSG 距离场"铺开"的过渡带宽度（格）：球外这一圈内也写一次 `max`，使墙面附近的
/// 密度成为**真实距离**而不是饱和值，Surface Nets 的顶点插值才落在正确位置（否则墙面会整体偏厚）。
/// 取值与**唯一口径** `vx::kCarveSdfBandBlocks`（`dig_volume.hpp`，T50 的碎块补丁雕刻共用）一致。
constexpr double kCarveBandBlocks = kCarveSdfBandBlocks;

/// **"该列不裁剪"的哨兵地板**（T59 / [ADR 0020](../../docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md)
/// 决策一）：恒低于任何合法世界 Y（世界垂直范围 0~512 格），故用 `y < 地板` 判定时永不命中。
constexpr double kBandFloorNone = -1.0;

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

/// 密度数组下标（`i/j/k ∈ [0, kVolumeBlockSize]`）—— 转发到**公开**的 `VolumeDensityIndex`（T81：
/// worker 侧的纯构建路径与主线程路径必须共用同一个布局实现，不得各写一份）。
[[nodiscard]] inline std::size_t DensityIndex(int i, int j, int k) noexcept {
    return VolumeDensityIndex(i, j, k);
}

/// **由"该列表表面高度"推导密度**（唯一口径；`FillBlockDensity` 与 worker 侧纯构建路径**共用**）。
///
/// 语义：`d = clamp((y − 地表高度) × 127, ±127)` ⇒ 地下为负（实心）、空中为正（空）、地表处跨零。
/// `surface` 为 `NaN`（无地形数据）时按"空"处理（+127 饱和）。
///
/// **为什么必须共用**（T81 / ADR 0022）：worker 构建与主线程构建要逐位一致 ⇒ 连
/// `float`/`double` 的运算顺序与取整方式都不能有第二份实现。
[[nodiscard]] float TerrainDerivedDensityFromHeight(float surface, double y) noexcept {
    if (std::isnan(surface)) {
        return static_cast<float>(kDensityMax);  // 无地形数据 ⇒ 视为空
    }
    const double delta = (y - static_cast<double>(surface)) * static_cast<double>(kDensityUnitsPerBlock);
    return static_cast<float>(
        std::clamp(delta, static_cast<double>(kDensityMin), static_cast<double>(kDensityMax)));
}

}  // namespace

BlockFill ClassifyFill(const std::vector<std::int8_t>& density) noexcept {
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

namespace {

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

    /// 材质槽位（ADR 0014 / T42）：① 块内**已写入**的体素材质优先（塌落搬来的残骸）；
    /// ② 否则回落"该列地表的可挖材质"纯函数。越界与否走同一条路径，无需回退分支。
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
    : m_terrain(terrain), m_regions(regions), m_initCoords(regions.Blocks()) {}

bool DigVolumeWorld::IsInsideRegion(double x, double y, double z) const noexcept {
    return m_regions.IsDiggable(x, y, z);
}

std::uint8_t DigVolumeWorld::SampleMaterialSlot(int worldX, int worldY, int worldZ) const noexcept {
    // ① 已写入的体素材质优先（T42：塌落把残骸的材质搬到了别处，它必须记住自己**原本是什么**）。
    const auto found = m_blocks.find(BlockCoord { BlockIndexOf(static_cast<double>(worldX)),
                                                 BlockIndexOf(static_cast<double>(worldY)),
                                                 BlockIndexOf(static_cast<double>(worldZ)) });
    if (found != m_blocks.end() && !found->second.material.empty()) {
        const VolumeBlock& block = found->second;
        const std::uint8_t stored =
            block.material[DensityIndex(worldX - BlockOriginBlocks(block.coord.x),
                                        worldY - BlockOriginBlocks(block.coord.y),
                                        worldZ - BlockOriginBlocks(block.coord.z))];
        if (stored != kNoMaterialSlot) {
            return stored;
        }
    }

    // ② 未写入 ⇒ 回落"该列地表派生的可挖材质"（纯函数，与引入存储之前逐位一致）。
    std::uint8_t slot = kNoMaterialSlot;
    if (m_terrain.QueryDigMaterialSlot(static_cast<float>(worldX), static_cast<float>(worldZ), slot)) {
        return slot;
    }
    return kNoMaterialSlot;
}

std::vector<std::uint8_t> DigVolumeWorld::ReadMaterialRegion(int minX, int minY, int minZ, int sizeX, int sizeY,
                                                             int sizeZ) const {
    std::vector<std::uint8_t> materials;
    if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0) {
        return materials;
    }
    materials.assign(static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY) *
                         static_cast<std::size_t>(sizeZ),
                     kNoMaterialSlot);

    // 逐列（x, z）：地表派生值**每列只查一次**；列内按 y 走块（y 单调 ⇒ 每 32 步才换一次块查找）。
    for (int rz = 0; rz < sizeZ; ++rz) {
        for (int rx = 0; rx < sizeX; ++rx) {
            const int worldX = minX + rx;
            const int worldZ = minZ + rz;
            const int blockX = BlockIndexOf(static_cast<double>(worldX));
            const int blockZ = BlockIndexOf(static_cast<double>(worldZ));

            std::uint8_t derived       = kNoMaterialSlot;
            bool         derivedCached = false;
            const VolumeBlock* block   = nullptr;
            int                blockY  = std::numeric_limits<int>::min();

            for (int ry = 0; ry < sizeY; ++ry) {
                const int worldY  = minY + ry;
                const int currentY = BlockIndexOf(static_cast<double>(worldY));
                if (currentY != blockY) {
                    blockY                 = currentY;
                    const auto blockFound  = m_blocks.find(BlockCoord { blockX, blockY, blockZ });
                    block                  = (blockFound != m_blocks.end()) ? &blockFound->second : nullptr;
                }

                std::uint8_t slot = kNoMaterialSlot;
                if (block != nullptr && !block->material.empty()) {
                    slot = block->material[DensityIndex(worldX - BlockOriginBlocks(blockX),
                                                        worldY - BlockOriginBlocks(blockY),
                                                        worldZ - BlockOriginBlocks(blockZ))];
                }
                if (slot == kNoMaterialSlot) {
                    if (!derivedCached) {
                        derivedCached = true;
                        (void)m_terrain.QueryDigMaterialSlot(static_cast<float>(worldX), static_cast<float>(worldZ),
                                                             derived);
                    }
                    slot = derived;
                }
                materials[static_cast<std::size_t>(rx) + static_cast<std::size_t>(sizeX) *
                                                              (static_cast<std::size_t>(ry) +
                                                               static_cast<std::size_t>(sizeY) *
                                                                   static_cast<std::size_t>(rz))] = slot;
            }
        }
    }
    return materials;
}

void DigVolumeWorld::SetMaterialSlot(int worldX, int worldY, int worldZ, std::uint8_t slot) {
    const BlockCoord coord { BlockIndexOf(static_cast<double>(worldX)), BlockIndexOf(static_cast<double>(worldY)),
                             BlockIndexOf(static_cast<double>(worldZ)) };
    const auto       found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return;  // 区域外（无块）⇒ 材质无处可存；调用方（塌落回写）本就只写"确实存在的块"
    }

    VolumeBlock& block = found->second;
    if (block.material.empty()) {
        // 懒分配：只有"残骸落在这里"才付这 33³ 字节（未发生倒塌的块永远为零内存）。
        block.material.assign(static_cast<std::size_t>(kVolumeSampleCount) * static_cast<std::size_t>(kVolumeSampleCount) *
                                  static_cast<std::size_t>(kVolumeSampleCount),
                              kNoMaterialSlot);
    }
    block.material[DensityIndex(worldX - BlockOriginBlocks(coord.x), worldY - BlockOriginBlocks(coord.y),
                                worldZ - BlockOriginBlocks(coord.z))] = slot;
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
        // 无地形数据 ⇒ 推导为空。**唯一口径在 `TerrainDerivedDensityFromHeight`**（T81：worker 侧共用同一实现）。
        surface = std::numeric_limits<float>::quiet_NaN();
    }
    return TerrainDerivedDensityFromHeight(surface, y);
}

void DigVolumeWorld::FillBlockDensity(const BlockCoord& coord) {
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

void DigVolumeWorld::MeshBlock(const BlockCoord& coord) {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return;
    }
    VolumeBlock&      stored = found->second;
    const BlockSampler sampler(*this, stored);
    stored.mesh = BuildVolumeMesh(sampler);
    stored.fill = ClassifyFill(stored.density);  // T30：与密度同步（塌落邻域收缩依赖它）
}

// ---------------------------------------------------------------------------
// V0.10 S3：存档差量（ADR 0037）
// ---------------------------------------------------------------------------

bool DigVolumeWorld::ExportBlockSave(const BlockCoord& coord, VolumeDirtyPayload& out) const {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return false;  // 未常驻 ⇒ 导不出（差量应在**卸载前**采集；ADR 0037 决策四）
    }
    const VolumeBlock& block = found->second;
    const std::size_t  owned = kVolumeSaveVoxelCount;

    out.density.assign(owned, 0);
    for (int k = 0; k < kVolumeBlockSize; ++k) {
        for (int j = 0; j < kVolumeBlockSize; ++j) {
            for (int i = 0; i < kVolumeBlockSize; ++i) {
                const std::size_t index = static_cast<std::size_t>(i) +
                                          static_cast<std::size_t>(kVolumeBlockSize) *
                                              (static_cast<std::size_t>(j) + static_cast<std::size_t>(kVolumeBlockSize) *
                                                                                static_cast<std::size_t>(k));
                out.density[index] = block.density[DensityIndex(i, j, k)];
            }
        }
    }

    // 材质**懒分配**：从未写过 ⇒ `materialPresent = false`（读回后仍走"回落列派生"的零内存路径）。
    out.materialPresent = !block.material.empty();
    out.material.clear();
    if (out.materialPresent) {
        out.material.assign(owned, kNoMaterialSlot);
        for (int k = 0; k < kVolumeBlockSize; ++k) {
            for (int j = 0; j < kVolumeBlockSize; ++j) {
                for (int i = 0; i < kVolumeBlockSize; ++i) {
                    const std::size_t index = static_cast<std::size_t>(i) +
                                              static_cast<std::size_t>(kVolumeBlockSize) *
                                                  (static_cast<std::size_t>(j) +
                                                   static_cast<std::size_t>(kVolumeBlockSize) *
                                                       static_cast<std::size_t>(k));
                    out.material[index] = block.material[DensityIndex(i, j, k)];
                }
            }
        }
    }
    return true;
}

bool DigVolumeWorld::ApplyBlockSave(const BlockCoord& coord, const VolumeDirtyPayload& payload) {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return false;  // 未常驻：不加载差量，等它被建出来时**再叠加**（ADR 0037 决策三）
    }
    const std::size_t owned = kVolumeSaveVoxelCount;
    if (payload.density.size() != owned) {
        throw std::invalid_argument("体积块存档：密度载荷长度不符（应为 32³）");
    }
    if (payload.materialPresent && payload.material.size() != owned) {
        throw std::invalid_argument("体积块存档：材质载荷长度不符（应为 32³）");
    }

    VolumeBlock& block   = found->second;
    bool         changed = false;

    for (int k = 0; k < kVolumeBlockSize; ++k) {
        for (int j = 0; j < kVolumeBlockSize; ++j) {
            for (int i = 0; i < kVolumeBlockSize; ++i) {
                const std::size_t index =
                    static_cast<std::size_t>(i) +
                    static_cast<std::size_t>(kVolumeBlockSize) *
                        (static_cast<std::size_t>(j) +
                         static_cast<std::size_t>(kVolumeBlockSize) * static_cast<std::size_t>(k));
                std::int8_t& density = block.density[DensityIndex(i, j, k)];
                if (density != payload.density[index]) {
                    density = payload.density[index];
                    changed = true;
                }
            }
        }
    }

    if (payload.materialPresent) {
        if (block.material.empty()) {
            // 与 `SetMaterialSlot` 同口径：懒分配（此块自此走"已写入优先"路径）。
            block.material.assign(static_cast<std::size_t>(kVolumeSampleCount) * static_cast<std::size_t>(kVolumeSampleCount) *
                                      static_cast<std::size_t>(kVolumeSampleCount),
                                  kNoMaterialSlot);
        }
        for (int k = 0; k < kVolumeBlockSize; ++k) {
            for (int j = 0; j < kVolumeBlockSize; ++j) {
                for (int i = 0; i < kVolumeBlockSize; ++i) {
                    const std::size_t index =
                        static_cast<std::size_t>(i) +
                        static_cast<std::size_t>(kVolumeBlockSize) *
                            (static_cast<std::size_t>(j) +
                             static_cast<std::size_t>(kVolumeBlockSize) * static_cast<std::size_t>(k));
                    std::uint8_t& slot = block.material[DensityIndex(i, j, k)];
                    if (slot != payload.material[index]) {
                        slot    = payload.material[index];
                        changed = true;  // 材质进顶点 ⇒ 变了就得重网格
                    }
                }
            }
        }
    }

    // 共享边界层（本块 + 已常驻邻块）：不重算的话，先恢复的那块会留着按"邻块尚未恢复"算出的边界值。
    const bool boundaryChanged = SyncBlockBoundaryLayers(coord);
    changed                    = changed || boundaryChanged;
    if (changed) {
        block.carved = true;                // 与 `WriteDensityRegion` 同口径：该块此后 `IsBlockDirty()` 为真
        block.fill   = BlockFill::Mixed;    // 挖除只会把实心变空 ⇒ 失去"可整块排除"资格（T30 口径）
    }
    return changed;
}

bool DigVolumeWorld::SyncBlockBoundaryLayers(const BlockCoord& coord) {
    // 本块 + 6 邻（只有**已常驻**的才参与）：每个块的边界层都从"**拥有**该采样的块"重新取值 ——
    // `SampleDensity` 已实现"常驻 ⇒ 读块数据 / 非常驻 ⇒ 回退高度场推导"，故结果**不依赖恢复顺序**。
    const BlockCoord candidates[7] = {
        coord,
        BlockCoord { coord.x - 1, coord.y, coord.z }, BlockCoord { coord.x + 1, coord.y, coord.z },
        BlockCoord { coord.x, coord.y - 1, coord.z }, BlockCoord { coord.x, coord.y + 1, coord.z },
        BlockCoord { coord.x, coord.y, coord.z - 1 }, BlockCoord { coord.x, coord.y, coord.z + 1 },
    };

    bool changed = false;
    for (const BlockCoord& candidate : candidates) {
        const auto found = m_blocks.find(candidate);
        if (found == m_blocks.end()) {
            continue;
        }
        VolumeBlock& block   = found->second;
        const int    originX = BlockOriginBlocks(candidate.x);
        const int    originY = BlockOriginBlocks(candidate.y);
        const int    originZ = BlockOriginBlocks(candidate.z);
        for (int k = 0; k <= kVolumeBlockSize; ++k) {
            for (int j = 0; j <= kVolumeBlockSize; ++j) {
                for (int i = 0; i <= kVolumeBlockSize; ++i) {
                    const bool boundary = (i == kVolumeBlockSize) || (j == kVolumeBlockSize) || (k == kVolumeBlockSize);
                    if (!boundary) {
                        continue;
                    }
                    // 与 `FillBlockDensity` **同一口径**（`lround` 到 int8），故"非常驻"回退值与生成结果逐位一致。
                    const std::int8_t value = static_cast<std::int8_t>(
                        std::lround(SampleDensity(static_cast<double>(originX + i), static_cast<double>(originY + j),
                                                  static_cast<double>(originZ + k))));
                    std::int8_t& density = block.density[DensityIndex(i, j, k)];
                    if (density != value) {
                        density = value;
                        changed = true;
                    }
                }
            }
        }
        block.fill = ClassifyFill(block.density);  // 边界层变了 ⇒ 填充分类同步（T30）
    }
    return changed;
}

void DigVolumeWorld::InitFromHeightField() {
    BeginInitFromHeightField();
    (void)StepInitFromHeightField(InitTotalSteps());
}

void DigVolumeWorld::BeginInitFromHeightField() {
    // 旧行为：初始化**全部**区域块（单测与"不关心常驻调度"的调用方走这条）。
    BeginInitFromHeightField(m_regions.Blocks());
}

void DigVolumeWorld::BeginInitFromHeightField(const std::vector<BlockCoord>& coords) {
    m_blocks.clear();
    m_initCoords = coords;
    BuildSurfaceHeightCache();
    m_initCursor = 0;
}

bool DigVolumeWorld::StepInitFromHeightField(std::size_t maxSteps) {
    const std::vector<BlockCoord>& coords = m_initCoords;
    const std::size_t              blocks = coords.size();
    const std::size_t              total  = blocks * 2U;

    std::size_t steps = 0;
    while (m_initCursor < total && steps < maxSteps) {
        // 第一轮 [0, blocks)：逐块填密度（全部就位后网格化才与一次性初始化逐位一致）；
        // 第二轮 [blocks, 2·blocks)：逐块网格化。
        if (m_initCursor < blocks) {
            FillBlockDensity(coords[m_initCursor]);
        } else {
            MeshBlock(coords[m_initCursor - blocks]);
        }
        ++m_initCursor;
        ++steps;
    }
    return m_initCursor >= total;
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

bool DigVolumeWorld::RasterizeBall(const glm::dvec3& center, float radiusBlocks, bool skipIndestructible,
                                   const BlastMask* mask, std::vector<BlockCoord>& dirtyOut, VoxelBounds* boundsOut) {
    if (boundsOut != nullptr) {
        *boundsOut = VoxelBounds {};  // 先视为空；有改动时才写出范围
    }
    if (!(radiusBlocks > 0.0F) || m_blocks.empty()) {
        return false;
    }

    const double radius     = static_cast<double>(radiusBlocks);
    const double outer      = radius + kCarveBandBlocks;
    const double outerSq    = outer * outer;
    bool         changedAny = false;

    // T79④（T77 清单第 9 条）：**由球心与半径直接推出受影响的块索引范围**，只遍历那几个块，
    // 不再对**全部常驻块**做 AABB 判定（旧实现每次挖除遍历 441 个块 —— 与"世界总量"成正比）。
    // 遍历顺序仍是 `(x, y, z)` 升序 ⇒ `dirtyOut` 与旧实现**逐位一致**（红线 7）。
    //
    // 边界口径（为什么这个范围是完备的）：块 `b` 的采样是世界 `[origin, origin + 32]`（**含共享边界**），
    // 而 `BlockIndexOf` 是"向下取整到 32 格"。故"球 AABB 挡住的块"至少覆盖全部**严格落在带内**的采样：
    // 若某采样的距离 < outer，它所在块的索引必然落在 `[BlockIndexOf(c − outer), BlockIndexOf(c + outer)]`；
    // 恰好落在 `c ± outer` 上的采样在下面会被 `distanceSq >= outerSq` 跳过 ⇒ 不会漏改。
    const int blockMinX = BlockIndexOf(center.x - outer);
    const int blockMaxX = BlockIndexOf(center.x + outer);
    const int blockMinY = BlockIndexOf(center.y - outer);
    const int blockMaxY = BlockIndexOf(center.y + outer);
    const int blockMinZ = BlockIndexOf(center.z - outer);
    const int blockMaxZ = BlockIndexOf(center.z + outer);

    for (int bz = blockMinZ; bz <= blockMaxZ; ++bz) {
        for (int by = blockMinY; by <= blockMaxY; ++by) {
            for (int bx = blockMinX; bx <= blockMaxX; ++bx) {
                const auto found = m_blocks.find(BlockCoord { bx, by, bz });
                if (found == m_blocks.end()) {
                    continue;
                }
                VolumeBlock& block   = found->second;
                const int    originX = BlockOriginBlocks(block.coord.x);
                const int    originY = BlockOriginBlocks(block.coord.y);
                const int    originZ = BlockOriginBlocks(block.coord.z);

                bool blockChanged = false;
                // T59 / ADR 0020 决策一：**每列的"带宽地板"只算一次**（列 = `(originX + i, originZ + k)`）——
                // 预填在 k 层内，避免在 33³ 内层反复查地表高度。`kBandFloorNone` = 该列不裁剪。
                std::array<double, static_cast<std::size_t>(kVolumeSampleCount)> columnBandFloor {};
                for (int k = 0; k < kVolumeSampleCount; ++k) {
                    for (int i = 0; i < kVolumeSampleCount; ++i) {
                        columnBandFloor[static_cast<std::size_t>(i)] = ColumnBandFloor(originX + i, originZ + k);
                    }
                    for (int j = 0; j < kVolumeSampleCount; ++j) {
                        const double sampleY = static_cast<double>(originY + j);
                        for (int i = 0; i < kVolumeSampleCount; ++i) {
                            // 地表以下超出带宽 ⇒ 不可挖（与"不可破坏材质"同口径：保持原状、不参与挖除）。
                            if (sampleY < columnBandFloor[static_cast<std::size_t>(i)]) {
                                continue;
                            }
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
                            // T31：**不可破坏材质**的采样保持原状（它永不参与挖除；`CarveSphere` 不启用该开关）。
                            if (skipIndestructible && IsIndestructibleSample(originX + i, originY + j, originZ + k)) {
                                continue;
                            }
                            // T53：**爆炸波到不了**的采样保持原状（岩后的东西不受破坏）—— 只有 `CarveByDamage` 传掩码。
                            if (mask != nullptr && !mask->Reachable(originX + i, originY + j, originZ + k)) {
                                continue;
                            }
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

bool DigVolumeWorld::IsIndestructibleSample(int worldX, int worldY, int worldZ) const noexcept {
    const std::uint8_t slot = SampleMaterialSlot(worldX, worldY, worldZ);
    if (slot == kNoMaterialSlot) {
        return true;  // 不属于可挖体积（无块）⇒ 不参与挖除
    }
    const MaterialLayer& layer = Materials().Layer(static_cast<int>(slot));
    return layer.indestructible || !(layer.toughness > 0.0F);
}

double DigVolumeWorld::ColumnBandFloor(int worldX, int worldZ) const noexcept {
    const int bandDown = m_regions.BandDownBlocks();
    if (bandDown <= 0) {
        return kBandFloorNone;  // 未启用带宽 ⇒ 旧口径（不裁剪）
    }
    float surface = 0.0F;
    if (!SurfaceHeight(static_cast<double>(worldX), static_cast<double>(worldZ), surface)) {
        return kBandFloorNone;  // 该列没有地形数据 ⇒ 无从判断，按"不裁剪"处理（不改变既有行为）
    }
    return static_cast<double>(surface) - static_cast<double>(bandDown);
}

bool DigVolumeWorld::BelowDiggableBand(int worldX, int worldY, int worldZ) const noexcept {
    return static_cast<double>(worldY) < ColumnBandFloor(worldX, worldZ);
}

bool DigVolumeWorld::CreateBlock(const BlockCoord& coord) {
    if (m_blocks.find(coord) != m_blocks.end()) {
        return false;  // 已常驻
    }
    const std::vector<BlockCoord>& allowed = m_regions.Blocks();  // 升序（`DigRegionTable` 保证）
    if (!std::binary_search(allowed.begin(), allowed.end(), coord)) {
        return false;  // 窗口只能从可挖区域表里取（ADR 0004 硬约束 2）
    }

    // T81 / ADR 0022：装了任务池 ⇒ **只采快照 + 提交**，计算交给 worker（返回 false = "尚未就位"）。
    // 已经在飞（`m_pendingBuilds`）⇒ 不重复提交（否则一次窗口调整会提交多次同一块）。
    if (m_buildPipeline != nullptr) {
        if (m_pendingBuilds.find(coord) != m_pendingBuilds.end()) {
            return false;
        }
        m_pendingBuilds.insert(coord);
        m_buildPipeline->Submit(CaptureBlockBuildInput(coord));
        return false;  // 块要等 `PollBlockBuildsAndInstall` 安装后才存在
    }

    // 与批量初始化**逐字同一条路径**：先填密度，再网格化（保证"按需创建"与"批量初始化"结果一致）。
    FillBlockDensity(coord);
    MeshBlock(coord);
    return true;
}

bool DigVolumeWorld::UnloadBlock(const BlockCoord& coord) {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return false;
    }
    if (IsBlockDirty(coord)) {
        return false;  // ADR 0020 决策五：已改动的块不得卸载（否则玩家挖的洞会消失）
    }
    m_blocks.erase(found);
    return true;
}

bool DigVolumeWorld::EvictBlock(const BlockCoord& coord) {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return false;
    }
    // 刻意**不查** `IsBlockDirty`：淘汰是"内存上界"这一硬约束的出口（ADR 0020 决策五），代价是丢改动。
    m_blocks.erase(found);
    return true;
}

bool DigVolumeWorld::IsBlockDirty(const BlockCoord& coord) const noexcept {
    const auto found = m_blocks.find(coord);
    if (found == m_blocks.end()) {
        return false;
    }
    // `carved` = 被挖过；`material` 非空 = 写过体素材质（塌落搬来的残骸落在该块）。
    // 两者都属于"这个块已经与纯函数推导值不同"⇒ 卸载会丢玩家改动。
    return found->second.carved || !found->second.material.empty();
}

std::vector<BlockCoord> DigVolumeWorld::ResidentBlocks() const {
    std::vector<BlockCoord> coords;
    coords.reserve(m_blocks.size());
    for (const auto& entry : m_blocks) {
        coords.push_back(entry.first);  // `std::map` 按 key 升序 ⇒ 天然升序
    }
    return coords;
}

bool DigVolumeWorld::CarveSphere(const glm::dvec3& center, float radiusBlocks, std::vector<BlockCoord>& dirtyOut,
                                 VoxelBounds* boundsOut) {
    // 半径驱动的挖除（测试 / 工具用；**不**咨询材质）：等价于 T31 引入伤害模型之前的判据。
    // 不传掩码 ⇒ 不做爆炸波遮挡（这是"工具"语义，与玩家爆炸的 `CarveByDamage` 不同）。
    return RasterizeBall(center, radiusBlocks, /*skipIndestructible*/false, /*mask*/nullptr, dirtyOut, boundsOut);
}

bool DigVolumeWorld::CarveByDamage(const glm::dvec3& center, float radiusBlocks, int budgetPoints,
                                   std::vector<BlockCoord>& dirtyOut, VoxelBounds* boundsOut) {
    if (boundsOut != nullptr) {
        *boundsOut = VoxelBounds {};
    }
    if (!(radiusBlocks > 0.0F) || budgetPoints <= 0 || m_blocks.empty()) {
        return false;
    }

    // ① 候选格³：格心距球心 ≤ radius（格心 = 整数格坐标 + 0.5）。
    // T79④：候选表、掩码与三张缓存、洪泛栈**全部复用成员 scratch**（每次调用完整覆盖 ⇒ 结果不变，
    // 但每发的堆分配降为稳态零；见 `dig_volume.hpp` 的 `CarveScratch`）。
    CarveScratch&      scratch = m_carveScratch;
    std::vector<CarveCandidate>& cells = scratch.cells;
    const double radius  = static_cast<double>(radiusBlocks);
    const double radiusSq = radius * radius;
    cells.clear();
    const int loX = static_cast<int>(std::floor(center.x - radius));
    const int hiX = static_cast<int>(std::ceil(center.x + radius));
    const int loY = static_cast<int>(std::floor(center.y - radius));
    const int hiY = static_cast<int>(std::ceil(center.y + radius));
    const int loZ = static_cast<int>(std::floor(center.z - radius));
    const int hiZ = static_cast<int>(std::ceil(center.z + radius));
    for (int z = loZ; z <= hiZ; ++z) {
        for (int y = loY; y <= hiY; ++y) {
            for (int x = loX; x <= hiX; ++x) {
                const double dx = static_cast<double>(x) + 0.5 - center.x;
                const double dy = static_cast<double>(y) + 0.5 - center.y;
                const double dz = static_cast<double>(z) + 0.5 - center.z;
                const double distanceSq = dx * dx + dy * dy + dz * dz;
                if (distanceSq > radiusSq) {
                    continue;
                }
                cells.push_back(CarveCandidate { std::sqrt(distanceSq), x, y, z });
            }
        }
    }
    // 确定序（红线 7）：距离升序 → (x, y, z) 升序 ⇒ 同输入永远同一结果。
    std::sort(cells.begin(), cells.end(), [](const CarveCandidate& a, const CarveCandidate& b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        if (a.x != b.x) {
            return a.x < b.x;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.z < b.z;
    });

    // ② **爆炸波可达性洪泛**（T53）：爆炸波是标量扩散，**不可破坏材质（岩）= 遮挡体**，
    //    波到不了岩后面 ⇒ 那些格既不参与预算结算、也不被栅格化（④ 传掩码）。
    //
    //    掩码定义在**球 + 过渡带**的球体内（`RasterizeBall` 会写到球面外 `kCarveBandBlocks` 一圈，
    //    那一圈的格也必须判可达；球体之外的格一律视为**不可达** —— 它们既不可能被栅格化，
    //    也不该被当成"波的绕行通道"）。同时把每格的**材质槽位 / 实心性**缓存下来，
    //    洪泛与预算结算共用**一次**采样（改前预算循环还要再采一轮材质 + 密度）。
    BlastMask& mask = scratch.mask;  // T79④：复用掩码缓冲（`assign` 在容量足够时不重新分配）
    const double blastReach    = radius + kCarveBandBlocks;
    const double blastReachSq  = blastReach * blastReach;
    mask.minX  = static_cast<int>(std::floor(center.x - blastReach));
    mask.minY  = static_cast<int>(std::floor(center.y - blastReach));
    mask.minZ  = static_cast<int>(std::floor(center.z - blastReach));
    mask.sizeX = static_cast<int>(std::ceil(center.x + blastReach)) - mask.minX + 1;
    mask.sizeY = static_cast<int>(std::ceil(center.y + blastReach)) - mask.minY + 1;
    mask.sizeZ = static_cast<int>(std::ceil(center.z + blastReach)) - mask.minZ + 1;
    const std::size_t maskCells = static_cast<std::size_t>(mask.sizeX) * static_cast<std::size_t>(mask.sizeY) *
                                  static_cast<std::size_t>(mask.sizeZ);
    mask.reachable.assign(maskCells, 0U);
    std::vector<std::uint8_t>& slotOf    = scratch.slotOf;
    std::vector<std::uint8_t>& solidOf   = scratch.solidOf;
    std::vector<std::uint8_t>& blockerOf = scratch.blockerOf;
    slotOf.assign(maskCells, static_cast<std::uint8_t>(kNoMaterialSlot));
    solidOf.assign(maskCells, 0U);
    blockerOf.assign(maskCells, 1U);  // 先全部视为遮挡体（球体之外不采样、也不通行）
    for (int z = mask.minZ; z < mask.minZ + mask.sizeZ; ++z) {
        for (int y = mask.minY; y < mask.minY + mask.sizeY; ++y) {
            for (int x = mask.minX; x < mask.minX + mask.sizeX; ++x) {
                const double dx = static_cast<double>(x) - center.x;
                const double dy = static_cast<double>(y) - center.y;
                const double dz = static_cast<double>(z) - center.z;
                if (dx * dx + dy * dy + dz * dz > blastReachSq) {
                    continue;  // 球体之外：保持"遮挡体"（不可通行、也不参与结算）
                }
                const std::size_t  index = mask.Index(x, y, z);
                const std::uint8_t slot  = SampleMaterialSlot(x, y, z);
                const bool solid = SampleDensity(static_cast<double>(x), static_cast<double>(y),
                                                 static_cast<double>(z)) < 0.0F;
                // 不可破坏的判据与 `IsIndestructibleSample` 同口径（`kNoMaterialSlot` = 不属于可挖体积）。
                bool indestructible = true;
                if (slot != kNoMaterialSlot) {
                    const MaterialLayer& layer = Materials().Layer(static_cast<int>(slot));
                    indestructible             = layer.indestructible || !(layer.toughness > 0.0F);
                }
                slotOf[index]    = slot;
                solidOf[index]   = solid ? 1U : 0U;
                blockerOf[index] = (solid && indestructible) ? 1U : 0U;
            }
        }
    }

    // 洪泛：从爆心所在格出发做 6 邻域扩散（**不用对角** —— 对角会让波从两块岩的角缝里漏过去）。
    // 用显式栈而不是递归：深度可达数万格，递归会爆栈且不确定。可达集合与遍历次序无关（确定性）。
    {
        const int        originX = static_cast<int>(std::floor(center.x));
        const int        originY = static_cast<int>(std::floor(center.y));
        const int        originZ = static_cast<int>(std::floor(center.z));
        std::vector<int>& stack   = scratch.floodStack;  // T79④：复用栈缓冲（每发完整重建）
        stack.clear();
        if (mask.Contains(originX, originY, originZ)) {
            const std::size_t originIndex = mask.Index(originX, originY, originZ);
            if (blockerOf[originIndex] == 0U) {  // 爆心落在岩体内 ⇒ 一格都进不去（波不外泄）
                mask.reachable[originIndex] = 1U;
                stack.push_back(static_cast<int>(originIndex));
            }
        }
        const int planeStride = mask.sizeX * mask.sizeY;
        while (!stack.empty()) {
            const int index = stack.back();
            stack.pop_back();
            // 解码必须**加回掩码原点**（`Index` 存的是相对 `min*` 的偏移，漏掉这一步会让整条洪泛停在盒子角落）。
            const int x = mask.minX + index % mask.sizeX;
            const int y = mask.minY + (index / mask.sizeX) % mask.sizeY;
            const int z = mask.minZ + index / planeStride;
            const int neighbours[6][3] = { { x - 1, y, z }, { x + 1, y, z }, { x, y - 1, z },
                                           { x, y + 1, z }, { x, y, z - 1 }, { x, y, z + 1 } };
            for (const int(&next)[3] : neighbours) {
                if (!mask.Contains(next[0], next[1], next[2])) {
                    continue;
                }
                const std::size_t nextIndex = mask.Index(next[0], next[1], next[2]);
                if (mask.reachable[nextIndex] != 0U || blockerOf[nextIndex] != 0U) {
                    continue;
                }
                mask.reachable[nextIndex] = 1U;
                stack.push_back(static_cast<int>(nextIndex));
            }
        }
    }

    // ③ 自爆心向外逐格³ 扣减（**整数点**），**只结算可达的格**；余额不足以破坏下一格 ⇒ 立即停止。
    int         remaining  = budgetPoints;
    double      stopRadius = 0.0;
    std::size_t destroyed  = 0;
    for (const CarveCandidate& cell : cells) {
        // 候选格的**格心**距球心 ≤ radius，故它的**采样点**距球心 ≤ radius + √3/2 < radius + 1.5
        // ⇒ 一定落在掩码球内，`Index` 不会越界（掩码球半径 = radius + `kCarveBandBlocks`）。
        const std::size_t index = mask.Index(cell.x, cell.y, cell.z);
        // T53：**被岩石遮挡**的格 ⇒ 跳过，且**不消耗预算**（近侧该挖多少还是多少）。
        // 「不可达」已包含「实心 ∧ 不可破坏」这一整类（它们正是洪泛的遮挡体），故此处不再另判不可破坏。
        if (mask.reachable[index] == 0U) {
            continue;
        }
        const std::uint8_t slot = slotOf[index];
        if (slot == kNoMaterialSlot) {
            continue;  // 不属于可挖体积
        }
        if (solidOf[index] == 0U) {
            continue;  // 已经是空（空气）：无物可破坏 ⇒ **不消耗**预算（否则在空气中爆炸会"空烧"预算、把腔体算大）
        }
        // T59 / ADR 0020 决策一：**地表以下超出带宽**的格不可挖 ⇒ 跳过，且**不消耗预算**
        // （与"被岩石遮挡"、"已是空气"同口径：近侧该挖多少还是多少）。
        if (BelowDiggableBand(cell.x, cell.y, cell.z)) {
            continue;
        }
        const MaterialLayer& layer = Materials().Layer(static_cast<int>(slot));
        const int            cost  = static_cast<int>(std::lround(layer.toughness));  // 点 / 格³ → 整数点
        if (cost > remaining) {
            break;  // 该格及其更远者保留
        }
        remaining -= cost;
        stopRadius = cell.distance;
        ++destroyed;
    }
    if (destroyed == 0) {
        return false;  // 一格都挖不动（例如全落在不可破坏材质里 / 爆心在岩体内）⇒ 无改动是正确结果
    }

    // ④ 以"最后一个被破坏的格"的半径为**光滑球面半径**按掩码栅格化：
    //    不可破坏材质（T31）与**爆炸波到不了**的采样（T53）都保持原状。
    return RasterizeBall(center, static_cast<float>(stopRadius), /*skipIndestructible*/true, &mask, dirtyOut,
                         boundsOut);
}

bool DigVolumeWorld::RemeshBlock(const BlockCoord& coord) {
    if (m_blocks.find(coord) == m_blocks.end()) {
        return false;
    }
    MeshBlock(coord);
    return true;
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

std::size_t DigVolumeWorld::MaterialBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& entry : m_blocks) {
        bytes += entry.second.material.size();
    }
    return bytes;
}

const TerrainMaterialTable& DigVolumeWorld::Materials() const noexcept {
    return m_terrain.Materials();
}

// ---------------------------------------------------------------------------
// T81 / ADR 0022：块构建下沉 worker —— 快照采集 + 纯函数构建 + 安装
// ---------------------------------------------------------------------------

BlockBuildResult BuildBlockFromInput(const BlockBuildInput& input) {
    vx::Clock computeClock;  // 观测：worker 侧计算耗时（不影响任何判据）

    BlockBuildResult result;
    result.coord = input.coord;

    // 世界 Y 原点（密度推导要"世界高度"）。
    const int originY = BlockOriginBlocks(input.coord.y);

    // ① **填密度**（原 `FillBlockDensity` 的工作）：内部 33³ 采样由**列高度补丁**推导。
    //    公式与同步路径**共用** `TerrainDerivedDensityFromHeight` ⇒ 逐位一致（红线 7）。
    result.density.assign(static_cast<std::size_t>(kVolumeSampleCount) * static_cast<std::size_t>(kVolumeSampleCount) *
                              static_cast<std::size_t>(kVolumeSampleCount),
                          0);
    for (int k = 0; k <= kVolumeBlockSize; ++k) {
        for (int j = 0; j <= kVolumeBlockSize; ++j) {
            for (int i = 0; i <= kVolumeBlockSize; ++i) {
                const float height =
                    input.surfaceHeights[static_cast<std::size_t>(i + 1) +
                                         BlockBuildInput::kPatchSize * static_cast<std::size_t>(k + 1)];
                result.density[DensityIndex(i, j, k)] =
                    static_cast<std::int8_t>(std::lround(TerrainDerivedDensityFromHeight(
                        height, static_cast<double>(originY + j))));
            }
        }
    }

    // ② **网格化**：采样器直接读**快照**（内部密度 + 壳层 + 材质补丁），不触碰世界对象。
    //    采样索引范围与 `BuildVolumeMesh` 的读取范围一致：局部 `-1..kVolumeBlockSize`。
    struct SnapshotSampler final : public IVolumeSampler {
        const BlockBuildInput&          input;
        const std::vector<std::int8_t>& density;

        SnapshotSampler(const BlockBuildInput& inputRef, const std::vector<std::int8_t>& densityRef) noexcept
            : input(inputRef), density(densityRef) {}

        [[nodiscard]] float Sample(int i, int j, int k) const override {
            const auto inside = [](int value) noexcept { return value >= 0 && value <= kVolumeBlockSize; };
            if (inside(i) && inside(j) && inside(k)) {
                return static_cast<float>(density[DensityIndex(i, j, k)]);
            }
            return static_cast<float>(
                input.density[static_cast<std::size_t>(i + 1) +
                              BlockBuildInput::kSampleExtent *
                                  (static_cast<std::size_t>(j + 1) +
                                   BlockBuildInput::kSampleExtent * static_cast<std::size_t>(k + 1))]);
        }

        /// 材质：壳层取"邻块已写入值"（非 `kNoMaterialSlot` 时优先），否则——**内部与壳层一样**——
        /// 回落**列派生**补丁。口径与 `SampleMaterialSlot` 的两级判定逐字一致
        /// （新建块从未写过材质 ⇒ 内部必然走回落分支）。
        [[nodiscard]] std::uint8_t SampleMaterial(int i, int j, int k) const override {
            const auto inside = [](int value) noexcept { return value >= 0 && value <= kVolumeBlockSize; };
            if (!(inside(i) && inside(j) && inside(k))) {
                const std::size_t sampleIndex =
                    static_cast<std::size_t>(i + 1) +
                    BlockBuildInput::kSampleExtent *
                        (static_cast<std::size_t>(j + 1) +
                         BlockBuildInput::kSampleExtent * static_cast<std::size_t>(k + 1));
                const std::uint8_t stored = input.storedMaterial[sampleIndex];
                if (stored != kNoMaterialSlot) {
                    return stored;
                }
            }
            return input.columnMaterial[static_cast<std::size_t>(i + 1) +
                                        BlockBuildInput::kPatchSize * static_cast<std::size_t>(k + 1)];
        }
    } sampler { input, result.density };

    result.mesh = BuildVolumeMesh(sampler);
    result.fill = ClassifyFill(result.density);
    result.computeMs = computeClock.Tick() * 1000.0;
    return result;
}

BlockBuildInput DigVolumeWorld::CaptureBlockBuildInput(const BlockCoord& coord) const {
    BlockBuildInput input;
    input.coord = coord;
    // **必须先填哨兵**：`kNoMaterialSlot` = 0xFF，而 `std::array` 的值初始化是 0 —— 而 0 是一个**合法材质槽位**
    //（草）。若不清成哨兵，未写入的采样会被误判成"邻块已写入材质 0" ⇒ 与同步路径的材质不一致。
    input.storedMaterial.fill(kNoMaterialSlot);

    constexpr int kPatch = BlockBuildInput::kPatchSize;  // 35（局部列 -1..33）
    const int     originX = BlockOriginBlocks(coord.x);
    const int     originY = BlockOriginBlocks(coord.y);
    const int     originZ = BlockOriginBlocks(coord.z);

    // ① 列补丁：地表高度 + 列派生材质（后者**直接调用**世界的实现 ⇒ 与同步路径同一份代码）。
    for (int kz = -1; kz <= kVolumeBlockSize + 1; ++kz) {
        for (int ix = -1; ix <= kVolumeBlockSize + 1; ++ix) {
            const int worldX = originX + ix;
            const int worldZ = originZ + kz;
            const std::size_t index =
                static_cast<std::size_t>(ix + 1) + static_cast<std::size_t>(kPatch) * static_cast<std::size_t>(kz + 1);

            float height = 0.0F;
            input.surfaceHeights[index] =
                SurfaceHeight(static_cast<double>(worldX), static_cast<double>(worldZ), height)
                    ? height
                    : std::numeric_limits<float>::quiet_NaN();

            std::uint8_t slot = kNoMaterialSlot;
            (void)m_terrain.QueryDigMaterialSlot(static_cast<float>(worldX), static_cast<float>(worldZ), slot);
            input.columnMaterial[index] = slot;
        }
    }

    // ② 采样壳层（局部 `-1` 那一圈，34³ − 33³ = 3367 个）：邻块存在 ⇒ 直接取它的密度 / 已写入材质；
    //    邻块不存在 ⇒ 密度按列补丁推导（与 `BlockSampler` 的越界回落同一口径）、材质留 `kNoMaterialSlot`
    //    （⇒ worker 侧回落列派生补丁）。内部 33³ 刻意**留空**：那正是要交给 worker 的"填密度"工作。
    for (int k = -1; k <= kVolumeBlockSize; ++k) {
        for (int j = -1; j <= kVolumeBlockSize; ++j) {
            for (int i = -1; i <= kVolumeBlockSize; ++i) {
                if (i >= 0 && j >= 0 && k >= 0) {
                    continue;  // 内部：留给 worker 填
                }
                const std::size_t sampleIndex =
                    static_cast<std::size_t>(i + 1) +
                    BlockBuildInput::kSampleExtent *
                        (static_cast<std::size_t>(j + 1) +
                         BlockBuildInput::kSampleExtent * static_cast<std::size_t>(k + 1));

                const int worldX = originX + i;
                const int worldY = originY + j;
                const int worldZ = originZ + k;
                const BlockCoord neighbour { BlockIndexOf(static_cast<double>(worldX)),
                                             BlockIndexOf(static_cast<double>(worldY)),
                                             BlockIndexOf(static_cast<double>(worldZ)) };

                const auto found = m_blocks.find(neighbour);
                if (found != m_blocks.end()) {
                    const VolumeBlock& block = found->second;
                    input.density[sampleIndex] =
                        block.density[DensityIndex(worldX - BlockOriginBlocks(block.coord.x),
                                                   worldY - BlockOriginBlocks(block.coord.y),
                                                   worldZ - BlockOriginBlocks(block.coord.z))];
                    if (!block.material.empty()) {
                        input.storedMaterial[sampleIndex] =
                            block.material[DensityIndex(worldX - BlockOriginBlocks(block.coord.x),
                                                        worldY - BlockOriginBlocks(block.coord.y),
                                                        worldZ - BlockOriginBlocks(block.coord.z))];
                    }
                } else {
                    const float height =
                        input.surfaceHeights[static_cast<std::size_t>(i + 1) +
                                             static_cast<std::size_t>(kPatch) * static_cast<std::size_t>(k + 1)];
                    input.density[sampleIndex] = static_cast<std::int8_t>(std::lround(
                        TerrainDerivedDensityFromHeight(height, static_cast<double>(worldY))));
                }
            }
        }
    }
    return input;
}

bool DigVolumeWorld::InstallBuiltBlock(BlockBuildResult&& result) {
    // **无论安装成功与否都要清掉"在飞"记录**：否则该坐标一旦被卸载 / 走远，
    // `CreateBlock` 会误判"它还在飞"而拒绝重新提交 ⇒ 玩家回头时块永远建不出来。
    m_pendingBuilds.erase(result.coord);
    if (m_blocks.find(result.coord) != m_blocks.end()) {
        return false;  // 已被同步路径建过 / 窗口又移动过 ⇒ 丢弃（不静默使用陈旧结果）
    }
    VolumeBlock block;
    block.coord   = result.coord;
    block.density = std::move(result.density);
    block.mesh    = std::move(result.mesh);
    block.carved  = false;
    block.fill    = result.fill;
    m_blocks.emplace(result.coord, std::move(block));
    return true;
}

std::size_t DigVolumeWorld::PollBlockBuildsAndInstall(std::vector<BlockCoord>& installedOut, std::size_t maxResults) {
    if (m_buildPipeline == nullptr || maxResults == 0U) {
        return 0;
    }
    std::size_t installed = 0;
    // 逐个取回（`maxResults` 有界 ⇒ 单帧主线程成本有上界：一次安装只是几次 move，不含任何计算）。
    while (installed < maxResults) {
        BlockBuildResult result;
        if (!m_buildPipeline->TakeCompleted(result)) {
            break;
        }
        const BlockCoord coord = result.coord;  // 安装会把密度 / 网格 move 走，坐标先取出
        if (InstallBuiltBlock(std::move(result))) {
            installedOut.push_back(coord);
            ++installed;
        }
    }
    return installed;
}

}  // namespace vx
