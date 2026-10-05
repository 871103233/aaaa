// W4 / [ADR 0023]：**地表壳**（世界表示 v2 层②）的表示与网格化单测。
//
// 验收判据（见 docs/plans/v0.4.md §1.3，判据②已由所有者裁定修正）：
//   ① 近场可见悬垂 —— 存在"同一列 ≥ 2 处符号变化"（= 不只一片表面 ⇒ 悬垂 / 洞穴）；
//   ② 无裂缝 + 内部无孔 —— 相邻块**共享采样点密度逐位相同**（无裂缝的根因）+ 退化三角形 = 0
//      + 相邻两块合并成区域后**三角形数守恒**（无重复、无缺失）；
//   ③ 与宏高度场无缝隙 —— 悬垂幅度 = 0 时等值面 y 与宏地表高度一致。

#include "shell/surface_shell.hpp"

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

using vx::BlockCoord;
using vx::BuildRegionMesh;
using vx::BuildShellBlockMesh;
using vx::CountDegenerateTriangles;
using vx::HeightToBlocks;
using vx::kVolumeBlockSize;
using vx::MeshData;
using vx::SurfaceShellParams;
using vx::SurfaceShellRegion;
using vx::SurfaceShellSampler;
using vx::TerrainGenerationParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED2024ULL;

/// 测试区域：世界列 `[0, 256)²`（正坐标，避免负除法），边界淡出 32 格 ⇒ 内部 [32, 224)。
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

/// 测试用生成参数：地貌关闭（保持地形简单）、悬垂启用（默认值）。
[[nodiscard]] TerrainGenerationParams MakeGeneration() { return TerrainGenerationParams::Default(); }

/// 任意世界采样点的壳密度（按该点所属块构造采样器；世界坐标为正）。
[[nodiscard]] float ShellDensityAt(const TerrainNoiseGenerator& noise, const TerrainGenerationParams& generation,
                                   const SurfaceShellParams& params, const SurfaceShellRegion& region, int x, int y,
                                   int z) {
    const BlockCoord block { x / kVolumeBlockSize, y / kVolumeBlockSize, z / kVolumeBlockSize };
    const SurfaceShellSampler sampler(noise, generation, params, region, block);
    return sampler.Sample(x - block.x * kVolumeBlockSize, y - block.y * kVolumeBlockSize,
                          z - block.z * kVolumeBlockSize);
}

}  // namespace

// ① 悬垂：区域内应存在某列沿竖直方向出现 **≥ 2 次符号变化**（= 不只一片表面）⇒ 表示层确有悬垂 / 洞穴。
TEST(SurfaceShell, OverhangFoldsTheSurface) {
    const TerrainGenerationParams generation = MakeGeneration();
    const SurfaceShellParams      params     = MakeParams();
    const SurfaceShellRegion      region     = MakeRegion();
    const TerrainNoiseGenerator   noise(kSeed, generation);

    int maxCrossings = 0;
    for (int z = 64; z < 192; z += 7) {
        for (int x = 64; x < 192; x += 7) {
            const int center = static_cast<int>(HeightToBlocks(noise.HeightUnits(x, z)));
            int       crossings = 0;
            float     previous  = ShellDensityAt(noise, generation, params, region, x, center - 30, z);
            for (int y = center - 29; y <= center + 30; ++y) {
                const float current = ShellDensityAt(noise, generation, params, region, x, y, z);
                if ((previous < 0.0F) != (current < 0.0F)) {
                    ++crossings;
                }
                previous = current;
            }
            maxCrossings = std::max(maxCrossings, crossings);
        }
    }
    EXPECT_GE(maxCrossings, 2) << "区域内应存在「同一列 ≥2 片表面」的悬垂 / 洞穴（单张地表只会有 1 次符号变化）";
}

// ③ 无悬垂时，等值面与**宏高度场**一致（容差 ≤ 0.2 格）：证明壳在无悬垂处就是地表本身。
TEST(SurfaceShell, IsosurfaceMatchesMacroHeightWhenOverhangDisabled) {
    TerrainGenerationParams generation = MakeGeneration();
    generation.overhang.amplitudeBlocks = 0.0F;  // 关掉悬垂 ⇒ 密度严格 = (y − H)

    const SurfaceShellParams    params = MakeParams();
    const SurfaceShellRegion    region = MakeRegion();
    const TerrainNoiseGenerator noise(kSeed, generation);

    for (int z = 64; z < 160; z += 11) {
        for (int x = 64; x < 160; x += 11) {
            const float height = HeightToBlocks(noise.HeightUnits(x, z));
            // 在高度附近找一段**跨零**区间（允许端点恰为 0：高度是 1/16 格的整数倍，常见恰好落在格点上）。
            const int start = std::max(0, static_cast<int>(std::floor(height)) - 2);
            bool      found = false;
            int       lower = start;
            float     d0    = 0.0F;
            float     d1    = 0.0F;
            for (int y = start; y < start + 5; ++y) {
                const float a = ShellDensityAt(noise, generation, params, region, x, y, z);
                const float b = ShellDensityAt(noise, generation, params, region, x, y + 1, z);
                if (a <= 0.0F && b >= 0.0F) {
                    found = true;
                    lower = y;
                    d0    = a;
                    d1    = b;
                    break;
                }
            }
            ASSERT_TRUE(found) << "高度附近必须存在跨零区间（列 " << x << ", " << z << "）";
            const float isosurface = (d1 > d0) ? (static_cast<float>(lower) + (0.0F - d0) / (d1 - d0))
                                               : static_cast<float>(lower);
            EXPECT_NEAR(isosurface, height, 0.2F) << "无悬垂处等值面必须与宏地表高度一致（列 " << x << ", " << z << "）";
        }
    }
}

// ② 无裂缝的根因：同一个世界采样点，用**相邻的两个块坐标**各算一次 ⇒ 密度**逐位相同**。
TEST(SurfaceShell, SamplingIsConsistentAcrossAdjacentBlocks) {
    const TerrainGenerationParams generation = MakeGeneration();
    const SurfaceShellParams      params     = MakeParams();
    const SurfaceShellRegion      region     = MakeRegion();
    const TerrainNoiseGenerator   noise(kSeed, generation);

    // 取一个块，并在它的三个 max 面上取采样点（那些点同时属于相邻块的负侧围裙）。
    const BlockCoord base { 4, 4, 4 };  // 世界 128..160
    const SurfaceShellSampler lowerBlock(noise, generation, params, region, base);

    const SurfaceShellSampler nextX(noise, generation, params, region, BlockCoord { base.x + 1, base.y, base.z });
    const SurfaceShellSampler nextY(noise, generation, params, region, BlockCoord { base.x, base.y + 1, base.z });
    const SurfaceShellSampler nextZ(noise, generation, params, region, BlockCoord { base.x, base.y, base.z + 1 });

    for (int j = 0; j <= kVolumeBlockSize; j += 8) {
        for (int k = 0; k <= kVolumeBlockSize; k += 8) {
            // 共享 X 面（本块 i = 32 ↔ 邻块 i = 0）
            EXPECT_FLOAT_EQ(lowerBlock.Sample(kVolumeBlockSize, j, k), nextX.Sample(0, j, k))
                << "共享 X 面的采样必须逐位一致";
            // 共享 Z 面
            EXPECT_FLOAT_EQ(lowerBlock.Sample(j, k, kVolumeBlockSize), nextZ.Sample(j, k, 0))
                << "共享 Z 面的采样必须逐位一致";
            // 共享 Y 面
            EXPECT_FLOAT_EQ(lowerBlock.Sample(j, kVolumeBlockSize, k), nextY.Sample(j, 0, k))
                << "共享 Y 面的采样必须逐位一致";
        }
    }
}

// ② 内部无孔：相邻两块**各自建模**的三角形数之和 == 把两块并成一个区域建模的三角形数（无重复、无缺失）；
//    且区域网格无退化三角形。（`CountBoundaryEdges > 0` 是本判据修正的由来：地表是**开曲面**。）
TEST(SurfaceShell, AdjacentBlockMeshesTileTheRegionWithoutDuplicationOrLoss) {
    TerrainGenerationParams generation = MakeGeneration();
    generation.overhang.amplitudeBlocks = 7.0F;

    const SurfaceShellParams    params = MakeParams();
    const SurfaceShellRegion    region = MakeRegion();
    const TerrainNoiseGenerator noise(kSeed, generation);

    const BlockCoord blockA { 4, 4, 4 };  // 世界 128..160（y 覆盖 128..160，地表在其中）

    const SurfaceShellSampler samplerA(noise, generation, params, region, blockA);
    const SurfaceShellSampler samplerB(noise, generation, params, region,
                                        BlockCoord { blockA.x + 1, blockA.y, blockA.z });

    const MeshData meshA = vx::BuildVolumeMesh(samplerA);
    const MeshData meshB = vx::BuildVolumeMesh(samplerB);
    ASSERT_FALSE(meshA.indices.empty());
    ASSERT_FALSE(meshB.indices.empty());
    EXPECT_EQ(CountDegenerateTriangles(meshA), 0U) << "单块 A 不得含退化三角形";
    EXPECT_EQ(CountDegenerateTriangles(meshB), 0U) << "单块 B 不得含退化三角形（A="
                                                  << CountDegenerateTriangles(meshA) << "）";

    // 同一区域（两块并集）= 2×32 列 × 32 × 32 体素。
    const MeshData regionMesh =
        BuildRegionMesh(samplerA, 2 * kVolumeBlockSize, kVolumeBlockSize, kVolumeBlockSize);

    EXPECT_EQ(regionMesh.indices.size(), meshA.indices.size() + meshB.indices.size())
        << "合并区域的三角形数必须等于两块之和（接缝处既不重复也不缺失）";
    EXPECT_EQ(CountDegenerateTriangles(regionMesh), 0U) << "壳网格不得含退化三角形";
}

// 确定性：同一输入两次网格化逐值一致（红线 7）。
TEST(SurfaceShell, BlockMeshIsDeterministic) {
    TerrainGenerationParams generation = MakeGeneration();
    generation.overhang.amplitudeBlocks = 7.0F;

    const SurfaceShellParams    params = MakeParams();
    const SurfaceShellRegion    region = MakeRegion();
    const TerrainNoiseGenerator noise(kSeed, generation);
    const BlockCoord            block { 4, 4, 4 };

    const MeshData first  = BuildShellBlockMesh(noise, generation, params, region, block);
    const MeshData second = BuildShellBlockMesh(noise, generation, params, region, block);

    ASSERT_EQ(first.vertices.size(), second.vertices.size());
    EXPECT_EQ(first.indices, second.indices);
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        EXPECT_EQ(first.vertices[i].position[0], second.vertices[i].position[0]);
        EXPECT_EQ(first.vertices[i].position[1], second.vertices[i].position[1]);
        EXPECT_EQ(first.vertices[i].position[2], second.vertices[i].position[2]);
    }
}
