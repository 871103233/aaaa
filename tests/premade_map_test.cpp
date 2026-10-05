// W2 / [ADR 0026]：预制地图**容器格式**的单测。
//
// 覆盖四条可判定判据：
//   ① 确定性 —— 同一输入两次写盘 ⇒ 文件**逐字节相同**（红线 7）；
//   ② 随机访问 —— 往返**逐值一致**；不存在的块 ⇒ 抛（不返回空 / 不静默）；
//   ③ 版本与损坏 —— `schema_version` 不符 / 魔数错 / 文件缺失 ⇒ 抛；
//   ④ 边界 —— 空块 ⇒ 抛。

#include "premade/premade_map.hpp"
#include "premade/premade_bake.hpp"

#include "generation/map_preset.hpp"
#include "generation/terrain_noise.hpp"
#include "terrain/terrain_tile.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using vx::PremadeChunkKey;
using vx::PremadeChunkKind;
using vx::PremadeMapReader;
using vx::PremadeMapWriter;

/// 每个用例使用独立临时文件，析构时清理（避免残留互相干扰）。
struct ScopedTempFile {
    std::filesystem::path path;

    explicit ScopedTempFile(const char* name)
        : path(std::filesystem::temp_directory_path() / (std::string("voxel_premade_") + name + ".bin")) {}

    ~ScopedTempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    ScopedTempFile(const ScopedTempFile&)            = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
};

/// 可复现的字节图案（内容不重要，关键是"同一输入 ⇒ 同一字节"）。
[[nodiscard]] std::vector<std::uint8_t> Pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 31U + static_cast<std::size_t>(seed) * 7U) & 0xFFU);
    }
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteWholeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// 三块内容不同、坐标含负数的固定输入。
[[nodiscard]] PremadeMapWriter MakeWriter() {
    PremadeMapWriter writer;
    writer.SetWorldInfo(8, 8, 0x5EED1234ULL);
    writer.SetChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, -1, 0, 2 }, Pattern(4096, 1));
    writer.SetChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 0, 0, 0 }, Pattern(8192, 2));
    writer.SetChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 3, 0, -4 }, Pattern(100, 3));
    return writer;
}

}  // namespace

// ① 往返逐值一致 + 头字段（世界范围 / 种子 / 块数）。
TEST(PremadeMap, RoundTripsChunksBitForBit) {
    ScopedTempFile file("roundtrip");
    MakeWriter().WriteToFile(file.path);

    const PremadeMapReader reader = PremadeMapReader::Open(file.path);
    EXPECT_EQ(reader.SchemaVersion(), PremadeMapReader::kSchemaVersion);
    EXPECT_EQ(reader.TileRadiusX(), 8);
    EXPECT_EQ(reader.TileRadiusZ(), 8);
    EXPECT_EQ(reader.Seed(), 0x5EED1234ULL);
    EXPECT_EQ(reader.ChunkCount(), 3U);

    EXPECT_EQ(reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 0, 0, 0 }), Pattern(8192, 2));
    EXPECT_EQ(reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, -1, 0, 2 }), Pattern(4096, 1));
    EXPECT_EQ(reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 3, 0, -4 }), Pattern(100, 3));
}

// ② 随机访问：存在性判定正确；不存在的块**抛异常**（不返回空向量）。
TEST(PremadeMap, RandomAccessReportsPresenceAndRejectsMissingChunk) {
    ScopedTempFile file("randomaccess");
    MakeWriter().WriteToFile(file.path);
    const PremadeMapReader reader = PremadeMapReader::Open(file.path);

    EXPECT_TRUE(reader.HasChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 0, 0, 0 }));
    EXPECT_FALSE(reader.HasChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 5, 0, 5 }));
    EXPECT_THROW((void)reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 5, 0, 5 }),
                 std::runtime_error);
}

// ① 确定性：同一输入两次写盘 ⇒ 逐字节相同。
TEST(PremadeMap, WriteIsDeterministic) {
    ScopedTempFile a("deterministic_a");
    ScopedTempFile b("deterministic_b");
    MakeWriter().WriteToFile(a.path);
    MakeWriter().WriteToFile(b.path);
    EXPECT_EQ(ReadWholeFile(a.path), ReadWholeFile(b.path)) << "同一输入两次写盘必须逐字节相同（红线 7）";
}

// ③ `schema_version` 不符 ⇒ 打开即抛（ADR 0026 / ADR 0005 口径）。
TEST(PremadeMap, RejectsSchemaVersionMismatch) {
    ScopedTempFile file("badversion");
    MakeWriter().WriteToFile(file.path);

    std::vector<std::uint8_t> bytes = ReadWholeFile(file.path);
    ASSERT_GE(bytes.size(), 12U);
    bytes[8] = 0x63U;  // schema_version = 99（小端）
    bytes[9] = 0U;
    bytes[10] = 0U;
    bytes[11] = 0U;
    WriteWholeFile(file.path, bytes);

    EXPECT_THROW((void)PremadeMapReader::Open(file.path), std::runtime_error);
}

// ③ 魔数错 ⇒ 打开即抛（"这不是预制地图文件"）。
TEST(PremadeMap, RejectsCorruptMagic) {
    ScopedTempFile file("badmagic");
    MakeWriter().WriteToFile(file.path);

    std::vector<std::uint8_t> bytes = ReadWholeFile(file.path);
    ASSERT_FALSE(bytes.empty());
    bytes[0] = 'X';
    WriteWholeFile(file.path, bytes);

    EXPECT_THROW((void)PremadeMapReader::Open(file.path), std::runtime_error);
}

// ③ 文件缺失 ⇒ 抛（不静默返回空地图）。
TEST(PremadeMap, RejectsMissingFile) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "voxel_premade_definitely_does_not_exist.bin";
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_THROW((void)PremadeMapReader::Open(path), std::runtime_error);
}

// ④ 边界：空块非法（不得写出无法解释的空块）。
TEST(PremadeMap, RejectsEmptyChunk) {
    PremadeMapWriter writer;
    EXPECT_THROW(writer.SetChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 0, 0, 0 }, {}),
                 std::runtime_error);
}

// ---- W2-S2：离线烘焙（宏地形高度场）----

// 烘焙确定性：同一预设两次 ⇒ 逐字节相同；块数 = (2r+1)²、世界范围与种子回读正确。
TEST(PremadeMap, BakeIsDeterministicAndReportsWorldInfo) {
    ScopedTempFile a("bake_a");
    ScopedTempFile b("bake_b");

    vx::MapPreset preset;
    preset.seed        = 0x5EED1234ULL;
    preset.tileRadiusX = 1;
    preset.tileRadiusZ = 1;

    vx::BakeMacroHeightTilesIntoPremadeMap(preset, vx::TerrainGenerationParams::Default(), a.path);
    vx::BakeMacroHeightTilesIntoPremadeMap(preset, vx::TerrainGenerationParams::Default(), b.path);
    EXPECT_EQ(ReadWholeFile(a.path), ReadWholeFile(b.path)) << "同一预设两次烘焙必须逐字节相同";

    const PremadeMapReader reader = PremadeMapReader::Open(a.path);
    EXPECT_EQ(reader.ChunkCount(), 9U);  // (2×1+1)²
    EXPECT_EQ(reader.TileRadiusX(), 1);
    EXPECT_EQ(reader.TileRadiusZ(), 1);
    EXPECT_EQ(reader.Seed(), preset.seed);
    EXPECT_EQ(reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, 0, 0, 0 }).size(),
              vx::kMacroHeightTileBytes);
}

// 烘焙出的高度场必须与**运行时生成**逐位一致（同源 ⇒ 预制地图可等价替代运行时生成，红线 7）。
TEST(PremadeMap, BakedHeightTileMatchesRuntimeGeneration) {
    ScopedTempFile file("bake_match");

    vx::MapPreset preset;
    preset.seed        = 0x5EED9999ULL;
    preset.tileRadiusX = 1;
    preset.tileRadiusZ = 1;

    // 覆盖**地貌分区启用**的路径：烘焙与运行时生成必须用同一份参数。
    vx::TerrainGenerationParams params = vx::TerrainGenerationParams::Default();
    params.landform.enabled            = true;

    vx::BakeMacroHeightTilesIntoPremadeMap(preset, params, file.path);
    const PremadeMapReader reader = PremadeMapReader::Open(file.path);

    const vx::TerrainNoiseGenerator noise(preset.seed, params);
    for (const vx::TileCoord coord : { vx::TileCoord { 0, 0 }, vx::TileCoord { -1, -1 }, vx::TileCoord { 1, 1 } }) {
        // 运行时生成（与 `TerrainWorld::GenerateTile` 同一路径）。
        vx::TerrainTile runtime;
        runtime.coord = coord;
        vx::GenerateTerrainTile(runtime, noise);
        vx::ApplyMapEditsToTile(preset.edits, runtime);

        // 预制地图读回。
        vx::TerrainTile loaded;
        vx::DeserializeMacroHeightTile(
            reader.ReadChunk(PremadeChunkKey { PremadeChunkKind::MacroHeightTile, coord.x, 0, coord.z }), loaded);

        EXPECT_TRUE(loaded.heights == runtime.heights)
            << "tile (" << coord.x << ", " << coord.z << ") 的预制高度必须与运行时生成逐位一致";
    }
}
