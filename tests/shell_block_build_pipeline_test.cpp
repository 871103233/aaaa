// W7-S3b① / [ADR 0022] 形态：**地表壳块构建下沉 worker** 的核心测试。
//
// 覆盖：① 同步回退路径（`workerThreads = 1`）与直接 `BuildShellBlockMesh` **逐位一致**；
//       ② 真实 worker 线程的结果与同步路径**逐位一致**（真并行下的确定性，红线 7）；
//       ③ 请求里的**区域快照**被真正使用（不同区域 ⇒ 边界淡出不同 ⇒ 结果不同）。

#include "streaming/shell_block_build_pipeline.hpp"

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "physics/physics_world.hpp"  // worker 侧 `PrepareMeshShape` 需要 Jolt 已初始化（PhysicsWorld 持有）
#include "shell/surface_shell.hpp"
#include "terrain/terrain_types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vx::BlockCoord;
using vx::BuildShellBlockMesh;
using vx::MeshData;
using vx::ShellBlockBuildPipeline;
using vx::ShellBlockBuildRequest;
using vx::ShellBlockBuildResult;
using vx::SurfaceShellParams;
using vx::SurfaceShellRegion;
using vx::TerrainGenerationParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED2024ULL;

[[nodiscard]] SurfaceShellRegion MakeRegion() {
    SurfaceShellRegion region;
    region.minColumnX = 0;
    region.maxColumnX = 256;
    region.minColumnZ = 0;
    region.maxColumnZ = 256;
    return region;
}

[[nodiscard]] SurfaceShellParams MakeParams() {
    SurfaceShellParams params;
    params.bandHalfThicknessBlocks = 24.0F;
    params.edgeFadeBlocks           = 32.0F;
    return params;
}

/// 逐位比较两份网格（顶点位置 / 法线 / 材质 + 索引）。
void ExpectMeshBitEqual(const MeshData& a, const MeshData& b) {
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    ASSERT_EQ(a.indices.size(), b.indices.size());
    for (std::size_t k = 0; k < a.vertices.size(); ++k) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(a.vertices[k].position[c], b.vertices[k].position[c]) << "顶点 " << k;
            EXPECT_FLOAT_EQ(a.vertices[k].normal[c], b.vertices[k].normal[c]) << "顶点 " << k;
        }
        EXPECT_FLOAT_EQ(a.vertices[k].material, b.vertices[k].material) << "顶点 " << k;
    }
    EXPECT_EQ(a.indices, b.indices);
}

[[nodiscard]] std::vector<BlockCoord> MakeBlocks() {
    return { BlockCoord { 2, 0, 2 }, BlockCoord { 3, 1, 2 }, BlockCoord { 2, 0, 3 }, BlockCoord { 4, 0, 4 } };
}

}  // namespace

// ① 同步回退（`workerThreads = 1` ⇒ 线程池不可用）与直接构建逐位一致。
TEST(ShellBlockWorker, SyncFallbackMatchesDirectBuild) {
    const vx::PhysicsWorld      physics;  // 初始化 Jolt（`PrepareMeshShape` 在 worker 上需要）
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    ShellBlockBuildPipeline pipeline(kSeed, TerrainGenerationParams::Default(), params, nullptr, /*workerThreads=*/1);
    ASSERT_FALSE(pipeline.HasWorkers());

    const SurfaceShellRegion region = MakeRegion();
    std::size_t              got    = 0;
    for (const BlockCoord& block : MakeBlocks()) {
        ShellBlockBuildRequest request;
        request.block  = block;
        request.region = region;
        pipeline.Submit(std::move(request));
    }

    ShellBlockBuildResult result;
    while (pipeline.TakeCompleted(result)) {
        const MeshData expected =
            BuildShellBlockMesh(noise, TerrainGenerationParams::Default(), params, result.region, result.block, nullptr);
        ExpectMeshBitEqual(expected, result.mesh);
        ++got;
    }
    EXPECT_EQ(got, MakeBlocks().size());
}

// ② 真实 worker 线程与同步路径逐位一致（确定性，红线 7）。
TEST(ShellBlockWorker, RealWorkersProduceBitIdenticalResults) {
    const vx::PhysicsWorld  physics;  // 初始化 Jolt（`PrepareMeshShape` 在 worker 上需要）
    ShellBlockBuildPipeline pipeline(kSeed, TerrainGenerationParams::Default(), MakeParams());
    if (!pipeline.HasWorkers()) {
        GTEST_SKIP() << "线程池不可用（回退路径已由上一个用例覆盖）";
    }

    const SurfaceShellRegion region = MakeRegion();
    for (const BlockCoord& block : MakeBlocks()) {
        ShellBlockBuildRequest request;
        request.block  = block;
        request.region = region;
        pipeline.Submit(std::move(request));
    }
    while (pipeline.PendingCount() > 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    std::size_t                 got = 0;
    ShellBlockBuildResult       result;
    while (pipeline.TakeCompleted(result)) {
        const MeshData expected = BuildShellBlockMesh(noise, TerrainGenerationParams::Default(), MakeParams(),
                                                      result.region, result.block, nullptr);
        ExpectMeshBitEqual(expected, result.mesh);
        ++got;
    }
    EXPECT_EQ(got, MakeBlocks().size());
}

// ③ 区域快照确实参与密度：同一个**含地表**的块，贴着区域边界（淡出 0）与深居区域内部（淡出 1）⇒ 网格不同。
TEST(ShellBlockWorker, RegionSnapshotAffectsFadeAndGeometry) {
    const TerrainNoiseGenerator noise(kSeed, TerrainGenerationParams::Default());
    const SurfaceShellParams    params = MakeParams();

    // 取列块 (0, 0)（列 [0, 32)²）中**含地表**的 Y 块，否则深居地下的块两侧都恒为实心、比较无意义。
    const float macro = vx::HeightToBlocks(noise.HeightUnits(16, 16));
    const BlockCoord block { 0, static_cast<int>(std::floor(macro / static_cast<float>(vx::kVolumeBlockSize))), 0 };

    SurfaceShellRegion nearBoundary;  // 该块紧贴左 / 下边界 ⇒ 淡出为 0
    nearBoundary.minColumnX = 0;
    nearBoundary.maxColumnX = 256;
    nearBoundary.minColumnZ = 0;
    nearBoundary.maxColumnZ = 256;

    SurfaceShellRegion deepInside;  // 该块距边界 256 列 ⇒ 淡出为 1（全幅悬垂）
    deepInside.minColumnX = -256;
    deepInside.maxColumnX = 512;
    deepInside.minColumnZ = -256;
    deepInside.maxColumnZ = 512;

    const MeshData a =
        BuildShellBlockMesh(noise, TerrainGenerationParams::Default(), params, nearBoundary, block, nullptr);
    const MeshData b =
        BuildShellBlockMesh(noise, TerrainGenerationParams::Default(), params, deepInside, block, nullptr);

    EXPECT_FALSE(a.vertices.empty()) << "该块含地表 ⇒ 应有等值面";
    EXPECT_FALSE(b.vertices.empty());
    const bool same = (a.vertices.size() == b.vertices.size()) && (a.indices == b.indices);
    EXPECT_FALSE(same) << "区域边界淡出不同 ⇒ 壳几何应不同";
}
