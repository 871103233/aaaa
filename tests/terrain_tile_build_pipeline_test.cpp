// 地表 tile 构建**下沉 worker**（W7-S3b / ADR 0022 形态）与"上传后释放 CPU 侧网格"的核心测试。
//
// 覆盖：
//   ① `GenerateTerrainTileData` / `BuildTerrainMesh` 是纯函数，worker 路径（含真实线程）与同步路径**逐位一致**；
//   ② `TerrainWorld` 预取缓存的**安装**（含层间交接过滤）与同步构建**逐位一致**；
//   ③ `ApplyQuadFilterToMesh`：`nullptr` 为无操作；命中格子的 6 个索引整段丢弃（独立重建参考实现比对）；
//   ④ 释放 CPU 侧网格后：高度 / `meshEmpty` / `lodLevel` 保留，且能从**高度**重网格（relod / 笔刷复用的前提）；
//   ⑤ `InstallRemeshedMesh`（relod 安装）对"未常驻 / LOD 不符"返回 false（不静默使用陈旧数据）。

#include "streaming/terrain_tile_build_pipeline.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vx::Height;
using vx::HeightToBlocks;
using vx::MeshData;
using vx::MeshVertex;
using vx::TerrainMaterialTable;
using vx::TerrainQuad;
using vx::TerrainTile;
using vx::TerrainTileBuildPipeline;
using vx::TerrainTileBuildRequest;
using vx::TerrainTileBuildResult;
using vx::TerrainTileMesh;
using vx::TerrainWorld;
using vx::TileCoord;

/// ADR 0008 §3 固定基准场景的世界种子（与其它地表测试同源）。
constexpr std::uint64_t kSeed = 0x5EED0001ULL;

/// 逐位比较两个 tile（坐标 + 全部高度 + 最高面缓存）。
void ExpectTileBitEqual(const TerrainTile& a, const TerrainTile& b) {
    EXPECT_EQ(a.coord, b.coord);
    EXPECT_FLOAT_EQ(a.maxSurfaceBlocks, b.maxSurfaceBlocks);
    for (int j = 0; j <= vx::kTerrainTileSize; ++j) {
        for (int i = 0; i <= vx::kTerrainTileSize; ++i) {
            EXPECT_EQ(a.At(i, j), b.At(i, j)) << "列 (" << i << ", " << j << ")";
        }
    }
}

/// 逐位比较两份网格（顶点全字段 + 索引 + 接管标记 + LOD）。
void ExpectMeshBitEqual(const TerrainTileMesh& a, const TerrainTileMesh& b) {
    EXPECT_EQ(a.coord, b.coord);
    EXPECT_EQ(a.lodLevel, b.lodLevel);
    EXPECT_EQ(a.verticesPerSide, b.verticesPerSide);
    EXPECT_EQ(a.meshEmpty, b.meshEmpty);
    ASSERT_EQ(a.mesh.vertices.size(), b.mesh.vertices.size());
    ASSERT_EQ(a.mesh.indices.size(), b.mesh.indices.size());
    for (std::size_t k = 0; k < a.mesh.vertices.size(); ++k) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(a.mesh.vertices[k].position[c], b.mesh.vertices[k].position[c]) << "顶点 " << k;
            EXPECT_FLOAT_EQ(a.mesh.vertices[k].normal[c], b.mesh.vertices[k].normal[c]) << "顶点 " << k;
        }
        EXPECT_FLOAT_EQ(a.mesh.vertices[k].material, b.mesh.vertices[k].material) << "顶点 " << k;
        EXPECT_FLOAT_EQ(a.mesh.vertices[k].morph, b.mesh.vertices[k].morph) << "顶点 " << k;
    }
    for (std::size_t k = 0; k < a.mesh.indices.size(); ++k) {
        EXPECT_EQ(a.mesh.indices[k], b.mesh.indices[k]) << "索引 " << k;
    }
}

/// 只跳过"四角全部落在给定**世界列矩形**内"的格子的过滤器（与 `ShellQuadFilter` 同形）。
class ColumnRectFilter final : public vx::ITerrainQuadFilter {
public:
    ColumnRectFilter(int minColumnX, int maxColumnX, int minColumnZ, int maxColumnZ) noexcept
        : m_minX(minColumnX), m_maxX(maxColumnX), m_minZ(minColumnZ), m_maxZ(maxColumnZ) {}

    [[nodiscard]] bool SkipQuad(const TerrainQuad& quad) const override {
        for (int corner = 0; corner < 4; ++corner) {
            if (quad.columnX[corner] < m_minX || quad.columnX[corner] > m_maxX || quad.columnZ[corner] < m_minZ ||
                quad.columnZ[corner] > m_maxZ) {
                return false;
            }
        }
        return true;
    }

private:
    int m_minX = 0;
    int m_maxX = 0;
    int m_minZ = 0;
    int m_maxZ = 0;
};

/// 独立的参考过滤实现（在测试里重建，**不调用**被测函数）：按 `(j, i)` 升序遍历格子，
/// 命中 `SkipQuad` 则丢弃该格 6 个索引，否则从未过滤列表整段拷入。
[[nodiscard]] std::vector<std::uint32_t> ReferenceFilteredIndices(const TerrainTile& tile,
                                                                  const vx::ITerrainQuadFilter& filter,
                                                                  int lodLevel,
                                                                  const std::vector<std::uint32_t>& unfiltered) {
    const int step = vx::TerrainLodStep(lodLevel);
    const int side = vx::TerrainLodVertexSide(lodLevel);
    const int originColumnX = vx::TileOriginColumn(tile.coord.x);
    const int originColumnZ = vx::TileOriginColumn(tile.coord.z);

    std::vector<std::uint32_t> out;
    std::size_t                quadIndex = 0;
    for (int j = 0; j < side - 1; ++j) {
        for (int i = 0; i < side - 1; ++i, ++quadIndex) {
            const int gi = i * step;
            const int gj = j * step;
            TerrainQuad quad;
            quad.columnX[0] = originColumnX + gi;
            quad.columnZ[0] = originColumnZ + gj;
            quad.columnX[1] = originColumnX + gi + step;
            quad.columnZ[1] = originColumnZ + gj;
            quad.columnX[2] = originColumnX + gi;
            quad.columnZ[2] = originColumnZ + gj + step;
            quad.columnX[3] = originColumnX + gi + step;
            quad.columnZ[3] = originColumnZ + gj + step;
            quad.height[0]  = HeightToBlocks(tile.At(gi, gj));
            quad.height[1]  = HeightToBlocks(tile.At(gi + step, gj));
            quad.height[2]  = HeightToBlocks(tile.At(gi, gj + step));
            quad.height[3]  = HeightToBlocks(tile.At(gi + step, gj + step));
            if (filter.SkipQuad(quad)) {
                continue;
            }
            for (std::size_t k = 0; k < 6U; ++k) {
                out.push_back(unfiltered[quadIndex * 6U + k]);
            }
        }
    }
    return out;
}

}  // namespace

// ① worker 纯函数（同步回退路径）与 `TerrainWorld::LoadTile` 逐位一致。
TEST(TerrainTileWorker, SyncFallbackPipelineMatchesWorldBuildBitForBit) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(1, -2, 0);
    const TerrainTile*     tile = world.FindTile(1, -2);
    const TerrainTileMesh* mesh = world.FindMesh(1, -2);
    ASSERT_NE(tile, nullptr);
    ASSERT_NE(mesh, nullptr);

    // workerThreads=1 ⇒ `TaskScheduler` 视为不可用 ⇒ 提交即同步算完（回退路径）。
    TerrainTileBuildPipeline pipeline(kSeed, vx::TerrainGenerationParams::Default(), {}, /*workerThreads=*/1);
    ASSERT_FALSE(pipeline.HasWorkers());

    TerrainTileBuildRequest request;
    request.coord    = TileCoord { 1, -2 };
    request.lodLevel = 0;
    pipeline.Submit(std::move(request));

    TerrainTileBuildResult result;
    ASSERT_TRUE(pipeline.TakeCompleted(result));
    EXPECT_EQ(result.coord, (TileCoord { 1, -2 }));
    ExpectTileBitEqual(*tile, result.tile);
    ExpectMeshBitEqual(*mesh, result.mesh);
}

// ① worker 真实线程与同步路径逐位一致（真并行下的确定性，红线 7）。
TEST(TerrainTileWorker, RealWorkersProduceBitIdenticalResults) {
    TerrainTileBuildPipeline pipeline(kSeed, vx::TerrainGenerationParams::Default(), {});
    if (!pipeline.HasWorkers()) {
        GTEST_SKIP() << "线程池不可用（回退路径已由上一个用例覆盖）";
    }

    const std::vector<TileCoord> coords = { TileCoord { 0, 0 }, TileCoord { 1, 0 }, TileCoord { -2, 3 },
                                            TileCoord { 4, -4 } };
    for (const TileCoord& coord : coords) {
        TerrainTileBuildRequest request;
        request.coord    = coord;
        request.lodLevel = 0;
        pipeline.Submit(std::move(request));
    }
    // 非阻塞收包 ⇒ 轮询到全部任务结束（4 个 tile，毫秒级）。
    while (pipeline.PendingCount() > 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const vx::TerrainNoiseGenerator noise(kSeed, vx::TerrainGenerationParams::Default());
    std::size_t                     got = 0;
    TerrainTileBuildResult          result;
    while (pipeline.TakeCompleted(result)) {
        const TerrainTile expectedTile = vx::GenerateTerrainTileData(noise, {}, result.coord.x, result.coord.z);
        ExpectTileBitEqual(expectedTile, result.tile);
        const TerrainTileMesh expectedMesh = vx::BuildTerrainMesh(expectedTile, nullptr, result.lodLevel);
        ExpectMeshBitEqual(expectedMesh, result.mesh);
        ++got;
    }
    EXPECT_EQ(got, coords.size());
}

// ② 预取缓存**安装**（含主线程过滤）与同步构建逐位一致；命中缓存 ⇒ 不回退。
TEST(TerrainTileWorker, StagedInstallAppliesQuadFilterIdenticallyToSyncBuild) {
    // 过滤器覆盖 tile (0, 0) 的整个列范围 [0, 64] ⇒ 该 tile 的可见面全部交给体积 / 壳 ⇒ 空网格。
    ColumnRectFilter filter(0, vx::kTerrainTileSize, 0, vx::kTerrainTileSize);

    TerrainWorld syncWorld(kSeed, TerrainMaterialTable::Default());
    syncWorld.SetQuadFilter(&filter);
    syncWorld.LoadTile(0, 0, 0);
    const TerrainTileMesh* syncMesh = syncWorld.FindMesh(0, 0);
    ASSERT_NE(syncMesh, nullptr);
    EXPECT_TRUE(syncMesh->meshEmpty);
    EXPECT_TRUE(syncMesh->mesh.indices.empty());
    EXPECT_EQ(syncWorld.SyncFallbackCount(), 1U) << "同步路径（未预取）会记一次回退";

    // 预取路径：worker 产出**未过滤**网格 → 暂存 → `LoadTile` 命中 ⇒ 安装时按当前过滤器过滤。
    TerrainWorld stagedWorld(kSeed, TerrainMaterialTable::Default());
    stagedWorld.SetQuadFilter(&filter);
    const vx::TerrainNoiseGenerator noise(kSeed, vx::TerrainGenerationParams::Default());
    TerrainTile                    tile = vx::GenerateTerrainTileData(noise, {}, 0, 0);
    TerrainTileMesh                mesh = vx::BuildTerrainMesh(tile, nullptr, 0);
    EXPECT_FALSE(mesh.meshEmpty) << "未过滤 ⇒ 满网格（非空）";
    stagedWorld.StageTile(std::move(tile), std::move(mesh));
    EXPECT_TRUE(stagedWorld.HasStagedTile(0, 0, 0));

    stagedWorld.LoadTile(0, 0, 0);
    EXPECT_EQ(stagedWorld.SyncFallbackCount(), 0U) << "命中预取缓存 ⇒ 不回退到同步生成";
    const TerrainTileMesh* stagedMesh = stagedWorld.FindMesh(0, 0);
    ASSERT_NE(stagedMesh, nullptr);
    ExpectMeshBitEqual(*syncMesh, *stagedMesh);
    EXPECT_TRUE(stagedMesh->meshEmpty);
}

// ③ `ApplyQuadFilterToMesh(nullptr, ...)` = 无操作；且与"整段丢弃命中格"的独立参考实现一致。
TEST(TerrainTileWorker, ApplyQuadFilterMatchesIndependentReference) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(2, 2, 0);
    const TerrainTile* tile = world.FindTile(2, 2);
    ASSERT_NE(tile, nullptr);

    TerrainTileMesh unfiltered = vx::BuildTerrainMesh(*tile, nullptr, 0);

    // nullptr ⇒ 逐位不变。
    TerrainTileMesh noFilter = unfiltered;
    vx::ApplyQuadFilterToMesh(*tile, nullptr, 0, noFilter.mesh);
    EXPECT_EQ(noFilter.mesh.indices, unfiltered.mesh.indices);

    // 只跳过左下 1/4（世界列 [128, 160] × [128, 160] 的四角全在其中的格子）。
    ColumnRectFilter filter(128, 160, 128, 160);
    TerrainTileMesh  filtered = unfiltered;
    vx::ApplyQuadFilterToMesh(*tile, &filter, 0, filtered.mesh);

    const std::vector<std::uint32_t> expected =
        ReferenceFilteredIndices(*tile, filter, 0, unfiltered.mesh.indices);
    EXPECT_EQ(filtered.mesh.indices, expected);
    EXPECT_LT(filtered.mesh.indices.size(), unfiltered.mesh.indices.size()) << "必须真的丢掉了一些格子";
    EXPECT_EQ(filtered.mesh.indices.size() % 6U, 0U) << "剩余索引仍是整格的 6 元组";
}

// ④ 释放 CPU 侧网格后：高度 / `meshEmpty` / `lodLevel` 保留，且能从**高度**重网格（relod / 笔刷前提）。
TEST(TerrainTileWorker, ReleaseCpuMeshKeepsHeightsAndRemeshRebuildsFromHeights) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(1, 1, 0);
    const TerrainTile* tile = world.FindTile(1, 1);
    ASSERT_NE(tile, nullptr);
    const TerrainTile tileCopy = *tile;
    const TerrainTileMesh* mesh = world.FindMesh(1, 1);
    ASSERT_NE(mesh, nullptr);
    EXPECT_FALSE(mesh->meshEmpty);
    EXPECT_EQ(mesh->mesh.vertices.size(), std::size_t { vx::kTerrainTileVertexCount } * vx::kTerrainTileVertexCount);

    world.ReleaseTileMeshCpu(1, 1);
    const TerrainTileMesh* released = world.FindMesh(1, 1);
    ASSERT_NE(released, nullptr);
    EXPECT_TRUE(released->mesh.vertices.empty()) << "顶点缓冲必须已释放";
    EXPECT_TRUE(released->mesh.indices.empty()) << "索引缓冲必须已释放";
    EXPECT_FALSE(released->meshEmpty) << "接管标记必须保留（不能靠 indices.empty() 判断）";
    EXPECT_EQ(released->lodLevel, 0);
    EXPECT_EQ(released->verticesPerSide, vx::kTerrainTileVertexCount);

    // 高度数据保留 ⇒ 从高度重建（relod）得到目标 LOD 的几何。
    ExpectTileBitEqual(tileCopy, *world.FindTile(1, 1));
    world.MeshTile(1, 1, 1);
    const TerrainTileMesh* relod = world.FindMesh(1, 1);
    ASSERT_NE(relod, nullptr);
    EXPECT_EQ(relod->lodLevel, 1);
    EXPECT_EQ(relod->mesh.vertices.size(), std::size_t { vx::TerrainLodVertexCount(1) });
    EXPECT_EQ(relod->mesh.indices.size(), std::size_t { vx::TerrainLodIndexCount(1) });
    EXPECT_FALSE(relod->meshEmpty);
    // 重网格不得改动高度数据。
    ExpectTileBitEqual(tileCopy, *world.FindTile(1, 1));
}

// ④b 被体积接管的空网格：释放后 `meshEmpty` 仍为 true（接管判据不受释放影响）。
TEST(TerrainTileWorker, ReleaseKeepsEmptyMarkForTakenOverTile) {
    ColumnRectFilter filter(0, vx::kTerrainTileSize, 0, vx::kTerrainTileSize);
    TerrainWorld      world(kSeed, TerrainMaterialTable::Default());
    world.SetQuadFilter(&filter);
    world.LoadTile(0, 0, 0);

    ASSERT_TRUE(world.FindMesh(0, 0)->meshEmpty);
    world.ReleaseTileMeshCpu(0, 0);
    const TerrainTileMesh* released = world.FindMesh(0, 0);
    ASSERT_NE(released, nullptr);
    EXPECT_TRUE(released->mesh.indices.empty());
    EXPECT_TRUE(released->meshEmpty) << "空网格标记在释放后必须仍表达'被接管'";
}

// ⑤ `InstallRemeshedMesh`：未常驻 / LOD 不符 ⇒ false（不静默使用陈旧数据）；常驻且 LOD 相符 ⇒ 换网格。
TEST(TerrainTileWorker, InstallRemeshedMeshValidatesResidencyAndLod) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0, 0);
    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);

    TerrainTileMesh mesh1 = vx::BuildTerrainMesh(*tile, nullptr, 1);
    EXPECT_FALSE(world.InstallRemeshedMesh(TileCoord { 5, 5 }, 1, mesh1)) << "未常驻 ⇒ 拒绝";
    EXPECT_FALSE(world.InstallRemeshedMesh(TileCoord { 0, 0 }, 2, vx::BuildTerrainMesh(*tile, nullptr, 1)))
        << "LOD 不符 ⇒ 拒绝";
    EXPECT_TRUE(world.InstallRemeshedMesh(TileCoord { 0, 0 }, 1, vx::BuildTerrainMesh(*tile, nullptr, 1)));
    EXPECT_EQ(world.FindMesh(0, 0)->lodLevel, 1);
}

// ⑤b relod 提交（快照 + 只网格化）与直接 `BuildTerrainMesh` 逐位一致（worker 只重网格路径）。
TEST(TerrainTileWorker, RemeshRequestMatchesDirectBuild) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0, 0);
    const TerrainTile* tile = world.FindTile(0, 0);
    ASSERT_NE(tile, nullptr);

    TerrainTileBuildPipeline pipeline(kSeed, vx::TerrainGenerationParams::Default(), {}, /*workerThreads=*/1);
    TerrainTileBuildRequest  request;
    request.coord      = TileCoord { 0, 0 };
    request.lodLevel   = 1;
    request.remeshOnly = true;
    request.tile       = *tile;  // 高度快照
    pipeline.Submit(std::move(request));

    TerrainTileBuildResult result;
    ASSERT_TRUE(pipeline.TakeCompleted(result));
    EXPECT_TRUE(result.remeshOnly);
    ExpectMeshBitEqual(vx::BuildTerrainMesh(*tile, nullptr, 1), result.mesh);
}
