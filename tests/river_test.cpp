// W6：**河流 + 水体**的表示与自洽性单测（见 docs/plans/v0.4.md §1.6 / [ADR 0027]）。
//
// 覆盖四条判据：
//   ① 河床沿下坡刻蚀、河岸连续 —— 路径水位沿程**单调不升**；下切场中心满值、远处为 0 且随距离**单调递减**；
//   ② 水位连续且与地形自洽（水在低处）—— 每个断面 `河床底 < 水位 ≤ 宏地表`；
//   ③ 水面与角色碰撞 —— 河道刻进地表壳 ⇒ 壳的等值面在河道中心**被下切**（碰撞随壳自动承担）；
//   ④ 网格质量 —— 水面网格无退化三角形；关闭河流时**逐位一致**（不下切）。

#include "water/river.hpp"

#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "shell/surface_shell.hpp"
#include "terrain/terrain_types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using vx::BlockCoord;
using vx::CountDegenerateTriangles;
using vx::GenerateRiverPath;
using vx::HeightToBlocks;
using vx::kVolumeBlockSize;
using vx::MeshData;
using vx::RiverCarveField;
using vx::RiverPath;
using vx::SurfaceShellParams;
using vx::SurfaceShellRegion;
using vx::SurfaceShellSampler;
using vx::TerrainGenerationParams;
using vx::TerrainNoiseGenerator;

constexpr std::uint64_t kSeed = 0x5EED2024ULL;

constexpr int kMinX = 0;
constexpr int kMinZ = 0;
constexpr int kMaxX = 256;
constexpr int kMaxZ = 256;

[[nodiscard]] TerrainGenerationParams RiverParams() {
    TerrainGenerationParams params = TerrainGenerationParams::Default();
    params.river.enabled           = true;
    return params;
}

[[nodiscard]] SurfaceShellParams MakeShellParams() {
    SurfaceShellParams params;
    params.bandHalfThicknessBlocks = 24.0F;
    params.edgeFadeBlocks          = 32.0F;
    return params;
}

[[nodiscard]] SurfaceShellRegion MakeRegion() {
    SurfaceShellRegion region;
    region.minColumnX = kMinX;
    region.maxColumnX = kMaxX;
    region.minColumnZ = kMinZ;
    region.maxColumnZ = kMaxZ;
    return region;
}

[[nodiscard]] float MacroHeightAt(const TerrainNoiseGenerator& noise, float x, float z) {
    return HeightToBlocks(noise.HeightUnits(static_cast<std::int64_t>(std::llround(x)),
                                            static_cast<std::int64_t>(std::llround(z))));
}

/// 壳在列 `(x, z)` 处的**最高等值面** y（从高处往下找第一处"空 → 实"的符号变化，并线性插值出零交点）。
[[nodiscard]] float TopShellSurfaceAt(const TerrainNoiseGenerator& noise, const TerrainGenerationParams& generation,
                                      const SurfaceShellParams& params, const SurfaceShellRegion& region,
                                      const RiverCarveField* river, int x, int z) {
    const float top = MacroHeightAt(noise, static_cast<float>(x), static_cast<float>(z)) + 24.0F;
    float       previous = 0.0F;
    bool        have     = false;
    for (int y = static_cast<int>(std::floor(top)); y > static_cast<int>(std::floor(top)) - 60; --y) {
        const BlockCoord block { x / kVolumeBlockSize, y / kVolumeBlockSize, z / kVolumeBlockSize };
        const SurfaceShellSampler sampler(noise, generation, params, region, block, river);
        const float value = sampler.Sample(x - block.x * kVolumeBlockSize, y - block.y * kVolumeBlockSize,
                                           z - block.z * kVolumeBlockSize);
        if (have && previous > 0.0F && value <= 0.0F) {
            // `previous` 在 y+1（空）、`value` 在 y（实）⇒ 线性插值出零交点。
            const float fraction = (previous == value) ? 0.5F : (previous / (previous - value));
            return (static_cast<float>(y) + 1.0F) - fraction;
        }
        previous = value;
        have     = true;
    }
    return top;
}

}  // namespace

// ① 河道沿下坡：路径非空、节点在区域内、**水位沿程单调不升**。
TEST(River, PathDescendsDownhillMonotonically) {
    const TerrainGenerationParams generation = RiverParams();
    const TerrainNoiseGenerator   noise(kSeed, generation);
    const RiverPath               path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);

    ASSERT_FALSE(path.Empty()) << "启用河流后必须生成至少 2 个节点的河道";
    float previousLevel = 1.0e30F;
    for (const vx::RiverNode& node : path.nodes) {
        EXPECT_GE(node.x, static_cast<float>(kMinX));
        EXPECT_LT(node.x, static_cast<float>(kMaxX));
        EXPECT_GE(node.z, static_cast<float>(kMinZ));
        EXPECT_LT(node.z, static_cast<float>(kMaxZ));
        EXPECT_LE(node.waterLevelBlocks, previousLevel + 1.0e-3F) << "水位必须沿程单调不升（水往低处流）";
        EXPECT_GT(node.halfWidthBlocks, 0.0F);
        previousLevel = node.waterLevelBlocks;
    }
}

// ② 水在低处：每个断面 `河床底 < 水位 ≤ 宏地表`。
TEST(River, WaterLevelSitsInsideTheChannel) {
    const TerrainGenerationParams generation = RiverParams();
    const TerrainNoiseGenerator   noise(kSeed, generation);
    const RiverPath               path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    ASSERT_FALSE(path.Empty());

    const float depth = generation.river.channelDepthBlocks;
    const float water = generation.river.waterDepthBlocks;
    for (const vx::RiverNode& node : path.nodes) {
        const float surface = MacroHeightAt(noise, node.x, node.z);
        const float bed     = surface - node.carveDepthBlocks;
        EXPECT_GT(node.waterLevelBlocks, bed - 1.0e-3F) << "水位必须在河床之上";
        EXPECT_LE(node.waterLevelBlocks, surface + 1.0e-3F) << "水位不得高于当地地表（否则就是水漫过河岸）";
        EXPECT_GE(node.carveDepthBlocks, depth - 1.0e-3F) << "下切深度不得小于配置的河道深度";
        // 无回水时水位 = 河床底 + 水深；单调约束只允许它**更低**（回水 / 洼地）。
        EXPECT_LE(node.waterLevelBlocks, surface - depth + water + 1.0e-3F);
    }
}

// ① 河岸连续（**剖面**）：在一条**人工直线河道**上，下切量随离河道的距离**单调不增**、中心满值、远处为 0。
//    用人工路径把"剖面"与"路径生成"分开验证（真实路径的下切深度沿程变化，不适合做全局距离单调性判据）。
TEST(River, CarveRampIsMonotonicAndZeroAwayFromAStraightChannel) {
    TerrainGenerationParams params = RiverParams();
    params.river.channelDepthBlocks     = 4.0F;
    params.river.channelHalfWidthBlocks = 6.0F;
    params.river.bankHalfWidthBlocks    = 4.0F;

    vx::RiverPath path;
    for (float x = 100.0F; x <= 140.0F; x += 20.0F) {
        vx::RiverNode node;
        node.x                = x;
        node.z                = 100.0F;
        node.waterLevelBlocks = 100.0F;
        node.halfWidthBlocks  = 2.0F;
        node.carveDepthBlocks = 4.0F;
        path.nodes.push_back(node);
    }

    const RiverCarveField field(path, params.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);

    EXPECT_FLOAT_EQ(field.CarveAt(120.0F, 100.0F), 4.0F);   // 中心
    EXPECT_FLOAT_EQ(field.CarveAt(120.0F, 106.0F), 4.0F);   // 下切满值区边缘（d = 6）
    EXPECT_NEAR(field.CarveAt(120.0F, 108.0F), 2.0F, 0.05F);  // 河岸中点（d = 8 ⇒ 系数 0.5）
    EXPECT_FLOAT_EQ(field.CarveAt(120.0F, 110.0F), 0.0F);   // 河岸外缘（d = 10）
    EXPECT_FLOAT_EQ(field.CarveAt(120.0F, 130.0F), 0.0F);   // 远处

    // 沿垂直方向远离河道 ⇒ 单调不增。
    float previous = 1.0e30F;
    for (float offset = 0.0F; offset <= 14.0F; offset += 0.5F) {
        const float carve = field.CarveAt(120.0F, 100.0F + offset);
        EXPECT_LE(carve, previous + 1.0e-4F) << "下切量必须随距离单调不增（offset=" << offset << "）";
        EXPECT_GE(carve, 0.0F);
        EXPECT_LE(carve, 4.0F + 1.0e-4F);
        previous = carve;
    }
}

// ① 真实河道：河道中心的库值下切（≥ 配置深度）、远离河道处为 0（河岸连续、不会整片下切）。
TEST(River, CarveFieldIsFullAtTheRealChannelAndZeroAwayFromIt) {
    const TerrainGenerationParams generation = RiverParams();
    const TerrainNoiseGenerator   noise(kSeed, generation);
    const RiverPath               path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    ASSERT_FALSE(path.Empty());

    const RiverCarveField field(path, generation.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);
    ASSERT_FALSE(field.Empty());

    const vx::RiverNode* interior = nullptr;
    for (const vx::RiverNode& node : path.nodes) {
        if (node.x > 40.0F && node.x < static_cast<float>(kMaxX) - 40.0F && node.z > 40.0F &&
            node.z < static_cast<float>(kMaxZ) - 40.0F) {
            interior = &node;
            break;
        }
    }
    ASSERT_NE(interior, nullptr) << "河道应至少有一个位于区域内部（边界淡出之外）的断面";
    EXPECT_GE(field.CarveAt(interior->x, interior->z), generation.river.channelDepthBlocks - 0.75F);

    float maxCarveDepth = generation.river.channelDepthBlocks;
    for (const vx::RiverNode& node : path.nodes) {
        maxCarveDepth = std::max(maxCarveDepth, node.carveDepthBlocks);
    }
    for (int z = 0; z < kMaxZ; z += 8) {
        for (int x = 0; x < kMaxX; x += 8) {
            const float carve = field.CarveAt(static_cast<float>(x), static_cast<float>(z));
            EXPECT_GE(carve, 0.0F);
            EXPECT_LE(carve, maxCarveDepth + 1.0e-3F);
        }
    }
    // 区域角落（远离河道）必须不刻蚀。
    EXPECT_FLOAT_EQ(field.CarveAt(2.0F, 2.0F), 0.0F);
    EXPECT_FLOAT_EQ(field.CarveAt(253.0F, 253.0F), 0.0F);
}

// ④ 水面网格：非空、无退化三角形；顶点 y 与水位一致。
TEST(River, WaterMeshIsNonDegenerateAndMatchesWaterLevels) {
    const TerrainGenerationParams generation = RiverParams();
    const TerrainNoiseGenerator   noise(kSeed, generation);
    const RiverPath               path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    ASSERT_FALSE(path.Empty());

    const MeshData mesh = vx::BuildRiverWaterMesh(path, kMinX, kMinZ);
    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 3U, 0U);
    EXPECT_EQ(CountDegenerateTriangles(mesh), 0U) << "水面网格不得出现退化三角形";

    // 顶点 y 必须等于（某个断面的）水位 ⇒ 水面连续、不悬空。
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        const float y = vertex.position[1];
        const bool  matches = std::any_of(path.nodes.begin(), path.nodes.end(), [y](const vx::RiverNode& node) {
            return std::abs(node.waterLevelBlocks - y) < 1.0e-3F;
        });
        EXPECT_TRUE(matches) << "水面顶点 y 必须等于某个断面的水位";
    }
}

// ③ + ① 一体化：河道刻进地表壳 ⇒ 壳的等值面在河道中心被**下切**（碰撞随壳自动承担）。
TEST(River, CarvingLowersTheShellSurfaceAtTheChannel) {
    TerrainGenerationParams generation = RiverParams();
    // 隔离出**河道**项：关掉悬垂与洞穴 ⇒ 壳密度 = `(y − 宏地表 + 河道下切) × 单位`，
    // 于是"河道中心的地表被下切多少"可以精确判定（量化误差只来自线性插值）。
    generation.overhang.amplitudeBlocks = 0.0F;
    generation.caves.enabled            = false;

    const SurfaceShellParams    shell  = MakeShellParams();
    const SurfaceShellRegion    region = MakeRegion();
    const TerrainNoiseGenerator noise(kSeed, generation);

    const RiverPath       path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    ASSERT_FALSE(path.Empty());
    const RiverCarveField field(path, generation.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);

    const vx::RiverNode* interior = nullptr;
    for (const vx::RiverNode& node : path.nodes) {
        if (node.x > 40.0F && node.x < static_cast<float>(kMaxX) - 40.0F && node.z > 40.0F &&
            node.z < static_cast<float>(kMaxZ) - 40.0F) {
            interior = &node;
            break;
        }
    }
    ASSERT_NE(interior, nullptr);

    const int x = static_cast<int>(std::lround(interior->x));
    const int z = static_cast<int>(std::lround(interior->z));

    const float surfaceWithout = TopShellSurfaceAt(noise, generation, shell, region, nullptr, x, z);
    const float surfaceWith    = TopShellSurfaceAt(noise, generation, shell, region, &field, x, z);

    EXPECT_LT(surfaceWith, surfaceWithout - 1.0F) << "河道中心的地表壳等值面必须被下切（河床随壳提供碰撞）";
    EXPECT_NEAR(surfaceWithout - surfaceWith, interior->carveDepthBlocks, 0.75F)
        << "下切幅度应等于该断面的 carve_depth_blocks";
}

// 确定性：同一输入两次生成**逐位一致**（红线 7）。
TEST(River, GenerationIsDeterministic) {
    const TerrainGenerationParams generation = RiverParams();
    const TerrainNoiseGenerator   noise(kSeed, generation);

    const RiverPath first  = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    const RiverPath second = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    ASSERT_EQ(first.nodes.size(), second.nodes.size());
    for (std::size_t i = 0; i < first.nodes.size(); ++i) {
        EXPECT_EQ(first.nodes[i].x, second.nodes[i].x);
        EXPECT_EQ(first.nodes[i].z, second.nodes[i].z);
        EXPECT_EQ(first.nodes[i].waterLevelBlocks, second.nodes[i].waterLevelBlocks);
    }

    const RiverCarveField fieldA(first, generation.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);
    const RiverCarveField fieldB(second, generation.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);
    for (int z = 0; z < kMaxZ; z += 7) {
        for (int x = 0; x < kMaxX; x += 7) {
            EXPECT_FLOAT_EQ(fieldA.CarveAt(static_cast<float>(x), static_cast<float>(z)),
                            fieldB.CarveAt(static_cast<float>(x), static_cast<float>(z)));
        }
    }
}

// 关闭河流 ⇒ 空路径、下切恒 0、无水面网格（与引入本层之前逐位一致）。
TEST(River, DisabledRiverProducesNoPathCarveOrMesh) {
    const TerrainGenerationParams generation = TerrainGenerationParams::Default();  // river.enabled = false
    const TerrainNoiseGenerator   noise(kSeed, generation);

    const RiverPath path = GenerateRiverPath(noise, generation.river, kMinX, kMinZ, kMaxX, kMaxZ);
    EXPECT_TRUE(path.Empty()) << "关闭河流时不得生成河道";

    const RiverCarveField field(path, generation.river, kMinX, kMinZ, kMaxX - kMinX, kMaxZ - kMinZ);
    EXPECT_TRUE(field.Empty());
    for (int z = 0; z < kMaxZ; z += 11) {
        for (int x = 0; x < kMaxX; x += 11) {
            EXPECT_FLOAT_EQ(field.CarveAt(static_cast<float>(x), static_cast<float>(z)), 0.0F);
        }
    }
    EXPECT_TRUE(vx::BuildRiverWaterMesh(path, kMinX, kMinZ).indices.empty());
}

// 随仓库发布的 `assets/config/terrain.toml` 必须可加载且**启用**河流。
TEST(River, ShippedConfigLoadsAndEnablesRiver) {
    const std::filesystem::path path = std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/terrain.toml";
    const TerrainGenerationParams params = TerrainGenerationParams::LoadFromFile(path);

    EXPECT_TRUE(params.river.enabled) << "发布配置必须启用河流";
    EXPECT_LE(params.river.waterDepthBlocks, params.river.channelDepthBlocks) << "水必须在河床之内";
}

// 非法配置（水深 > 河床下切深度 ⇒ 水漫过河岸）⇒ 加载即抛（ADR 0005 口径，不静默回退）。
TEST(River, RejectsWaterDeeperThanTheChannel) {
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "voxel_river_bad.toml";
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
               "enabled = false\n"
               "frequency = 0.02\n"
               "tunnel_radius = 0.32\n"
               "carve_strength_blocks = 22.0\n"
               "depth_fade_blocks = 8.0\n"
               "seed_channel = 7\n"
               "[river]\n"
               "enabled = true\n"
               "step_blocks = 8.0\n"
               "max_nodes = 512\n"
               "channel_depth_blocks = 4.0\n"
               "channel_half_width_blocks = 6.0\n"
               "bank_half_width_blocks = 4.0\n"
               "water_depth_blocks = 9.0\n"  // > channel_depth_blocks ⇒ 非法
               "jitter_radians = 0.35\n"
               "jitter_frequency = 0.01\n"
               "seed_channel = 8\n";
    }
    EXPECT_THROW((void)TerrainGenerationParams::LoadFromFile(bad), std::runtime_error);
    std::error_code error;
    std::filesystem::remove(bad, error);
}
