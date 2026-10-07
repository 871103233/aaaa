// V0.10 / [ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md)：`.voxr` v2 容器与载荷编解码单测。
//
// 覆盖 ADR 0037 的可判定判据：① 往返一致；③ **确定性**（两次写盘逐字节相同）；
// ④ **版本纪律**（未知版本 / 魔数错 / 校验和不符 / 索引越界 ⇒ 拒绝）；
// 以及冻结格式的**载荷编解码**（脏列 / 脏体积块）与非法输入即抛。

#include "save/world_save.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::DecodeHeightDirtyTile;
using vx::DecodeVolumeDirtyBlock;
using vx::EncodeHeightDirtyTile;
using vx::EncodeVolumeDirtyBlock;
using vx::Fnv1a64;
using vx::HeightDirtyEntry;
using vx::kVolumeSaveVoxelCount;
using vx::VolumeDirtyPayload;
using vx::WorldSaveChunkKey;
using vx::WorldSaveChunkKind;
using vx::WorldSaveHeader;
using vx::WorldSaveReader;
using vx::WorldSaveWriter;

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
    header.generatorVersion       = 0x1122334455667788ULL;
    header.tileRadiusX            = 78;
    header.tileRadiusZ            = 78;
    header.worldSeed              = -1234567890123456789LL;
    header.digRegionSchemaVersion = 1;
    header.digRegionContentHash   = 0xAABBCCDDEEFF0011ULL;
    return header;
}

[[nodiscard]] std::vector<HeightDirtyEntry> MakeEntries() {
    return { HeightDirtyEntry { 3U, -7 }, HeightDirtyEntry { 100U, 4 }, HeightDirtyEntry { 4095U, 300 } };
}

[[nodiscard]] VolumeDirtyPayload MakeVolumePayload(bool withMaterial) {
    VolumeDirtyPayload payload;
    payload.materialPresent = withMaterial;
    payload.density.assign(kVolumeSaveVoxelCount, 0);
    payload.density[0]  = -50;
    payload.density[17] = 33;
    if (withMaterial) {
        payload.material.assign(kVolumeSaveVoxelCount, 0xFFU);
        payload.material[0] = 2U;
    }
    return payload;
}

[[nodiscard]] std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// --------------------------- 容器：往返 / 确定性 / 版本纪律 ---------------------------

/// 空档（只有头部）：头部字段逐值往返；`flags = 0`、`chunkCount = 0`；不留临时文件。
TEST(WorldSave, RoundTripsHeaderOnlyFile) {
    const TempPath file("vx_world_save_header.voxr");
    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    writer.WriteToFile(file.path());

    const WorldSaveReader reader = WorldSaveReader::Open(file.path());
    EXPECT_EQ(reader.Header().version, WorldSaveWriter::kSchemaVersion);
    EXPECT_EQ(reader.Header().flags, 0U);
    EXPECT_EQ(reader.Header().generatorVersion, 0x1122334455667788ULL);
    EXPECT_EQ(reader.Header().tileRadiusX, 78);
    EXPECT_EQ(reader.Header().tileRadiusZ, 78);
    EXPECT_EQ(reader.Header().worldSeed, -1234567890123456789LL);  // i64 负值无损
    EXPECT_EQ(reader.Header().digRegionSchemaVersion, 1);
    EXPECT_EQ(reader.Header().digRegionContentHash, 0xAABBCCDDEEFF0011ULL);
    EXPECT_EQ(reader.ChunkCount(), 0U);
    EXPECT_FALSE(std::filesystem::exists(file.path().string() + ".tmp"));  // 原子替换后不留临时文件
}

/// 高度场脏列：写 → 读 → **载荷逐字节一致** → 解码回原条目；`flags` 由块种类推导。
TEST(WorldSave, RoundTripsHeightDirtyTileChunk) {
    const TempPath file("vx_world_save_height.voxr");
    const std::vector<HeightDirtyEntry> entries = MakeEntries();
    const std::vector<std::uint8_t>     raw     = EncodeHeightDirtyTile(entries);

    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, 2, 0, -3 }, raw);
    writer.WriteToFile(file.path());

    const WorldSaveReader reader = WorldSaveReader::Open(file.path());
    EXPECT_EQ(reader.Header().flags, vx::kWorldSaveFlagHasHeightDirty);
    EXPECT_EQ(reader.ChunkCount(), 1U);
    const WorldSaveChunkKey key { WorldSaveChunkKind::HeightDirtyTile, 2, 0, -3 };
    ASSERT_TRUE(reader.HasChunk(key));
    EXPECT_EQ(reader.ReadChunk(key), raw);  // 容器层往返逐字节一致

    const std::vector<HeightDirtyEntry> decoded = DecodeHeightDirtyTile(reader.ReadChunk(key));
    ASSERT_EQ(decoded.size(), entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        EXPECT_EQ(decoded[index].columnIndex, entries[index].columnIndex);
        EXPECT_EQ(decoded[index].heightDelta, entries[index].heightDelta);
    }
}

/// 脏体积块：不带材质 / 带材质两条路径都往返；缺失的块 ⇒ `ReadChunk` 抛。
TEST(WorldSave, RoundTripsVolumeDirtyBlockChunks) {
    for (const bool withMaterial : { false, true }) {
        const TempPath file(withMaterial ? "vx_world_save_volume_mat.voxr" : "vx_world_save_volume.voxr");
        const VolumeDirtyPayload payload = MakeVolumePayload(withMaterial);
        const std::vector<std::uint8_t> raw = EncodeVolumeDirtyBlock(payload);

        WorldSaveWriter writer;
        writer.SetHeader(MakeHeader());
        writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::VolumeDirtyBlock, 0, 1, 0 }, raw);
        writer.WriteToFile(file.path());

        const WorldSaveReader reader = WorldSaveReader::Open(file.path());
        EXPECT_EQ(reader.Header().flags, vx::kWorldSaveFlagHasVolumeDirty);
        const WorldSaveChunkKey key { WorldSaveChunkKind::VolumeDirtyBlock, 0, 1, 0 };
        EXPECT_EQ(reader.ReadChunk(key), raw);

        const VolumeDirtyPayload decoded = DecodeVolumeDirtyBlock(reader.ReadChunk(key));
        EXPECT_EQ(decoded.materialPresent, withMaterial);
        EXPECT_EQ(decoded.density, payload.density);
        EXPECT_EQ(decoded.material, payload.material);

        // 未登记的块 ⇒ 抛（不静默返回空）。
        EXPECT_THROW(static_cast<void>(reader.ReadChunk(WorldSaveChunkKey { WorldSaveChunkKind::VolumeDirtyBlock, 9, 9, 9 })),
                     std::runtime_error);
    }
}

/// **确定性**（红线 7）：同一输入两次写盘 ⇒ 文件**逐字节相同**（块按 key 升序 + 固定 zstd 级别）。
TEST(WorldSave, WriteIsByteForByteDeterministic) {
    const TempPath first("vx_world_save_det_a.voxr");
    const TempPath second("vx_world_save_det_b.voxr");

    const auto writeInto = [](const std::filesystem::path& path) {
        WorldSaveWriter writer;
        writer.SetHeader(MakeHeader());
        // **乱序**加入三种块 ⇒ 仍必须按 key 升序落盘。
        writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::VolumeDirtyBlock, 1, 0, 1 },
                        EncodeVolumeDirtyBlock(MakeVolumePayload(true)));
        writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, 5, 0, 5 }, EncodeHeightDirtyTile(MakeEntries()));
        writer.SetChunk(WorldSaveChunkKey { WorldSaveChunkKind::HeightDirtyTile, -1, 0, 5 }, EncodeHeightDirtyTile(MakeEntries()));
        writer.WriteToFile(path);
    };
    writeInto(first.path());
    writeInto(second.path());

    EXPECT_EQ(ReadAllBytes(first.path()), ReadAllBytes(second.path()));
}

/// 未知 `version` ⇒ 拒绝（不静默迁移，红线 8）。
TEST(WorldSave, RejectsUnknownVersion) {
    const TempPath file("vx_world_save_badver.voxr");
    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    writer.WriteToFile(file.path());

    std::vector<std::uint8_t> bytes = ReadAllBytes(file.path());
    bytes[8]                        = static_cast<std::uint8_t>(3);  // version = 3
    WriteAllBytes(file.path(), bytes);
    EXPECT_THROW(static_cast<void>(WorldSaveReader::Open(file.path())), std::runtime_error);
}

/// 魔数不符 ⇒ 拒绝。
TEST(WorldSave, RejectsBadMagic) {
    const TempPath file("vx_world_save_badmagic.voxr");
    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    writer.WriteToFile(file.path());

    std::vector<std::uint8_t> bytes = ReadAllBytes(file.path());
    bytes[0]                        = 'X';
    WriteAllBytes(file.path(), bytes);
    EXPECT_THROW(static_cast<void>(WorldSaveReader::Open(file.path())), std::runtime_error);
}

/// 头部被改动 ⇒ **校验和**拦下（`worldSeed` 在偏移 32）。
TEST(WorldSave, RejectsCorruptHeaderChecksum) {
    const TempPath file("vx_world_save_badcrc.voxr");
    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    writer.WriteToFile(file.path());

    std::vector<std::uint8_t> bytes = ReadAllBytes(file.path());
    bytes[32]                       = static_cast<std::uint8_t>(bytes[32] ^ 0xFFU);  // 篡改 worldSeed 低字节
    WriteAllBytes(file.path(), bytes);
    EXPECT_THROW(static_cast<void>(WorldSaveReader::Open(file.path())), std::runtime_error);
}

/// 文件缺失 ⇒ 拒绝。
TEST(WorldSave, RejectsMissingFile) {
    const TempPath file("vx_world_save_missing.voxr");
    EXPECT_THROW(static_cast<void>(WorldSaveReader::Open(file.path())), std::runtime_error);
}

/// 空载荷 ⇒ 拒绝（空块没有语义）。
TEST(WorldSave, RejectsEmptyChunkPayload) {
    WorldSaveWriter writer;
    writer.SetHeader(MakeHeader());
    EXPECT_THROW(writer.SetChunk(WorldSaveChunkKey {}, std::vector<std::uint8_t> {}), std::runtime_error);
}

// --------------------------- 载荷编解码：冻结格式的非法输入 ---------------------------

/// 脏列：列号越界 / 非严格升序 / 重复 ⇒ 编码即抛（写盘确定性）；载荷长度不符 ⇒ 解码即抛。
TEST(WorldSave, HeightDirtyCodecRejectsMalformedInput) {
    EXPECT_THROW(static_cast<void>(EncodeHeightDirtyTile({ HeightDirtyEntry { 4096U, 0 } })), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(EncodeHeightDirtyTile({ HeightDirtyEntry { 5U, 0 }, HeightDirtyEntry { 5U, 1 } })),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(EncodeHeightDirtyTile({ HeightDirtyEntry { 9U, 0 }, HeightDirtyEntry { 3U, 1 } })),
                 std::invalid_argument);
    EXPECT_EQ(EncodeHeightDirtyTile({}).size(), 4U);  // 空表合法（0 条）

    const std::vector<std::uint8_t> good = EncodeHeightDirtyTile(MakeEntries());
    std::vector<std::uint8_t>       truncated(good.begin(), good.end() - 1);
    EXPECT_THROW(static_cast<void>(DecodeHeightDirtyTile(truncated)), std::runtime_error);
    std::vector<std::uint8_t> badOrder = EncodeHeightDirtyTile({ HeightDirtyEntry { 1U, 0 }, HeightDirtyEntry { 7U, 0 } });
    badOrder[8]                        = static_cast<std::uint8_t>(0);  // 第二条列号改成 0 ⇒ 不再升序
    badOrder[9]                        = static_cast<std::uint8_t>(0);
    EXPECT_THROW(static_cast<void>(DecodeHeightDirtyTile(badOrder)), std::runtime_error);
}

/// 脏体积块：密度长度必须 32³；带材质时材质长度也必须 32³；`materialPresent` 非 0/1 ⇒ 解码即抛。
TEST(WorldSave, VolumeDirtyCodecRejectsMalformedInput) {
    VolumeDirtyPayload wrongDensity = MakeVolumePayload(false);
    wrongDensity.density.pop_back();
    EXPECT_THROW(static_cast<void>(EncodeVolumeDirtyBlock(wrongDensity)), std::invalid_argument);

    VolumeDirtyPayload wrongMaterial = MakeVolumePayload(true);
    wrongMaterial.material.pop_back();
    EXPECT_THROW(static_cast<void>(EncodeVolumeDirtyBlock(wrongMaterial)), std::invalid_argument);

    std::vector<std::uint8_t> raw = EncodeVolumeDirtyBlock(MakeVolumePayload(false));
    raw[0]                        = static_cast<std::uint8_t>(2);  // materialPresent 非法
    EXPECT_THROW(static_cast<void>(DecodeVolumeDirtyBlock(raw)), std::runtime_error);
}

/// FNV-1a 64：**确定性**、空输入 = 偏移基值、不同输入不同值（世界定义一致性哈希的基础）。
TEST(WorldSave, Fnv1a64IsDeterministicAndSensitive) {
    const char   a[] = "terrain-params";
    const char   b[] = "terrain-paramz";
    const auto   ha  = Fnv1a64(a, sizeof(a) - 1);
    EXPECT_EQ(ha, Fnv1a64(a, sizeof(a) - 1));
    EXPECT_NE(ha, Fnv1a64(b, sizeof(b) - 1));
    EXPECT_EQ(Fnv1a64(nullptr, 0), 1469598103934665603ULL);
}

}  // namespace
