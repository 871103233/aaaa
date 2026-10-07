// V0.10 / S2：高度场脏列差量的**采集 / 叠加**与 `.voxr` 落盘往返
// （[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策二~四）。
//
// 覆盖判据：② **只存脏数据**（改 3 列 ⇒ 文件里只有这 3 条 entry、未改动 tile 不出现）；
// ① **往返逐位一致**（写 → 读 → 与源世界逐值相同）；以及红线 12（共享边界列不裂缝）。

#include "save/world_save.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using vx::DecodeHeightDirtyTile;
using vx::EncodeHeightDirtyTile;
using vx::Height;
using vx::HeightDirtyEntry;
using vx::kMaxTerrainHeightUnits;
using vx::kTerrainTileSize;
using vx::kTerrainTileVertexCount;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainWorld;
using vx::TileCoord;
using vx::WorldSaveChunkKey;
using vx::WorldSaveChunkKind;
using vx::WorldSaveHeader;
using vx::WorldSaveReader;
using vx::WorldSaveWriter;

constexpr std::uint64_t kSeed = 0x5EED0010ULL;

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

/// 本地 `(i, j)` ⇒ 冻结格式的列号（行主序）。
[[nodiscard]] std::uint16_t ColumnIndex(int i, int j) {
    return static_cast<std::uint16_t>(j * kTerrainTileSize + i);
}

/// 把导出项写成一个 `.voxr`（只写高度场脏列块；头部其余字段用最小可用值）。
void WriteEdits(const std::filesystem::path& path, const std::vector<TerrainWorld::HeightEditTile>& exports) {
    WorldSaveWriter writer;
    WorldSaveHeader header;
    header.worldSeed = static_cast<std::int64_t>(kSeed);
    writer.SetHeader(header);
    for (const TerrainWorld::HeightEditTile& group : exports) {
        writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, group.coord.x, 0, group.coord.z },
                        EncodeHeightDirtyTile(group.entries));
    }
    writer.WriteToFile(path);
}

/// 世界列 `(x, z)` 的**生成基线高度**（读档前的初值）。
[[nodiscard]] Height BaseAt(TerrainWorld& world, int x, int z) {
    Height     base = 0;
    const bool ok   = world.ReadColumnHeight(x, z, base);
    EXPECT_TRUE(ok) << "前置：该世界列必须已常驻";
    return base;
}

/// 把世界列 `(x, z)` 写成 `生成基线 + offset`（钳制到世界垂直范围），返回**实际差值**。
[[nodiscard]] std::int16_t WriteOffset(TerrainWorld& world, int x, int z, int offset,
                                       std::vector<TileCoord>& dirtyOut) {
    const Height base   = BaseAt(world, x, z);
    const Height target = static_cast<Height>(std::clamp(static_cast<int>(base) + offset, 0, kMaxTerrainHeightUnits));
    world.WriteColumnHeight(x, z, target, dirtyOut);
    return static_cast<std::int16_t>(static_cast<int>(target) - static_cast<int>(base));
}

}  // namespace

// ---------------------------------------------------------------------------
// 采集
// ---------------------------------------------------------------------------

TEST(HeightEdits, UneditedWorldExportsNothing) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);

    EXPECT_EQ(world.EditedTileCount(), 0U);
    EXPECT_TRUE(world.ExportHeightEdits().empty());
    EXPECT_FALSE(world.IsTileEdited(TileCoord { 0, 0 }));
}

TEST(HeightEdits, ThreeEditedColumnsProduceThreeAscendingEntries) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);

    std::vector<TileCoord> dirty;
    const std::int16_t deltaA = WriteOffset(world, 10, 20, 40, dirty);
    const std::int16_t deltaB = WriteOffset(world, 11, 20, -25, dirty);
    const std::int16_t deltaC = WriteOffset(world, 10, 21, 7, dirty);
    ASSERT_NE(deltaA, 0);
    ASSERT_NE(deltaB, 0);
    ASSERT_NE(deltaC, 0);

    EXPECT_EQ(world.EditedTileCount(), 1U);
    EXPECT_TRUE(world.IsTileEdited(TileCoord { 0, 0 }));

    const std::vector<TerrainWorld::HeightEditTile> exports = world.ExportHeightEdits();
    ASSERT_EQ(exports.size(), 1U);
    EXPECT_EQ(exports[0].coord, (TileCoord { 0, 0 }));
    ASSERT_EQ(exports[0].entries.size(), 3U) << "只有被改的 3 列是脏列";

    // 行主序 `(j, i)` ⇒ 列号严格升序（`EncodeHeightDirtyTile` 的前置条件）
    EXPECT_EQ(exports[0].entries[0].columnIndex, ColumnIndex(10, 20));
    EXPECT_EQ(exports[0].entries[0].heightDelta, deltaA);
    EXPECT_EQ(exports[0].entries[1].columnIndex, ColumnIndex(11, 20));
    EXPECT_EQ(exports[0].entries[1].heightDelta, deltaB);
    EXPECT_EQ(exports[0].entries[2].columnIndex, ColumnIndex(10, 21));
    EXPECT_EQ(exports[0].entries[2].heightDelta, deltaC);
    EXPECT_LT(exports[0].entries[0].columnIndex, exports[0].entries[1].columnIndex);
    EXPECT_LT(exports[0].entries[1].columnIndex, exports[0].entries[2].columnIndex);
}

TEST(HeightEdits, EditedThenUnloadedTileIsSkippedInsteadOfCrashed) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);

    std::vector<TileCoord> dirty;
    ASSERT_NE(WriteOffset(world, 10, 20, 55, dirty), 0);
    ASSERT_TRUE(world.IsTileEdited(TileCoord { 0, 0 }));

    ASSERT_TRUE(world.UnloadTile(0, 0));
    // 已卸载 ⇒ 不凭空重算（差量应在**卸载前**采集，ADR 0037 决策四 / S4）
    EXPECT_TRUE(world.ExportHeightEdits().empty());
    EXPECT_TRUE(world.ExportTileEdits(TileCoord { 0, 0 }).empty());
}

TEST(HeightEdits, SingleTileExportMatchesWholeExport) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);

    std::vector<TileCoord> dirty;
    ASSERT_NE(WriteOffset(world, 10, 20, 40, dirty), 0);
    ASSERT_NE(WriteOffset(world, 1 * kTerrainTileSize + 5, 30, -17, dirty), 0);  // 落在 tile(1,0) 内

    const std::vector<TerrainWorld::HeightEditTile> all = world.ExportHeightEdits();
    ASSERT_EQ(all.size(), 2U);
    for (const TerrainWorld::HeightEditTile& group : all) {
        const std::vector<HeightDirtyEntry> perTile = world.ExportTileEdits(group.coord);
        ASSERT_EQ(perTile.size(), group.entries.size()) << "单 tile 采集必须与整批采集条数一致";
        for (std::size_t k = 0; k < perTile.size(); ++k) {
            EXPECT_EQ(perTile[k].columnIndex, group.entries[k].columnIndex);
            EXPECT_EQ(perTile[k].heightDelta, group.entries[k].heightDelta);
        }
    }

    // 未编辑的第三个 tile ⇒ 空
    world.LoadTile(0, 1);
    EXPECT_TRUE(world.ExportTileEdits(TileCoord { 0, 1 }).empty());
    EXPECT_FALSE(world.IsTileEdited(TileCoord { 0, 1 }));
}

TEST(HeightEdits, EditedTilesListsExactlyTheEditedCoordsInOrder) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);
    world.LoadTile(0, 1);

    std::vector<TileCoord> dirty;
    ASSERT_NE(WriteOffset(world, 1 * kTerrainTileSize + 2, 3, 21, dirty), 0);  // tile(1,0)
    ASSERT_NE(WriteOffset(world, 4, 1 * kTerrainTileSize + 6, 33, dirty), 0);  // tile(0,1)

    const std::vector<TileCoord> edited = world.EditedTiles();
    ASSERT_EQ(edited.size(), 2U);
    EXPECT_EQ(edited[0], (TileCoord { 0, 1 }));
    EXPECT_EQ(edited[1], (TileCoord { 1, 0 }));
    EXPECT_EQ(world.EditedTileCount(), 2U);
}

TEST(HeightEdits, EditSerialBumpsOnlyWhenTerrainIsWritten) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);

    const std::uint64_t before = world.EditSerial();
    std::vector<TileCoord> dirty;
    ASSERT_NE(WriteOffset(world, 10, 20, 5, dirty), 0);
    EXPECT_GT(world.EditSerial(), before) << "写地形 ⇒ 序号必变（S4 的 flush 判据依赖它）";

    // 只读操作**不得**改序号（否则每帧都会误判"有改动" ⇒ flush 空转）
    const std::uint64_t after = world.EditSerial();
    Height              ignored = 0;
    (void)world.ReadColumnHeight(10, 20, ignored);
    (void)world.ExportTileEdits(TileCoord { 0, 0 });
    (void)world.ExportHeightEdits();
    (void)world.EditedTiles();
    EXPECT_EQ(world.EditSerial(), after);
}

// ---------------------------------------------------------------------------
// 落盘：只存脏数据 + 往返一致
// ---------------------------------------------------------------------------

TEST(HeightEdits, UntouchedResidentTilesAreAbsentFromFile) {
    const TempPath file("vx_terrain_edits_dirty_only.voxr");

    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);  // 常驻但**未改动**

    std::vector<TileCoord> dirty;
    ASSERT_NE(WriteOffset(world, 10, 20, 55, dirty), 0);

    const std::vector<TerrainWorld::HeightEditTile> exports = world.ExportHeightEdits();
    ASSERT_EQ(exports.size(), 1U);
    EXPECT_EQ(exports[0].coord, (TileCoord { 0, 0 }));
    EXPECT_FALSE(world.IsTileEdited(TileCoord { 1, 0 }));

    WriteEdits(file.path(), exports);

    const WorldSaveReader reader = WorldSaveReader::Open(file.path());
    EXPECT_EQ(reader.ChunkCount(), 1U) << "未改动 tile 不得落盘（判据②）";
    EXPECT_FALSE(reader.HasChunk(WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, 1, 0, 0 }));
}

TEST(HeightEdits, RoundTripsThroughVoxrFileBitExactly) {
    const TempPath file("vx_terrain_edits_roundtrip.voxr");

    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    std::vector<TileCoord> scratch;
    const std::int16_t d0 = WriteOffset(world, 10, 20, 37, scratch);
    const std::int16_t d1 = WriteOffset(world, 11, 20, 71, scratch);
    const std::int16_t d2 = WriteOffset(world, 10, 21, 113, scratch);
    ASSERT_NE(d0, 0);
    ASSERT_NE(d1, 0);
    ASSERT_NE(d2, 0);

    const std::vector<TerrainWorld::HeightEditTile> exports = world.ExportHeightEdits();
    ASSERT_EQ(exports.size(), 1U);
    ASSERT_EQ(exports[0].entries.size(), 3U);

    WriteEdits(file.path(), exports);

    // 文件里只有 1 个块、载荷解码后仍是这 3 条 entry
    const WorldSaveReader reader = WorldSaveReader::Open(file.path());
    ASSERT_EQ(reader.ChunkCount(), 1U);
    const WorldSaveChunkKey key { WorldSaveChunkKind::HeightDirtyTile, 0, 0, 0 };
    ASSERT_TRUE(reader.HasChunk(key));
    const std::vector<HeightDirtyEntry> decoded = DecodeHeightDirtyTile(reader.ReadChunk(key));
    ASSERT_EQ(decoded.size(), 3U);

    // 新世界：**先生成 → 再叠加**（顺序不得颠倒，ADR 0037 决策三）⇒ 与源世界逐位一致
    TerrainWorld restored(kSeed, TerrainMaterialTable::Default());
    restored.LoadTile(0, 0);
    std::vector<TileCoord> dirty;
    restored.ApplyHeightEdits(TileCoord { 0, 0 }, decoded, dirty);
    EXPECT_FALSE(dirty.empty());
    EXPECT_TRUE(restored.IsTileEdited(TileCoord { 0, 0 }));

    const TerrainTile* sourceTile   = world.FindTile(0, 0);
    const TerrainTile* restoredTile = restored.FindTile(0, 0);
    ASSERT_NE(sourceTile, nullptr);
    ASSERT_NE(restoredTile, nullptr);
    EXPECT_EQ(Snapshot(*restoredTile), Snapshot(*sourceTile));
}

// ---------------------------------------------------------------------------
// 共享边界列（红线 12）
// ---------------------------------------------------------------------------

TEST(HeightEdits, SharedBoundaryColumnSavesOnceAndKeepsTilesSeamless) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    world.LoadTile(1, 0);

    // 世界列 `x = 64` 是 tile(0,0) 的本地 `i = 64`（**边界层**）与 tile(1,0) 的本地 `i = 0`（**拥有者**）。
    std::vector<TileCoord> dirty;
    const Height       base  = BaseAt(world, 64, 20);
    const std::int16_t delta = WriteOffset(world, 64, 20, 50, dirty);
    ASSERT_NE(delta, 0);
    const Height expected = static_cast<Height>(static_cast<int>(base) + static_cast<int>(delta));

    EXPECT_TRUE(world.IsTileEdited(TileCoord { 0, 0 }));  // 边界层同步被改（否则裂缝）
    EXPECT_TRUE(world.IsTileEdited(TileCoord { 1, 0 }));

    const std::vector<TerrainWorld::HeightEditTile> exports = world.ExportHeightEdits();
    ASSERT_EQ(exports.size(), 1U) << "边界列只应由**拥有它的** tile(1,0) 导出一次";
    EXPECT_EQ(exports[0].coord, (TileCoord { 1, 0 }));
    ASSERT_EQ(exports[0].entries.size(), 1U);
    EXPECT_EQ(exports[0].entries[0].columnIndex, ColumnIndex(0, 20));

    TerrainWorld restored(kSeed, TerrainMaterialTable::Default());
    restored.LoadTile(0, 0);
    restored.LoadTile(1, 0);
    std::vector<TileCoord> restoreDirty;
    restored.ApplyHeightEdits(exports[0].coord, exports[0].entries, restoreDirty);

    const TerrainTile* left  = restored.FindTile(0, 0);
    const TerrainTile* right = restored.FindTile(1, 0);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->At(64, 20), right->At(0, 20)) << "共享边界列必须逐位相等（红线 12：不裂缝）";
    EXPECT_EQ(left->At(64, 20), expected);

    EXPECT_EQ(Snapshot(*restored.FindTile(0, 0)), Snapshot(*world.FindTile(0, 0)));
    EXPECT_EQ(Snapshot(*restored.FindTile(1, 0)), Snapshot(*world.FindTile(1, 0)));
}

// ---------------------------------------------------------------------------
// 叠加语义
// ---------------------------------------------------------------------------

TEST(HeightEdits, ApplyIsIdempotent) {
    TerrainWorld source(kSeed, TerrainMaterialTable::Default());
    source.LoadTile(0, 0);
    std::vector<TileCoord> scratch;
    const std::int16_t d0 = WriteOffset(source, 10, 20, 60, scratch);
    const std::int16_t d1 = WriteOffset(source, 30, 40, 90, scratch);
    ASSERT_NE(d0, 0);
    ASSERT_NE(d1, 0);
    const std::vector<TerrainWorld::HeightEditTile> exports = source.ExportHeightEdits();
    ASSERT_EQ(exports.size(), 1U);
    ASSERT_EQ(exports[0].entries.size(), 2U);

    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    world.LoadTile(0, 0);
    std::vector<TileCoord> dirty;
    world.ApplyHeightEdits(exports[0].coord, exports[0].entries, dirty);
    const HeightGrid once = Snapshot(*world.FindTile(0, 0));

    dirty.clear();
    world.ApplyHeightEdits(exports[0].coord, exports[0].entries, dirty);
    EXPECT_EQ(Snapshot(*world.FindTile(0, 0)), once) << "目标值 = 生成结果 + 差量 ⇒ 重复叠加结果相同";
}

TEST(HeightEdits, ApplyToNonResidentTileIsNoOp) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    std::vector<TileCoord> dirty;
    world.ApplyHeightEdits(TileCoord { 5, 5 }, { HeightDirtyEntry { ColumnIndex(1, 1), 10 } }, dirty);
    EXPECT_TRUE(dirty.empty());
    EXPECT_FALSE(world.HasTile(5, 5));
    EXPECT_FALSE(world.IsTileEdited(TileCoord { 5, 5 }));
}
