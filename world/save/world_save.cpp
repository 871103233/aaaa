// 世界状态存档 v2（`.voxr`）容器格式的实现（[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策二，**布局已冻结**）。
//
// 文件布局（全部**小端**，便于确定性复现）：
//   [0, 8)                    魔数 "VXSAVE2\0"
//   [8, 12)                   u32 version（= 2）
//   [12, 16)                  u32 flags（bit0 含高度场脏列 / bit1 含脏体积块）
//   [16, 24)                  u64 generatorVersion（地形生成参数内容哈希）
//   [24, 28) i32 tileRadiusX  [28, 32) i32 tileRadiusZ
//   [32, 40)                  i64 worldSeed
//   [40, 44)                  i32 digRegionSchemaVersion
//   [44, 52)                  u64 digRegionContentHash
//   [52, 56)                  u32 chunkCount
//   [56, 60)                  u32 reserved（写 0）
//   [60, 64)                  u32 headerChecksum = CRC32(前 60 字节)
//   [64, 64+32N)              索引：每项 32 B = u8 kind | u8[3] pad | i32 x | i32 y | i32 z | u64 offset | u32 compressedSize | u32 rawSize
//   [64+32N, ...)             数据段：各块的 zstd 压缩字节（按 key 升序）
//
// zstd 只在本 .cpp 与 premade_map.cpp 内出现（同属 world 模块；公共头不泄漏第三方类型）。

#include "save/world_save.hpp"

#include <zstd.h>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace vx {
namespace {

/// 魔数：8 字节（含结尾 `\0`，与 ADR 0037 的字段表逐字节一致）。
constexpr char kMagic[8] = { 'V', 'X', 'S', 'A', 'V', 'E', '2', '\0' };

constexpr std::size_t kHeaderBytes     = 64;
constexpr std::size_t kIndexEntryBytes = 32;

/// 损坏文件防护：避免在坏档上做巨量分配（超过即判为损坏并抛）。
constexpr std::uint32_t kMaxChunkCount     = 1U << 24;        ///< 16M 块
constexpr std::uint64_t kMaxChunkRawBytes  = 512ULL << 20;    ///< 单块解压后 ≤ 512 MB

/// zstd 压缩级别（固定 ⇒ 输出确定性）。
constexpr int kCompressionLevel = 3;

// ---- 小端读写 ----

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFULL));
    }
}

void AppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void AppendI32(std::vector<std::uint8_t>& out, int value) {
    AppendU32(out, static_cast<std::uint32_t>(value));  // 二进制补码：负数按位写入，读回一致
}

[[nodiscard]] std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8U * static_cast<unsigned>(index));
    }
    return value;
}

[[nodiscard]] std::int32_t ReadI32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int32_t>(ReadU32(bytes, offset));
}

[[nodiscard]] std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

/// CRC32（IEEE 反射多项式 `0xEDB88320`）；本仓库唯一的 CRC 实现（ADR 0037 决策二）。
[[nodiscard]] std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] std::vector<std::uint8_t> Compress(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const std::size_t         written =
        ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), kCompressionLevel);
    if (ZSTD_isError(written) != 0U) {
        throw std::runtime_error(std::string("世界存档：zstd 压缩失败（") + ZSTD_getErrorName(written) + "）");
    }
    compressed.resize(written);
    return compressed;
}

[[nodiscard]] std::vector<std::uint8_t> Decompress(const std::vector<std::uint8_t>& compressed, std::uint32_t rawSize) {
    std::vector<std::uint8_t> raw(rawSize);
    const std::size_t         written =
        ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(written) != 0U) {
        throw std::runtime_error(std::string("世界存档：zstd 解压失败（") + ZSTD_getErrorName(written) + "）");
    }
    if (written != raw.size()) {
        throw std::runtime_error("世界存档：解压长度与索引登记不符（登记 " + std::to_string(rawSize) + "，实际 " +
                                 std::to_string(written) + "）");
    }
    return raw;
}

/// 从字节流按偏移读定长区间（越界即抛）。
void ReadRange(std::ifstream& stream, std::uint64_t offset, std::size_t size, std::vector<std::uint8_t>& out) {
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) {
        throw std::runtime_error("世界存档：seek 失败（文件被截断？）");
    }
    out.resize(size);
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("世界存档：读取越界（文件被截断）");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 载荷编解码
// ---------------------------------------------------------------------------

std::uint64_t Fnv1a64(const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime       = 1099511628211ULL;
    std::uint64_t           hash         = kOffsetBasis;
    const auto*             bytes        = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kPrime;
    }
    return hash;
}

std::vector<std::uint8_t> EncodeHeightDirtyTile(const std::vector<HeightDirtyEntry>& entries) {
    std::vector<std::uint8_t> out;
    out.reserve(4U + entries.size() * 4U);
    AppendU32(out, static_cast<std::uint32_t>(entries.size()));
    std::uint32_t previous = 0;
    bool          first    = true;
    for (const HeightDirtyEntry& entry : entries) {
        if (entry.columnIndex >= kHeightDirtyColumnCount) {
            throw std::invalid_argument("世界存档：脏列号越界（必须 < 4096）");
        }
        if (!first && entry.columnIndex <= previous) {
            throw std::invalid_argument("世界存档：脏列必须按列号严格升序（写盘确定性）");
        }
        first    = false;
        previous = entry.columnIndex;
        AppendU16(out, entry.columnIndex);
        AppendU16(out, static_cast<std::uint16_t>(entry.heightDelta));  // i16 按位写
    }
    return out;
}

std::vector<HeightDirtyEntry> DecodeHeightDirtyTile(const std::vector<std::uint8_t>& raw) {
    if (raw.size() < 4U) {
        throw std::runtime_error("世界存档：脏列载荷过短（缺 entryCount）");
    }
    const std::uint32_t count = ReadU32(raw, 0);
    if (raw.size() != 4U + static_cast<std::size_t>(count) * 4U) {
        throw std::runtime_error("世界存档：脏列载荷长度与 entryCount 不符");
    }
    std::vector<HeightDirtyEntry> entries;
    entries.reserve(count);
    std::uint32_t previous = 0;
    bool          first    = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t      offset = 4U + static_cast<std::size_t>(index) * 4U;
        const std::uint16_t    column = ReadU16(raw, offset);
        const std::uint16_t    delta  = ReadU16(raw, offset + 2U);
        if (column >= kHeightDirtyColumnCount) {
            throw std::runtime_error("世界存档：脏列号越界（载荷损坏）");
        }
        if (!first && column <= previous) {
            throw std::runtime_error("世界存档：脏列未按列号严格升序（载荷损坏）");
        }
        first    = false;
        previous = column;
        entries.push_back(HeightDirtyEntry { column, static_cast<std::int16_t>(delta) });
    }
    return entries;
}

std::vector<std::uint8_t> EncodeVolumeDirtyBlock(const VolumeDirtyPayload& payload) {
    if (payload.density.size() != kVolumeSaveVoxelCount) {
        throw std::invalid_argument("世界存档：体积块密度长度必须 = 32768");
    }
    if (payload.materialPresent && payload.material.size() != kVolumeSaveVoxelCount) {
        throw std::invalid_argument("世界存档：带材质时材质长度必须 = 32768");
    }

    std::vector<std::uint8_t> out;
    out.reserve(4U + payload.density.size() + payload.material.size());
    out.push_back(payload.materialPresent ? 1U : 0U);
    out.push_back(0U);  // pad ×3（ADR 0037 冻结：写 0）
    out.push_back(0U);
    out.push_back(0U);
    for (const std::int8_t value : payload.density) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    if (payload.materialPresent) {
        out.insert(out.end(), payload.material.begin(), payload.material.end());
    }
    return out;
}

VolumeDirtyPayload DecodeVolumeDirtyBlock(const std::vector<std::uint8_t>& raw) {
    if (raw.size() < 4U) {
        throw std::runtime_error("世界存档：体积块载荷过短（缺 materialPresent）");
    }
    const std::uint8_t materialPresent = raw[0];
    if (materialPresent > 1U) {
        throw std::runtime_error("世界存档：materialPresent 必须是 0 或 1");
    }
    const std::size_t expected = 4U + kVolumeSaveVoxelCount + (materialPresent == 1U ? kVolumeSaveVoxelCount : 0U);
    if (raw.size() != expected) {
        throw std::runtime_error("世界存档：体积块载荷长度不符（可能损坏）");
    }

    VolumeDirtyPayload payload;
    payload.materialPresent = (materialPresent == 1U);
    payload.density.resize(kVolumeSaveVoxelCount);
    for (std::size_t index = 0; index < payload.density.size(); ++index) {
        payload.density[index] = static_cast<std::int8_t>(raw[4U + index]);
    }
    if (payload.materialPresent) {
        payload.material.assign(raw.begin() + static_cast<std::ptrdiff_t>(4U + kVolumeSaveVoxelCount), raw.end());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// 写入器
// ---------------------------------------------------------------------------

void WorldSaveWriter::SetChunk(const WorldSaveChunkKey& key, const std::vector<std::uint8_t>& raw) {
    if (raw.empty()) {
        throw std::runtime_error("世界存档：空块没有语义（拒绝写入）");
    }
    m_chunks[key] = raw;
}

void WorldSaveWriter::WriteToFile(const std::filesystem::path& path) const {
    // 1) flags 由**实际块种类**推导（ADR 0037 冻结）。
    std::uint32_t flags = 0;
    for (const auto& entry : m_chunks) {
        if (entry.first.kind == WorldSaveChunkKind::HeightDirtyTile) {
            flags |= kWorldSaveFlagHasHeightDirty;
        } else if (entry.first.kind == WorldSaveChunkKind::VolumeDirtyBlock) {
            flags |= kWorldSaveFlagHasVolumeDirty;
        }
    }

    // 2) 逐块压缩（按 key 升序 ⇒ 确定性）。
    std::vector<std::vector<std::uint8_t>> compressed;
    compressed.reserve(m_chunks.size());
    for (const auto& entry : m_chunks) {
        compressed.push_back(Compress(entry.second));
    }

    // 3) 头部（含 chunkCount 与校验和）。
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(m_chunks.size());
    std::vector<std::uint8_t> header;
    header.reserve(kHeaderBytes);
    header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
    AppendU32(header, kSchemaVersion);
    AppendU32(header, flags);
    AppendU64(header, m_header.generatorVersion);
    AppendI32(header, m_header.tileRadiusX);
    AppendI32(header, m_header.tileRadiusZ);
    AppendU64(header, static_cast<std::uint64_t>(m_header.worldSeed));
    AppendI32(header, m_header.digRegionSchemaVersion);
    AppendU64(header, m_header.digRegionContentHash);
    AppendU32(header, chunkCount);
    AppendU32(header, 0U);  // reserved
    // 校验和覆盖前 60 字节（此时 header 恰为 60 字节）。
    const std::uint32_t checksum = Crc32(header.data(), header.size());
    AppendU32(header, checksum);

    // 4) 索引（offset 从数据段起点递增）。
    std::uint64_t dataOffset = kHeaderBytes + static_cast<std::uint64_t>(chunkCount) * kIndexEntryBytes;
    std::vector<std::uint8_t> index;
    index.reserve(static_cast<std::size_t>(chunkCount) * kIndexEntryBytes);
    std::size_t chunkIndex = 0;
    for (const auto& entry : m_chunks) {
        const std::vector<std::uint8_t>& blob = compressed[chunkIndex++];
        index.push_back(static_cast<std::uint8_t>(entry.first.kind));
        index.push_back(0U);
        index.push_back(0U);
        index.push_back(0U);
        AppendI32(index, entry.first.x);
        AppendI32(index, entry.first.y);
        AppendI32(index, entry.first.z);
        AppendU64(index, dataOffset);
        AppendU32(index, static_cast<std::uint32_t>(blob.size()));
        AppendU32(index, static_cast<std::uint32_t>(entry.second.size()));
        dataOffset += blob.size();
    }

    // 5) 原子替换写盘（临时文件 + rename）。
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("世界存档：无法创建临时文件 " + tempPath.string());
        }
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (!index.empty()) {
            out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
        }
        for (const std::vector<std::uint8_t>& blob : compressed) {
            out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        }
        out.flush();
        if (!out) {
            throw std::runtime_error("世界存档：写入临时文件失败 " + tempPath.string());
        }
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);  // 清理临时文件（尽力而为）
        throw std::runtime_error("世界存档：原子替换失败（" + path.string() + "）：" + renameError.message());
    }
}

// ---------------------------------------------------------------------------
// 读取器
// ---------------------------------------------------------------------------

WorldSaveReader WorldSaveReader::Open(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("世界存档：无法打开文件 " + path.string());
    }

    std::vector<std::uint8_t> header;
    ReadRange(stream, 0U, kHeaderBytes, header);
    for (std::size_t index = 0; index < sizeof(kMagic); ++index) {
        if (header[index] != static_cast<std::uint8_t>(kMagic[index])) {
            throw std::runtime_error("世界存档：魔数不匹配（不是 .voxr v2 文件？）");
        }
    }
    const std::uint32_t version = ReadU32(header, 8U);
    if (version != kSchemaVersion) {
        throw std::runtime_error("世界存档：版本不符（期望 " + std::to_string(kSchemaVersion) + "，实际 " +
                                 std::to_string(version) + "）—— 未知版本拒绝，不静默迁移");
    }
    const std::uint32_t flags = ReadU32(header, 12U);
    const std::uint32_t knownFlags = kWorldSaveFlagHasHeightDirty | kWorldSaveFlagHasVolumeDirty;
    if ((flags & ~knownFlags) != 0U) {
        throw std::runtime_error("世界存档：flags 含未知位（格式不符）");
    }
    const std::uint32_t checksum = ReadU32(header, 60U);
    if (Crc32(header.data(), 60U) != checksum) {
        throw std::runtime_error("世界存档：头部校验和不符（文件损坏）");
    }
    const std::uint32_t chunkCount = ReadU32(header, 52U);
    if (chunkCount > kMaxChunkCount) {
        throw std::runtime_error("世界存档：块数超出上限（文件损坏？）");
    }

    WorldSaveReader reader;
    reader.m_path                  = path;
    reader.m_header.version        = version;
    reader.m_header.flags          = flags;
    reader.m_header.generatorVersion = ReadU64(header, 16U);
    reader.m_header.tileRadiusX    = ReadI32(header, 24U);
    reader.m_header.tileRadiusZ    = ReadI32(header, 28U);
    reader.m_header.worldSeed      = static_cast<std::int64_t>(ReadU64(header, 32U));
    reader.m_header.digRegionSchemaVersion = ReadI32(header, 40U);
    reader.m_header.digRegionContentHash   = ReadU64(header, 44U);

    // 索引。
    std::vector<std::uint8_t> index;
    ReadRange(stream, kHeaderBytes, static_cast<std::size_t>(chunkCount) * kIndexEntryBytes, index);
    const std::uint64_t dataBegin = kHeaderBytes + static_cast<std::uint64_t>(chunkCount) * kIndexEntryBytes;
    stream.clear();
    stream.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(static_cast<std::streamoff>(stream.tellg()));
    if (fileSize < dataBegin) {
        throw std::runtime_error("世界存档：文件短于索引（截断）");
    }

    bool          hasHeight = false;
    bool          hasVolume = false;
    bool          first     = true;
    WorldSaveChunkKey previous {};
    for (std::uint32_t index2 = 0; index2 < chunkCount; ++index2) {
        const std::size_t      base = static_cast<std::size_t>(index2) * kIndexEntryBytes;
        const std::uint8_t     kind = index[base];
        const std::uint64_t    offset         = ReadU64(index, base + 16U);
        const std::uint32_t    compressedSize = ReadU32(index, base + 24U);
        const std::uint32_t    rawSize        = ReadU32(index, base + 28U);

        WorldSaveChunkKey key;
        if (kind == static_cast<std::uint8_t>(WorldSaveChunkKind::HeightDirtyTile)) {
            key.kind  = WorldSaveChunkKind::HeightDirtyTile;
            hasHeight = true;
        } else if (kind == static_cast<std::uint8_t>(WorldSaveChunkKind::VolumeDirtyBlock)) {
            key.kind  = WorldSaveChunkKind::VolumeDirtyBlock;
            hasVolume = true;
        } else {
            throw std::runtime_error("世界存档：索引含未知块类型");
        }
        key.x = ReadI32(index, base + 4U);
        key.y = ReadI32(index, base + 8U);
        key.z = ReadI32(index, base + 12U);

        if (compressedSize == 0U) {
            throw std::runtime_error("世界存档：索引登记了空块");
        }
        if (static_cast<std::uint64_t>(rawSize) > kMaxChunkRawBytes) {
            throw std::runtime_error("世界存档：单块解压后长度超出上限（文件损坏？）");
        }
        if (offset < dataBegin || offset + compressedSize > fileSize) {
            throw std::runtime_error("世界存档：索引偏移越界（文件损坏）");
        }
        if (!first && !(previous < key)) {
            throw std::runtime_error("世界存档：索引未按 key 严格升序（格式不符）");
        }
        first    = false;
        previous = key;

        reader.m_index.emplace(key, Entry { offset, compressedSize, rawSize });
    }

    // flags 必须与索引实际内容一致（可判定的不变量）。
    if ((hasHeight ? 1U : 0U) != ((flags & kWorldSaveFlagHasHeightDirty) != 0U ? 1U : 0U) ||
        (hasVolume ? 1U : 0U) != ((flags & kWorldSaveFlagHasVolumeDirty) != 0U ? 1U : 0U)) {
        throw std::runtime_error("世界存档：flags 与索引内容不一致（格式不符）");
    }

    return reader;
}

std::vector<WorldSaveChunkKey> WorldSaveReader::Keys() const {
    std::vector<WorldSaveChunkKey> keys;
    keys.reserve(m_index.size());
    for (const auto& entry : m_index) {
        keys.push_back(entry.first);  // `std::map` ⇒ 已按 `operator<` 升序（与写盘顺序一致）
    }
    return keys;
}

std::vector<std::uint8_t> WorldSaveReader::ReadChunk(const WorldSaveChunkKey& key) const {
    const auto found = m_index.find(key);
    if (found == m_index.end()) {
        throw std::runtime_error("世界存档：请求的块不存在");
    }
    std::ifstream stream(m_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("世界存档：无法打开文件 " + m_path.string());
    }
    std::vector<std::uint8_t> compressed;
    ReadRange(stream, found->second.offset, found->second.compressedSize, compressed);
    return Decompress(compressed, found->second.rawSize);
}

}  // namespace vx
