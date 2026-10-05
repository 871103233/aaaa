// W5：**洞穴网络（隧道）**的表示与连通性单测（见 docs/plans/v0.4.md §1.5）。
//
// 验收判据：
//   ① 洞穴存在 —— 地表以下 ≥ 3 格深处存在空气（隧道内部），且显著多于"关闭洞穴"的对照组；
//   ② 与地表连通 —— 从地表之上的空气出发做**体素洪泛**，能到达地下 ≥ 3 格（= 有可进入的洞口）；
//   ③ 确定性 —— 同一种子两次网格化逐位一致；`caves.enabled == false` ⇒ 雕刻量恒 0（与 W4 逐位一致）；
//   ④ 网格质量 —— 无退化三角形 + 相邻块共享面采样逐位一致（沿用 W4 的"无裂缝 + 内部无孔"口径）。

#include "shell/surface_shell.hpp"

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

using vx::BlockCoord;
using vx::BuildShellBlockMesh;
using vx::BuildVolumeMesh;
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
    params.edgeFadeBlocks          = 32.0F;
    return params;
}

/// 启用洞穴网络的生成参数（其余保持默认）。
[[nodiscard]] TerrainGenerationParams CaveParams() {
    TerrainGenerationParams params = TerrainGenerationParams::Default();
    params.caves.enabled           = true;
    return params;
}

/// 任意世界采样点的壳密度（按该点所属块构造采样器；世界坐标为正）。
[[nodiscard]] float ShellDensityAt(const TerrainNoiseGenerator& noise, const TerrainGenerationParams& generation,
                                   const SurfaceShellParams& params, const SurfaceShellRegion& region, int x, int y,
                                   int z) {
    const BlockCoord block { x / kVolumeBlockSize, y / kVolumeBlockSize, z / kVolumeBlockSize };
    const SurfaceShellSampler sampler(noise, generation, params, region, block);
    return sampler.Sample(x - block.x * kVolumeBlockSize, y - block.y * kVolumeBlockSize,
                          z - block.z * kVolumeBlockSize);
}

/// 统计"地表以下 ≥ 3 格处仍为空气"的采样点数（正整数 x、z 网格）。
[[nodiscard]] std::size_t CountSubsurfaceAir(const TerrainGenerationParams& generation,
                                             const SurfaceShellParams& params, const SurfaceShellRegion& region,
                                             std::uint64_t seed) {
    const TerrainNoiseGenerator noise(seed, generation);
    std::size_t                 count = 0;
    for (int z = 48; z < 208; z += 3) {
        for (int x = 48; x < 208; x += 3) {
            const float height = HeightToBlocks(noise.HeightUnits(x, z));
            for (int depth = 3; depth <= 20; depth += 1) {
                const int   y = static_cast<int>(std::floor(height)) - depth;
                const float d = ShellDensityAt(noise, generation, params, region, x, y, z);
                if (d > 0.0F) {
                    ++count;
                }
            }
        }
    }
    return count;
}

}  // namespace

// ① 洞穴存在：启用后"地下空气"显著多于关闭时（关闭时只有悬垂折叠造成的少量空腔）。
TEST(Caves, TunnelsCarveAirBelowTheSurface) {
    const SurfaceShellParams params = MakeParams();
    const SurfaceShellRegion region = MakeRegion();

    const std::size_t withCaves    = CountSubsurfaceAir(CaveParams(), params, region, kSeed);
    const std::size_t withoutCaves = CountSubsurfaceAir(TerrainGenerationParams::Default(), params, region, kSeed);

    EXPECT_GT(withCaves, withoutCaves) << "启用洞穴后，地下空气采样点必须多于关闭时（无隧道则二者相近）";
    EXPECT_GT(withCaves, 200U) << "洞穴网络应在地表下形成可观测的空腔";
}

// ② 与地表连通：从地表之上的空气做体素洪泛，必须能到达地下 ≥ 3 格（= 存在可进入的洞口）。
TEST(Caves, UndergroundAirConnectsToTheSurface) {
    const TerrainGenerationParams generation = CaveParams();
    const SurfaceShellParams      params     = MakeParams();
    const SurfaceShellRegion      region     = MakeRegion();
    const TerrainNoiseGenerator   noise(kSeed, generation);

    constexpr int kMinX = 80;
    constexpr int kMaxX = 176;
    constexpr int kMinZ = 80;
    constexpr int kMaxZ = 176;

    float minHeight = 1.0e9F;
    float maxHeight = -1.0e9F;
    for (int z = kMinZ; z < kMaxZ; ++z) {
        for (int x = kMinX; x < kMaxX; ++x) {
            const float height = HeightToBlocks(noise.HeightUnits(x, z));
            minHeight          = std::min(minHeight, height);
            maxHeight          = std::max(maxHeight, height);
        }
    }

    const int yMin  = static_cast<int>(std::floor(minHeight)) - 26;
    const int yMax  = static_cast<int>(std::ceil(maxHeight)) + 12;  // 悬垂幅度 7 ⇒ 顶层仍必为空气
    const int sizeX = kMaxX - kMinX;
    const int sizeZ = kMaxZ - kMinZ;
    const int sizeY = yMax - yMin + 1;

    const auto flat = [&](int x, int y, int z) {
        return static_cast<std::size_t>(x - kMinX) +
               static_cast<std::size_t>(sizeX) *
                   (static_cast<std::size_t>(z - kMinZ) + static_cast<std::size_t>(sizeZ) *
                                                              static_cast<std::size_t>(y - yMin));
    };

    const std::size_t cellCount = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY) *
                                  static_cast<std::size_t>(sizeZ);
    std::vector<std::uint8_t> air(cellCount, 0);
    for (int z = kMinZ; z < kMaxZ; ++z) {
        for (int x = kMinX; x < kMaxX; ++x) {
            for (int y = yMin; y <= yMax; ++y) {
                if (ShellDensityAt(noise, generation, params, region, x, y, z) > 0.0F) {
                    air[flat(x, y, z)] = 1;
                }
            }
        }
    }

    std::vector<std::uint8_t> visited(cellCount, 0);
    std::vector<std::size_t>  stack;
    for (int z = kMinZ; z < kMaxZ; ++z) {
        for (int x = kMinX; x < kMaxX; ++x) {
            const std::size_t index = flat(x, yMax, z);
            if (air[index] != 0 && visited[index] == 0) {
                visited[index] = 1;
                stack.push_back(index);
            }
        }
    }

    const auto tryPush = [&](int x, int y, int z, std::vector<std::size_t>& out) {
        if (x < kMinX || x >= kMaxX || z < kMinZ || z >= kMaxZ || y < yMin || y > yMax) {
            return;
        }
        const std::size_t index = flat(x, y, z);
        if (air[index] != 0 && visited[index] == 0) {
            visited[index] = 1;
            out.push_back(index);
        }
    };

    float deepestDepth = -1.0e9F;
    while (!stack.empty()) {
        const std::size_t index = stack.back();
        stack.pop_back();

        int       remaining = static_cast<int>(index);
        const int y         = yMin + remaining / (sizeX * sizeZ);
        remaining %= (sizeX * sizeZ);
        const int z = kMinZ + remaining / sizeX;
        const int x = kMinX + remaining % sizeX;

        const float depth = HeightToBlocks(noise.HeightUnits(x, z)) - static_cast<float>(y);
        deepestDepth      = std::max(deepestDepth, depth);

        tryPush(x - 1, y, z, stack);
        tryPush(x + 1, y, z, stack);
        tryPush(x, y - 1, z, stack);
        tryPush(x, y + 1, z, stack);
        tryPush(x, y, z - 1, stack);
        tryPush(x, y, z + 1, stack);
    }

    EXPECT_GT(deepestDepth, 3.0F)
        << "地表空气必须能经洞口到达地下 ≥ 3 格（否则洞穴与地表不连通 / 洞口不可进入）";
}

// ③ 确定性 + 关闭时恒等：`caves.enabled == false` ⇒ `CaveCarveAt` 恒 0（与 W4 逐位一致）。
TEST(Caves, DisabledCarveIsZeroAndEnabledMeshIsDeterministic) {
    const TerrainNoiseGenerator disabledNoise(kSeed, TerrainGenerationParams::Default());
    for (int y = 60; y < 140; y += 13) {
        for (int z = 40; z < 200; z += 17) {
            for (int x = 40; x < 200; x += 17) {
                EXPECT_FLOAT_EQ(disabledNoise.CaveCarveAt(static_cast<float>(x), static_cast<float>(y),
                                                          static_cast<float>(z)),
                                0.0F)
                    << "关闭洞穴时雕刻量必须恒为 0（与 W4 逐位一致）";
            }
        }
    }

    const TerrainGenerationParams generation = CaveParams();
    const SurfaceShellParams      params     = MakeParams();
    const SurfaceShellRegion      region     = MakeRegion();
    const TerrainNoiseGenerator   noise(kSeed, generation);
    const BlockCoord              block { 4, 4, 4 };

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

// ④ 网格质量：启用洞穴后仍无退化三角形；相邻块共享面采样逐位一致（无裂缝，沿用 W4 口径）。
TEST(Caves, CaveMeshesStaySeamlessAndFreeOfDegenerateTriangles) {
    const TerrainGenerationParams generation = CaveParams();
    const SurfaceShellParams      params     = MakeParams();
    const SurfaceShellRegion      region     = MakeRegion();
    const TerrainNoiseGenerator   noise(kSeed, generation);

    const BlockCoord base { 4, 4, 4 };  // 世界 128..160
    const SurfaceShellSampler lowerBlock(noise, generation, params, region, base);
    const SurfaceShellSampler nextX(noise, generation, params, region, BlockCoord { base.x + 1, base.y, base.z });

    for (int j = 0; j <= kVolumeBlockSize; j += 8) {
        for (int k = 0; k <= kVolumeBlockSize; k += 8) {
            EXPECT_FLOAT_EQ(lowerBlock.Sample(kVolumeBlockSize, j, k), nextX.Sample(0, j, k))
                << "共享 X 面的采样必须逐位一致（洞穴叠加后仍不得产生裂缝）";
        }
    }

    const MeshData meshA = BuildVolumeMesh(lowerBlock);
    const MeshData meshB = BuildVolumeMesh(nextX);
    EXPECT_EQ(CountDegenerateTriangles(meshA), 0U) << "含洞穴的壳网格不得出现退化三角形";
    EXPECT_EQ(CountDegenerateTriangles(meshB), 0U) << "含洞穴的壳网格不得出现退化三角形";

    const MeshData regionMesh = vx::BuildRegionMesh(lowerBlock, 2 * kVolumeBlockSize, kVolumeBlockSize, kVolumeBlockSize);
    EXPECT_EQ(regionMesh.indices.size(), meshA.indices.size() + meshB.indices.size())
        << "合并区域的三角形数必须等于两块之和（接缝处既不重复也不缺失）";
}

// 随仓库发布的 `assets/config/terrain.toml` 必须可加载且**启用**洞穴网络。
TEST(Caves, ShippedConfigLoadsAndEnablesCaves) {
    const std::filesystem::path path = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/terrain.toml";
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(path);

    EXPECT_TRUE(params.caves.enabled) << "发布配置必须启用洞穴网络";
    EXPECT_GT(params.caves.carveStrengthBlocks, 0.0F);
    EXPECT_LE(params.caves.tunnelRadius, 1.0F);
}

// 非法隧道半径（> 1，噪声值域口径）⇒ 加载即抛（ADR 0005 口径，不静默回退）。
TEST(Caves, RejectsIllegalTunnelRadius) {
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "voxel_caves_bad.toml";
    {
        std::ofstream out(bad, std::ios::trunc);
        out << "schema_version = 1\n"
               "base_frequency = 0.0035\n"
               "detail_frequency = 0.015\n"
               "rough_frequency = 0.06\n"
               "base_amplitude = 96.0\n"
               "detail_amplitude = 22.0\n"
               "rough_amplitude = 4.0\n"
               "height_offset_blocks = 128.0\n"
               "variation_frequency = 0.05\n"
               "[landform]\n"
               "enabled = false\n"
               "frequency = 0.0012\n"
               "hills_start = 0.42\n"
               "mountains_start = 0.66\n"
               "blend = 0.08\n"
               "plains_amplitude_scale = 0.30\n"
               "hills_amplitude_scale = 0.85\n"
               "mountains_amplitude_scale = 1.45\n"
               "plains_offset_blocks = -8.0\n"
               "hills_offset_blocks = 0.0\n"
               "mountains_offset_blocks = 24.0\n"
               "seed_channel = 5\n"
               "[overhang]\n"
               "frequency = 0.05\n"
               "amplitude_blocks = 7.0\n"
               "seed_channel = 6\n"
               "[caves]\n"
               "enabled = true\n"
               "frequency = 0.02\n"
               "tunnel_radius = 1.5\n"
               "carve_strength_blocks = 22.0\n"
               "depth_fade_blocks = 8.0\n"
               "seed_channel = 7\n";
    }
    EXPECT_THROW((void)TerrainGenerationParams::LoadFromFile(bad), std::runtime_error);
    std::error_code error;
    std::filesystem::remove(bad, error);
}
