// V4 / [ADR 0026]：**预制地图作为地表数据源**（`PremadeTerrainTileSource`）的单测。
//
// 覆盖三条可判定判据（见 docs/plans/v0.5.md §1.12）：
//   ① **同源**：预制源给出的 tile 高度与运行时程序化生成**逐位相同**（含共享边界层与 `maxSurfaceBlocks` 缓存）；
//   ② **不静默**：预制文件缺该 tile ⇒ 返回 false 且缺块计数递增（调用方据此回退程序化）；
//   ③ **失败即抛**：文件缺失 ⇒ `Open` 抛（不返回空读取器）。

#include "premade/premade_terrain_source.hpp"

#include "generation/map_preset.hpp"
#include "generation/terrain_noise.hpp"
#include "generation/terrain_params.hpp"
#include "premade/premade_bake.hpp"
#include "premade/premade_map.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"  // GenerateTerrainTileData / RefreshTileMaxSurfaceBlocks

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

/// 每个用例独立临时文件，析构时清理。
struct ScopedTempFile {
    std::filesystem::path path;

    explicit ScopedTempFile(const char* name)
        : path(std::filesystem::temp_directory_path() / (std::string("voxel_premade_src_") + name + ".vxmap")) {}

    ~ScopedTempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    ScopedTempFile(const ScopedTempFile&)            = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
};

/// 一张最小可烘焙预设（3×3 tile，带一条编辑 ⇒ 同时覆盖"噪声 + 编辑"两条生成路径）。
[[nodiscard]] vx::MapPreset MakePreset() {
    vx::MapPreset preset;
    preset.name        = "v4_premade_source_test";
    preset.seed        = 123456789ULL;
    preset.tileRadiusX = 1;
    preset.tileRadiusZ = 1;
    preset.spawnX      = 0.0;
    preset.spawnZ      = 0.0;
    preset.edits.push_back(
        vx::MapEdit { "flat", vx::MapEditMode::Flatten, -16, 16, -16, 16, 120 * 16 });
    return preset;
}

}  // namespace

TEST(PremadeTerrainSource, MatchesProceduralGenerationBitForBit) {
    const vx::MapPreset              preset = MakePreset();
    const vx::TerrainGenerationParams params = vx::TerrainGenerationParams::Default();

    ScopedTempFile file("match");
    vx::BakeMacroHeightTilesIntoPremadeMap(preset, params, file.path);

    vx::PremadeTerrainTileSource source(vx::PremadeMapReader::Open(file.path));
    // 烘焙与运行**同源**的前提：容器里记录的半径 / 种子来自同一份预设。
    EXPECT_EQ(source.Reader().TileRadiusX(), preset.tileRadiusX);
    EXPECT_EQ(source.Reader().TileRadiusZ(), preset.tileRadiusZ);
    EXPECT_EQ(source.Reader().Seed(), preset.seed);

    const vx::TerrainNoiseGenerator noise(preset.seed, params);

    int compared = 0;
    for (int tileZ = -preset.tileRadiusZ; tileZ <= preset.tileRadiusZ; ++tileZ) {
        for (int tileX = -preset.tileRadiusX; tileX <= preset.tileRadiusX; ++tileX) {
            vx::TerrainTile fromSource;
            fromSource.coord = vx::TileCoord { tileX, tileZ };
            ASSERT_TRUE(source.FillTileHeights(fromSource)) << "tile (" << tileX << ", " << tileZ << ")";

            const vx::TerrainTile procedural = vx::GenerateTerrainTileData(noise, preset.edits, tileX, tileZ);
            ASSERT_EQ(fromSource.heights.size(), procedural.heights.size());
            for (std::size_t i = 0; i < procedural.heights.size(); ++i) {
                ASSERT_EQ(fromSource.heights[i], procedural.heights[i])
                    << "tile (" << tileX << ", " << tileZ << ") 第 " << i << " 个高度不同";
            }
            // 缓存也必须被刷新（否则阴影投射体盒会偏小 ⇒ 两条路径表现不一致）。
            EXPECT_FLOAT_EQ(fromSource.maxSurfaceBlocks, procedural.maxSurfaceBlocks);
            ++compared;
        }
    }
    EXPECT_EQ(compared, 9);
    EXPECT_EQ(source.MissingChunkCount(), 0U);
}

TEST(PremadeTerrainSource, MissingChunkReturnsFalseAndCounts) {
    const vx::MapPreset              preset = MakePreset();
    const vx::TerrainGenerationParams params = vx::TerrainGenerationParams::Default();

    ScopedTempFile file("missing");
    vx::BakeMacroHeightTilesIntoPremadeMap(preset, params, file.path);
    vx::PremadeTerrainTileSource source(vx::PremadeMapReader::Open(file.path));

    // 半径外（未烘焙）⇒ false，且**计数**（不是静默返回 true）。
    vx::TerrainTile outside;
    outside.coord = vx::TileCoord { 7, 7 };
    EXPECT_FALSE(source.FillTileHeights(outside));
    EXPECT_EQ(source.MissingChunkCount(), 1U);
    EXPECT_FALSE(source.FillTileHeights(outside));
    EXPECT_EQ(source.MissingChunkCount(), 2U);
}

TEST(PremadeTerrainSource, OpenMissingFileThrows) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "voxel_premade_src_definitely_missing_123456789.vxmap";
    std::error_code error;
    std::filesystem::remove(missing, error);  // 确保确实不存在
    EXPECT_THROW((void)vx::PremadeMapReader::Open(missing), std::runtime_error);
}
