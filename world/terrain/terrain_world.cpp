#include "terrain/terrain_world.hpp"

#include "terrain/material_blender.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

namespace vx {
namespace {

/// 遮挡查询的采样步长（格）：越小越精确，越大越省。
constexpr float kObstructionStepBlocks = 0.25F;

/// 世界列 → 所在 tile 索引（向下取整，负坐标也正确）。
[[nodiscard]] constexpr int TileIndexOfColumn(int column) noexcept {
    const int quotient  = column / kTerrainTileSize;
    const int remainder = column % kTerrainTileSize;
    return (remainder != 0 && ((remainder < 0) != (kTerrainTileSize < 0))) ? (quotient - 1) : quotient;
}

/// 可能持有世界列 `(worldX, worldZ)` 的 tile 候选（**最多 4 个**，升序 = `std::map` 的遍历顺序）。
///
/// 判据：`TileContainsColumn` 是"列 ∈ `[tile 原点, tile 原点 + kTerrainTileSize]`"（**含共享边界列**），
/// 故一个世界列每轴至多被 **2 个** tile 持有 ⇒ 候选 = `{floorDiv(c, 64), floorDiv(c, 64) − 1}` 的笛卡尔积。
///
/// 为什么需要（T79①）：`QueryHeight` 是**每帧热路径**（相机避障 / 弹道 / 材质派生），而
/// `ReadColumnHeight` / `WriteColumnHeight` 原先**线性遍历全部已加载 tile**（1 km 世界 = 289 tile
/// ⇒ 每次查询 289 次 map 命中 + 包围盒判定）。改成"由整数除法直接给出候选"后是 **O(1)**，
/// 且**语义逐位不变**（含共享边界列会被全部命中；返回值与写入集合与遍历全部 tile 时一致）。
[[nodiscard]] std::array<std::pair<int, int>, 4> ColumnTileCandidates(int worldX, int worldZ) noexcept {
    const int tileX = TileIndexOfColumn(worldX);
    const int tileZ = TileIndexOfColumn(worldZ);
    // 顺序 = `TileCoord` 的字典序（x 优先）⇒ 与旧实现"按 map 顺序取第一个命中的 tile"完全一致。
    return { std::pair<int, int> { tileX - 1, tileZ - 1 }, std::pair<int, int> { tileX - 1, tileZ },
             std::pair<int, int> { tileX, tileZ - 1 }, std::pair<int, int> { tileX, tileZ } };
}

}  // namespace

// 重算并写回 tile 的高度缓存（O(65²)）。只在**生成**与**重网格**时调用，
// 因此 `MaxSurfaceHeightBlocks()` 得以从"每帧 O(tile × 顶点)"降为"每帧 O(tile)"。
//
// V4 起公开：worker（`TerrainTileBuildPipeline`）与主线程（`TerrainWorld`）在"高度刚被填好"之后
// 都要刷新它 —— 无论高度是**生成**来的还是从**预制文件读**来的（两条路径共用本实现 ⇒ 不会分叉）。
void RefreshTileMaxSurfaceBlocks(TerrainTile& tile) noexcept {
    float maximum = 0.0F;
    for (const Height height : tile.heights) {
        const float blocks = HeightToBlocks(height);
        maximum            = (blocks > maximum) ? blocks : maximum;
    }
    tile.maxSurfaceBlocks = maximum;
}

TerrainTile GenerateTerrainTileData(const TerrainNoiseGenerator& noise, const std::vector<MapEdit>& edits,
                                    int tileX, int tileZ) {
    TerrainTile tile;
    tile.coord = TileCoord { tileX, tileZ };
    GenerateTerrainTile(tile, noise);
    // 预设地图：噪声先行，编辑按文件顺序覆盖其上（T11；纯函数，边界列逐位一致）。
    ApplyMapEditsToTile(edits, tile);
    RefreshTileMaxSurfaceBlocks(tile);
    return tile;
}

TerrainWorld::TerrainWorld(std::uint64_t worldSeed, TerrainMaterialTable materials, TerrainGenerationParams params)
    : m_seed(worldSeed), m_materials(std::move(materials)), m_noise(worldSeed, std::move(params)) {}

void TerrainWorld::SetMapPreset(const MapPreset& preset) {
    m_mapEdits = preset.edits;
}

void TerrainWorld::GenerateTile(int tileX, int tileZ) {
    // W7-S3b：生成逻辑抽为自由函数 `GenerateTerrainTileData`（worker 与主线程**共用同一份实现** ⇒ 逐位一致）。
    // V4（ADR 0026）：若设了**数据来源**（预制地图）则优先读它；来源缺该 tile ⇒ **回退程序化生成**
    // （来源侧负责一次性告警；这里不静默吞掉，也不假装来源成功）。
    const TileCoord coord { tileX, tileZ };
    TerrainTile     tile;
    tile.coord = coord;
    // 来源返回 true ⇒ 该 tile **完整可用**（含 `maxSurfaceBlocks`，见 `ITerrainTileSource` 契约）⇒ 不再重复刷新。
    const bool filledFromSource = (m_tileSource != nullptr) && m_tileSource->FillTileHeights(tile);
    if (!filledFromSource) {
        tile = GenerateTerrainTileData(m_noise, m_mapEdits, tileX, tileZ);  // 程序化路径自带缓存刷新
    }
    m_tiles[coord] = std::move(tile);
}

void TerrainWorld::MeshTile(int tileX, int tileZ, int lodLevel) {
    const auto found = m_tiles.find(TileCoord { tileX, tileZ });
    if (found == m_tiles.end()) {
        return;
    }
    TerrainTile& tile = found->second;
    // 高度缓存在这里刷新：`MeshTile` 是"生成后"与"笔刷改动后"（经 `RemeshDirtyTiles`）的**唯一汇合点**。
    RefreshTileMaxSurfaceBlocks(tile);
    m_meshes[TileCoord { tileX, tileZ }] = BuildTerrainMesh(tile, m_quadFilter, lodLevel);
}

void TerrainWorld::LoadTile(int tileX, int tileZ, int lodLevel) {
    const TileCoord coord { tileX, tileZ };
    const auto      staged = m_staged.find(coord);
    if (staged != m_staged.end() && staged->second.mesh.lodLevel == lodLevel) {
        // **命中预取缓存 ⇒ 只做安装**（廉价）：装高度 + 按**当前**过滤器过滤网格（主线程）。
        // 过滤在安装时（而非 worker 里）做，语义与"同步生成时过滤"**逐位一致**（红线 7 / ADR 0011）。
        TerrainTile& tile = m_tiles[coord];
        tile              = std::move(staged->second.tile);
        ApplyQuadFilterToMesh(tile, m_quadFilter, lodLevel, staged->second.mesh.mesh);
        staged->second.mesh.meshEmpty = staged->second.mesh.mesh.indices.empty();
        m_meshes[coord]               = std::move(staged->second.mesh);
        m_staged.erase(staged);
        return;
    }
    if (staged != m_staged.end()) {
        m_staged.erase(staged);  // LOD 不符 ⇒ 丢弃陈旧条目（不静默使用），走同步路径重建
    }
    // 缓存未命中 ⇒ 回退到同步生成（记一条计数便于观测；线程池不可用时这是预期路径）。
    ++m_syncFallbackCount;
    GenerateTile(tileX, tileZ);
    MeshTile(tileX, tileZ, lodLevel);
}

void TerrainWorld::StageTile(TerrainTile tile, TerrainTileMesh mesh) {
    const TileCoord coord = tile.coord;
    StagedTerrainTile staged;
    staged.tile = std::move(tile);
    staged.mesh = std::move(mesh);
    m_staged[coord] = std::move(staged);
}

bool TerrainWorld::HasStagedTile(int tileX, int tileZ, int lodLevel) const noexcept {
    const auto found = m_staged.find(TileCoord { tileX, tileZ });
    return found != m_staged.end() && found->second.mesh.lodLevel == lodLevel;
}

bool TerrainWorld::InstallRemeshedMesh(const TileCoord& coord, int lodLevel, TerrainTileMesh mesh) {
    const auto found = m_tiles.find(coord);
    if (found == m_tiles.end()) {
        return false;  // 已卸载 ⇒ 丢弃（不静默使用陈旧数据）
    }
    if (mesh.lodLevel != lodLevel) {
        return false;  // LOD 不符 ⇒ 丢弃
    }
    ApplyQuadFilterToMesh(found->second, m_quadFilter, lodLevel, mesh.mesh);
    mesh.meshEmpty      = mesh.mesh.indices.empty();
    m_meshes[coord]     = std::move(mesh);
    return true;
}

void TerrainWorld::ReleaseTileMeshCpu(int tileX, int tileZ) {
    const auto found = m_meshes.find(TileCoord { tileX, tileZ });
    if (found == m_meshes.end()) {
        return;
    }
    // 只放掉顶点 / 索引缓冲；`coord` / `lodLevel` / `verticesPerSide` / `meshEmpty` 全部保留。
    MeshData& mesh = found->second.mesh;
    if (!mesh.vertices.empty()) {
        mesh.vertices.clear();
        mesh.vertices.shrink_to_fit();
    }
    if (!mesh.indices.empty()) {
        mesh.indices.clear();
        mesh.indices.shrink_to_fit();
    }
}

bool TerrainWorld::UnloadTile(int tileX, int tileZ) {
    const TileCoord coord { tileX, tileZ };
    m_staged.erase(coord);  // 预取缓存里若有同坐标条目也一并丢弃（避免陈旧）
    m_meshes.erase(coord);  // 网格先于高度释放（两者都按坐标键；顺序不影响结果，保持与"建"相反）
    return m_tiles.erase(coord) > 0U;
}

std::vector<TileCoord> TerrainWorld::ResidentTiles() const {
    std::vector<TileCoord> coords;
    coords.reserve(m_tiles.size());
    for (const auto& entry : m_tiles) {
        coords.push_back(entry.first);  // `std::map` 按 `TileCoord::operator<` 升序 ⇒ 结果天然升序
    }
    return coords;
}

bool TerrainWorld::HasTile(int tileX, int tileZ) const noexcept {
    return m_tiles.find(TileCoord { tileX, tileZ }) != m_tiles.end();
}

const TerrainTile* TerrainWorld::FindTile(int tileX, int tileZ) const noexcept {
    const auto found = m_tiles.find(TileCoord { tileX, tileZ });
    return (found != m_tiles.end()) ? &found->second : nullptr;
}

const TerrainTileMesh* TerrainWorld::FindMesh(int tileX, int tileZ) const noexcept {
    const auto found = m_meshes.find(TileCoord { tileX, tileZ });
    return (found != m_meshes.end()) ? &found->second : nullptr;
}

bool TerrainWorld::ReadColumnHeight(int worldX, int worldZ, Height& outHeight) const noexcept {
    // T79①：由整数除法直接给出**最多 4 个**候选 tile（不再遍历全部已加载 tile）。
    for (const std::pair<int, int>& candidate : ColumnTileCandidates(worldX, worldZ)) {
        const auto found = m_tiles.find(TileCoord { candidate.first, candidate.second });
        if (found == m_tiles.end()) {
            continue;
        }
        const TerrainTile& tile = found->second;
        if (!TileContainsColumn(tile, worldX, worldZ)) {
            continue;  // 候选里只有真正含该列的才命中（共享边界列会被两个 tile 同时命中）
        }
        outHeight = tile.At(worldX - TileOriginColumn(tile.coord.x), worldZ - TileOriginColumn(tile.coord.z));
        return true;
    }
    return false;
}

void TerrainWorld::WriteColumnHeight(int worldX, int worldZ, Height height, std::vector<TileCoord>& dirtyOut) {
    const int clamped = std::clamp(static_cast<int>(height), kMinTerrainHeightUnits, kMaxTerrainHeightUnits);
    const Height value = static_cast<Height>(clamped);

    // T79①：同 `ReadColumnHeight` —— 只查最多 4 个候选；**共享边界列仍写进全部持有它的 tile**
    //（否则相邻 tile 在该列上不再逐位相等 ⇒ 出现裂缝，红线 12）。
    for (const std::pair<int, int>& candidate : ColumnTileCandidates(worldX, worldZ)) {
        const auto found = m_tiles.find(TileCoord { candidate.first, candidate.second });
        if (found == m_tiles.end()) {
            continue;
        }
        TerrainTile& tile = found->second;
        if (!TileContainsColumn(tile, worldX, worldZ)) {
            continue;
        }
        tile.SetAt(worldX - TileOriginColumn(tile.coord.x), worldZ - TileOriginColumn(tile.coord.z), value);
        dirtyOut.push_back(tile.coord);
    }
}

std::size_t TerrainWorld::RemeshDirtyTiles(const std::vector<TileCoord>& dirty) {
    std::set<TileCoord> uniqueTiles;
    std::size_t         remeshed = 0;
    for (const TileCoord& coord : dirty) {
        if (!uniqueTiles.insert(coord).second) {
            continue;
        }
        if (FindTile(coord.x, coord.z) == nullptr) {
            continue;
        }
        MeshTile(coord.x, coord.z);
        ++remeshed;
    }
    return remeshed;
}

float TerrainWorld::MaxSurfaceHeightBlocks() const noexcept {
    // 只遍历 tile（每 tile 的顶点最大值已由 `MeshTile` / `GenerateTile` 缓存）：
    // 每帧成本与 **tile 数**成正比，而不是与"tile × 顶点"成正比（见 `TerrainTile::maxSurfaceBlocks`）。
    float maximum = 0.0F;
    for (const auto& entry : m_tiles) {
        const float blocks = entry.second.maxSurfaceBlocks;
        maximum            = (blocks > maximum) ? blocks : maximum;
    }
    return maximum;
}

bool TerrainWorld::QueryHeight(float worldX, float worldZ, float& outHeight) const {
    const int columnX = static_cast<int>(std::floor(worldX));
    const int columnZ = static_cast<int>(std::floor(worldZ));

    Height height = 0;
    if (!ReadColumnHeight(columnX, columnZ, height)) {
        return false;
    }
    outHeight = HeightToBlocks(height);
    return true;
}

bool TerrainWorld::IsSolid(const glm::vec3& point) const {
    float height = 0.0F;
    if (!QueryHeight(point.x, point.z, height)) {
        return false;  // 未加载 / 无数据：不阻挡（口径与 QueryObstruction 一致）
    }
    return point.y <= height;
}

bool TerrainWorld::QueryDigMaterialSlot(float worldX, float worldZ, std::uint8_t& outSlot) const {
    float height = 0.0F;
    if (!QueryHeight(worldX, worldZ, height)) {
        return false;
    }

    // 坡度 = 1 - normal.y（与地表着色同口径）：用高度场中心差分求切线，再算法线的 y 分量。
    constexpr float kStep = 1.0F;
    float           hx0   = 0.0F;
    float           hx1   = 0.0F;
    float           hz0   = 0.0F;
    float           hz1   = 0.0F;
    const bool      okX   = QueryHeight(worldX - kStep, worldZ, hx0) && QueryHeight(worldX + kStep, worldZ, hx1);
    const bool      okZ   = QueryHeight(worldX, worldZ - kStep, hz0) && QueryHeight(worldX, worldZ + kStep, hz1);
    const float     dx    = okX ? (hx1 - hx0) / (2.0F * kStep) : 0.0F;
    const float     dz    = okZ ? (hz1 - hz0) / (2.0F * kStep) : 0.0F;
    const float     normalY = 1.0F / std::sqrt(1.0F + dx * dx + dz * dz);
    const float     slope   = std::clamp(1.0F - normalY, 0.0F, 1.0F);

    const std::array<float, static_cast<std::size_t>(kMaterialSlotCount)> weights =
        ComputeBlendWeights(m_materials, height, slope);
    std::size_t best = 0;
    for (std::size_t i = 1; i < weights.size(); ++i) {
        if (weights[i] > weights[best]) {
            best = i;
        }
    }

    // 表层 → 次表层映射（ADR 0014）：`-1` = 用自身（未配置映射的层，以及 Default 表以外的调用方）。
    const int subsurface = m_materials.Layer(static_cast<int>(best)).subsurfaceSlot;
    outSlot = static_cast<std::uint8_t>(subsurface >= 0 ? subsurface : static_cast<int>(best));
    return true;
}

bool TerrainWorld::QueryObstruction(const glm::vec3& from, const glm::vec3& to, float& outSafeT) const {
    outSafeT = 1.0F;

    const glm::vec3 delta  = to - from;
    const float     length = glm::length(delta);

    if (length <= 0.0F) {
        float surface = 0.0F;
        if (QueryHeight(from.x, from.z, surface) && from.y < surface) {
            outSafeT = 0.0F;
            return true;
        }
        return false;
    }

    const int   sampleCount = std::max(1, static_cast<int>(std::ceil(length / kObstructionStepBlocks)));
    float       lastSafeT   = 0.0F;
    for (int step = 0; step <= sampleCount; ++step) {
        const float t     = static_cast<float>(step) / static_cast<float>(sampleCount);
        const glm::vec3 point = from + delta * t;

        float surface = 0.0F;
        if (!QueryHeight(point.x, point.z, surface)) {
            // 无地形数据：不做阻挡判定（流式层保证查询范围内的 tile 已就绪）。
            lastSafeT = t;
            continue;
        }
        if (point.y < surface) {
            outSafeT = lastSafeT;
            return true;
        }
        lastSafeT = t;
    }

    return false;
}

}  // namespace vx
