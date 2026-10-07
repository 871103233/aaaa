// V0.10 / S4：世界状态**差量会话**与**异步写盘器**的单测
// （[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策四 / 五）。
//
// 覆盖：① 会话往返（写 → 读 → 逐块逐字节一致）；② 空档 / 缺档不算错误；
// ③ 脏标记（决定"还要不要写盘"）；④ 损坏档 ⇒ 抛（不静默）；⑤ 写盘器的**单飞**契约与错误上报。

#include "save/world_save.hpp"
#include "save/world_state_save.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::DecodeHeightDirtyTile;
using vx::EncodeHeightDirtyTile;
using vx::Height;
using vx::HeightDirtyEntry;
using vx::kTerrainTileSize;
using vx::kTerrainTileVertexCount;
using vx::kVolumeSaveVoxelCount;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainWorld;
using vx::TileCoord;
using vx::VolumeDirtyPayload;
using vx::WorldSaveChunkKey;
using vx::WorldSaveChunkKind;
using vx::WorldSaveHeader;
using vx::WorldSaveFlusher;
using vx::WorldStateSave;

/// 唯一临时文件路径（析构删除；同时清掉 `.tmp`）。
class TempPath {
public:
    explicit TempPath(std::string name) : path_(std::filesystem::temp_directory_path() / std::move(name)) {}
    ~TempPath() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + ".tmp", error);
    }
    TempPath(const TempPath&)            = delete;
    TempPath& operator=(const TempPath&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] WorldSaveHeader MakeHeader() {
    WorldSaveHeader header;
    header.generatorVersion       = 0x0123456789ABCDEFULL;
    header.tileRadiusX            = 40;
    header.tileRadiusZ            = 40;
    header.worldSeed              = 0x5EED0010ULL;
    header.digRegionSchemaVersion = 1;
    header.digRegionContentHash   = 0xFEEDFACECAFEBEEFULL;
    return header;
}

[[nodiscard]] WorldSaveChunkKey HeightKey(int tileX, int tileZ) {
    return WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, tileX, 0, tileZ };
}

[[nodiscard]] WorldSaveChunkKey VolumeKey(int x, int y, int z) {
    return WorldSaveChunkKey { WorldSaveChunkKind::VolumeDirtyBlock, x, y, z };
}

[[nodiscard]] std::vector<std::uint8_t> MakeHeightPayload(std::int16_t delta) {
    return EncodeHeightDirtyTile({ HeightDirtyEntry { 7U, delta }, HeightDirtyEntry { 4095U, -3 } });
}

[[nodiscard]] std::vector<std::uint8_t> MakeVolumePayload() {
    VolumeDirtyPayload payload;
    payload.materialPresent = true;
    payload.density.assign(kVolumeSaveVoxelCount, 0);
    payload.material.assign(kVolumeSaveVoxelCount, 0xFFU);
    payload.density[0]  = -100;
    payload.material[5] = 3U;
    return vx::EncodeVolumeDirtyBlock(payload);
}

void WriteGarbage(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char    bytes[] = "NOT-A-VOXR-FILE";
    out.write(bytes, static_cast<std::streamsize>(sizeof(bytes) - 1U));
}

}  // namespace

// ---------------------------------------------------------------------------
// 会话
// ---------------------------------------------------------------------------

TEST(WorldStateSave, RoundTripsHeaderAndChunksByteForByte) {
    const TempPath file("vx_world_state_roundtrip.voxr");

    WorldStateSave source;
    source.SetHeader(MakeHeader());
    source.SetChunk(HeightKey(0, 0), MakeHeightPayload(11));
    source.SetChunk(HeightKey(-2, 5), MakeHeightPayload(-1234));
    source.SetChunk(VolumeKey(1, 2, 3), MakeVolumePayload());
    ASSERT_EQ(source.ChunkCount(), 3U);
    source.WriteToFile(file.path());

    bool                 found = false;
    const WorldStateSave loaded = WorldStateSave::LoadFromFile(file.path(), found);
    ASSERT_TRUE(found);
    EXPECT_EQ(loaded.Header().worldSeed, source.Header().worldSeed);
    EXPECT_EQ(loaded.Header().generatorVersion, source.Header().generatorVersion);
    EXPECT_EQ(loaded.Header().tileRadiusX, source.Header().tileRadiusX);
    EXPECT_EQ(loaded.Header().digRegionContentHash, source.Header().digRegionContentHash);
    ASSERT_EQ(loaded.ChunkCount(), 3U);

    // 逐块逐字节一致（含**解码后**的语义一致）
    ASSERT_NE(loaded.FindChunk(HeightKey(-2, 5)), nullptr);
    EXPECT_EQ(*loaded.FindChunk(HeightKey(-2, 5)), *source.FindChunk(HeightKey(-2, 5)));
    const std::vector<HeightDirtyEntry> decoded = DecodeHeightDirtyTile(*loaded.FindChunk(HeightKey(-2, 5)));
    ASSERT_EQ(decoded.size(), 2U);
    EXPECT_EQ(decoded[0].heightDelta, -1234);
    EXPECT_EQ(decoded[1].columnIndex, 4095U);
    EXPECT_EQ(*loaded.FindChunk(VolumeKey(1, 2, 3)), *source.FindChunk(VolumeKey(1, 2, 3)));
    EXPECT_EQ(loaded.FindChunk(HeightKey(9, 9)), nullptr);
}

TEST(WorldStateSave, MissingFileIsNotAnError) {
    const TempPath       file("vx_world_state_absent.voxr");
    bool                 found = true;
    const WorldStateSave loaded = WorldStateSave::LoadFromFile(file.path(), found);
    EXPECT_FALSE(found) << "首次运行：无可读档 ⇒ found=false，**不报错**";
    EXPECT_EQ(loaded.ChunkCount(), 0U);
}

TEST(WorldStateSave, CorruptFileThrowsInsteadOfSilentlySucceeding) {
    const TempPath file("vx_world_state_corrupt.voxr");
    WriteGarbage(file.path());
    bool found = false;
    EXPECT_THROW((void)WorldStateSave::LoadFromFile(file.path(), found), std::runtime_error);
}

TEST(WorldStateSave, EmptyPayloadErasesChunkAndEraseIsIdempotent) {
    WorldStateSave save;
    save.SetHeader(MakeHeader());

    save.SetChunk(HeightKey(1, 1), MakeHeightPayload(5));
    EXPECT_EQ(save.ChunkCount(), 1U);
    EXPECT_TRUE(save.HasChunk(HeightKey(1, 1)));

    save.EraseChunk(HeightKey(9, 9));  // 删不存在的键 ⇒ 无操作（不得崩 / 不得改计数）
    EXPECT_EQ(save.ChunkCount(), 1U);

    save.EraseChunk(HeightKey(1, 1));
    EXPECT_EQ(save.ChunkCount(), 0U);

    // 空载荷等价于删除（空块在 S1 的写入器里是被拒的 ⇒ 这里必须挡住）
    save.SetChunk(HeightKey(3, 3), {});
    EXPECT_EQ(save.ChunkCount(), 0U);
    EXPECT_FALSE(save.HasChunk(HeightKey(3, 3)));
}

// ---------------------------------------------------------------------------
// 异步写盘器
// ---------------------------------------------------------------------------

TEST(WorldSaveFlusher, WritesAndReportsSuccess) {
    const TempPath file("vx_world_state_flusher_ok.voxr");

    WorldStateSave snapshot;
    snapshot.SetHeader(MakeHeader());
    snapshot.SetChunk(HeightKey(4, 4), MakeHeightPayload(19));

    WorldSaveFlusher flusher;
    ASSERT_TRUE(flusher.Submit(std::move(snapshot), file.path()));
    std::string error = "未收包";
    flusher.WaitForIdle(error);
    EXPECT_TRUE(error.empty()) << "写盘应成功：" << error;

    bool                 found = false;
    const WorldStateSave loaded = WorldStateSave::LoadFromFile(file.path(), found);
    ASSERT_TRUE(found);
    EXPECT_EQ(loaded.ChunkCount(), 1U);
    EXPECT_EQ(flusher.Poll(error), false) << "没有在飞任务 ⇒ Poll 无事可做";
}

TEST(WorldSaveFlusher, SingleFlightRejectsSecondSubmitUntilReaped) {
    const TempPath first("vx_world_state_flusher_a.voxr");
    const TempPath second("vx_world_state_flusher_b.voxr");

    WorldStateSave snapshot;
    snapshot.SetHeader(MakeHeader());
    snapshot.SetChunk(HeightKey(0, 0), MakeHeightPayload(1));

    WorldSaveFlusher flusher;
    ASSERT_TRUE(flusher.Submit(snapshot, first.path()));
    // 回收只发生在 `Poll` / `WaitForIdle` ⇒ 未收包时**一律**拒绝第二次提交（不排队堆积、错误不会被悄悄丢掉）。
    EXPECT_FALSE(flusher.Submit(snapshot, second.path()));

    std::string error;
    flusher.WaitForIdle(error);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(flusher.Submit(std::move(snapshot), second.path())) << "回收后可再次提交";
    flusher.WaitForIdle(error);
    EXPECT_TRUE(error.empty());
}

TEST(WorldSaveFlusher, ReportsWriteFailureInsteadOfSwallowingIt) {
    WorldStateSave snapshot;
    snapshot.SetHeader(MakeHeader());
    snapshot.SetChunk(HeightKey(0, 0), MakeHeightPayload(1));

    WorldSaveFlusher flusher;
    // 父目录不存在 ⇒ `WriteToFile` 必须失败（不静默）。
    const std::filesystem::path bad =
        std::filesystem::temp_directory_path() / "vx_no_such_dir_5EED" / "deep" / "state.voxr";
    ASSERT_TRUE(flusher.Submit(std::move(snapshot), bad));

    std::string error;
    flusher.WaitForIdle(error);
    EXPECT_FALSE(error.empty()) << "失败必须上报给调用方（而不是被吞掉）";
}

// ---------------------------------------------------------------------------
// 与地形层的**端到端**：卸载前采集 ⇒ 写盘 ⇒ 新世界生成后叠加（判据②"卸载不丢改动"）
// ---------------------------------------------------------------------------

namespace {

using HeightGrid = std::array<Height, static_cast<std::size_t>(kTerrainTileVertexCount) *
                                          static_cast<std::size_t>(kTerrainTileVertexCount)>;

/// 整张 tile 的高度快照（含共享边界层）—— 用于**逐位**比对。
[[nodiscard]] HeightGrid Snapshot(const TerrainTile& tile) {
    HeightGrid copy {};
    for (int j = 0; j < kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < kTerrainTileVertexCount; ++i) {
            copy[TerrainTile::Index(i, j)] = tile.At(i, j);
        }
    }
    return copy;
}

[[nodiscard]] WorldSaveChunkKey TerrainKey(const TileCoord& coord) {
    return WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, coord.x, 0, coord.z };
}

}  // namespace

TEST(WorldStateSaveSession, DirtyTilesSurviveUnloadThroughFile) {
    const TempPath      file("vx_world_state_unload_roundtrip.voxr");
    const std::uint64_t seed = 0x5EED00A0ULL;

    HeightGrid    expectedLeft {};
    HeightGrid    expectedRight {};
    WorldStateSave session;
    {
        // ---- 世界 A：编辑 → **卸载前采集**（= `WorldStatePersistence::RecordTile` 的动作）----
        TerrainWorld world(seed, TerrainMaterialTable::Default());
        world.LoadTile(0, 0);
        world.LoadTile(1, 0);

        std::vector<TileCoord> dirty;
        Height                 base = 0;
        ASSERT_TRUE(world.ReadColumnHeight(10, 20, base));
        world.WriteColumnHeight(10, 20, static_cast<Height>(base + 60), dirty);
        ASSERT_TRUE(world.ReadColumnHeight(kTerrainTileSize + 3, 40, base));
        world.WriteColumnHeight(kTerrainTileSize + 3, 40, static_cast<Height>(base - 45), dirty);

        expectedLeft  = Snapshot(*world.FindTile(0, 0));
        expectedRight = Snapshot(*world.FindTile(1, 0));

        for (const TileCoord& coord : world.EditedTiles()) {
            const std::vector<HeightDirtyEntry> entries = world.ExportTileEdits(coord);
            ASSERT_FALSE(entries.empty());
            session.SetChunk(TerrainKey(coord), EncodeHeightDirtyTile(entries));
        }
        ASSERT_TRUE(world.UnloadTile(0, 0));  // 走远 ⇒ 两个 tile 都离开常驻集合
        ASSERT_TRUE(world.UnloadTile(1, 0));
        EXPECT_TRUE(world.ExportHeightEdits().empty()) << "卸载后无法再采集 ⇒ 只能依赖会话里已存的那一份";
    }

    WorldSaveHeader header = MakeHeader();
    header.worldSeed       = static_cast<std::int64_t>(seed);
    session.SetHeader(header);
    session.WriteToFile(file.path());

    // ---- 世界 B：同种子重建 → 生成 → 叠加会话里的差量 ⇒ 与 A 卸载前**逐位一致** ----
    bool                 found = false;
    const WorldStateSave loaded = WorldStateSave::LoadFromFile(file.path(), found);
    ASSERT_TRUE(found);
    ASSERT_EQ(loaded.ChunkCount(), 2U);

    const WorldSaveChunkKey keys[2] = { TerrainKey(TileCoord { 0, 0 }), TerrainKey(TileCoord { 1, 0 }) };
    TerrainWorld            restored(seed, TerrainMaterialTable::Default());
    restored.LoadTile(0, 0);
    restored.LoadTile(1, 0);
    for (const WorldSaveChunkKey& key : keys) {
        const std::vector<std::uint8_t>* raw = loaded.FindChunk(key);
        ASSERT_NE(raw, nullptr);
        std::vector<TileCoord> touched;
        restored.ApplyHeightEdits(TileCoord { key.x, key.z }, DecodeHeightDirtyTile(*raw), touched);
        EXPECT_FALSE(touched.empty());
    }
    EXPECT_EQ(Snapshot(*restored.FindTile(0, 0)), expectedLeft);
    EXPECT_EQ(Snapshot(*restored.FindTile(1, 0)), expectedRight);
}
