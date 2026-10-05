// 预制地图容器格式的实现（[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
//
// 文件布局（全部**小端**，便于确定性复现）：
//   [0, 8)            魔数 "VXPREMAP"
//   [8, 12)           u32 schema_version
//   [12, 16)          i32 tile_radius_x
//   [16, 20)          i32 tile_radius_z
//   [20, 28)          u64 seed
//   [28, 32)          u32 chunk_count
//   [32, 32+37N)      索引：每项 37 字节 = u8 kind | i32 x | i32 y | i32 z | u64 offset | u64 compressed_size | u64 raw_size
//   [32+37N, ...)     数据段：各块的 zstd 压缩字节（按 key 升序）
//
// zstd 只在本 .cpp 内出现（与 texture_loader 隔离 stb_image 同口径：公共头不泄漏第三方类型）。

#include "premade/premade_map.hpp"

#include <zstd.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace vx {
namespace {

/// 魔数：8 字节，无终止符。
constexpr char kMagic[8] = { 'V', 'X', 'P', 'R', 'E', 'M', 'A', 'P' };

constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kIndexEntryBytes = 37;

/// 索引 / 块大小上界：用于在**损坏文件**上避免巨量分配（超过即判为损坏并抛）。
constexpr std::uint32_t kMaxChunkCount = 1U << 24;        ///< 16M 块
constexpr std::uint64_t kMaxChunkRawBytes = 256ULL << 20;  ///< 单块解压后 ≤ 256 MB

/// zstd 压缩级别（固定 ⇒ 输出确定性）。
constexpr int kCompressionLevel = 3;

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void AppendI32(std::vector<std::uint8_t>& out, int value) {
    AppendU32(out, static_cast<std::uint32_t>(value));  // 二进制补码：负数按位写入，读回一致
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFULL));
    }
}

[[nodiscard]] std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] int ReadI32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<int>(ReadU32(bytes, offset));
}

[[nodiscard]] std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(i)])
                 << static_cast<unsigned>(i * 8);
    }
    return value;
}

/// 读一个块需要的全部字节（用于索引项的 field-by-field 读取）。
void ReadExact(std::istream& stream, std::vector<std::uint8_t>& out, std::size_t count) {
    out.resize(count);
    if (count == 0) {
        return;
    }
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
    if (!stream) {
        throw std::runtime_error("预制地图：读取时文件被截断");
    }
}

[[nodiscard]] std::string Describe(const std::filesystem::path& path) {
    return "预制地图 " + path.string();
}

}  // namespace

void PremadeMapWriter::SetWorldInfo(int tileRadiusX, int tileRadiusZ, std::uint64_t seed) noexcept {
    m_tileRadiusX = tileRadiusX;
    m_tileRadiusZ = tileRadiusZ;
    m_seed        = seed;
}

void PremadeMapWriter::SetChunk(const PremadeChunkKey& key, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        throw std::runtime_error("预制地图写入：块为空（kind=" +
                                 std::to_string(static_cast<unsigned>(key.kind)) + "，坐标 (" + std::to_string(key.x) +
                                 ", " + std::to_string(key.y) + ", " + std::to_string(key.z) + ")）");
    }
    m_chunks[key] = bytes;
}

void PremadeMapWriter::WriteToFile(const std::filesystem::path& path) const {
    // 1) 逐块压缩（顺序由 std::map 的 key 升序保证 ⇒ 确定性）。
    std::vector<std::vector<std::uint8_t>> compressed;
    compressed.reserve(m_chunks.size());
    for (const auto& [key, raw] : m_chunks) {
        (void)key;
        const std::size_t bound = ZSTD_compressBound(raw.size());
        std::vector<std::uint8_t> buffer(bound);
        const std::size_t written = ZSTD_compress(buffer.data(), bound, raw.data(), raw.size(), kCompressionLevel);
        if (ZSTD_isError(written)) {
            throw std::runtime_error("预制地图写入：zstd 压缩失败（" +
                                     std::string(ZSTD_getErrorName(written)) + "）");
        }
        buffer.resize(written);
        compressed.push_back(std::move(buffer));
    }

    // 2) 计算数据段偏移（头 + 定长索引之后）。
    const std::uint64_t dataStart =
        static_cast<std::uint64_t>(kHeaderBytes) + static_cast<std::uint64_t>(kIndexEntryBytes) * m_chunks.size();

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error(Describe(path) + "：无法创建输出文件");
    }

    // 3) 头。
    std::vector<std::uint8_t> header;
    header.reserve(kHeaderBytes);
    header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
    AppendU32(header, kSchemaVersion);
    AppendI32(header, m_tileRadiusX);
    AppendI32(header, m_tileRadiusZ);
    AppendU64(header, m_seed);
    AppendU32(header, static_cast<std::uint32_t>(m_chunks.size()));
    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    // 4) 索引 + 数据（同一遍按 key 升序，偏移累加）。
    std::uint64_t offset = dataStart;
    std::size_t   index  = 0;
    for (const auto& [key, raw] : m_chunks) {
        std::vector<std::uint8_t> entry;
        entry.reserve(kIndexEntryBytes);
        entry.push_back(static_cast<std::uint8_t>(key.kind));
        AppendI32(entry, key.x);
        AppendI32(entry, key.y);
        AppendI32(entry, key.z);
        AppendU64(entry, offset);
        AppendU64(entry, compressed[index].size());
        AppendU64(entry, raw.size());
        stream.write(reinterpret_cast<const char*>(entry.data()), static_cast<std::streamsize>(entry.size()));

        offset += compressed[index].size();
        ++index;
    }
    for (const auto& buffer : compressed) {
        stream.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }

    stream.flush();
    if (!stream) {
        throw std::runtime_error(Describe(path) + "：写入失败（磁盘 / 权限？）");
    }
}

PremadeMapReader PremadeMapReader::Open(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(Describe(path) + "：文件不存在或无法打开");
    }

    PremadeMapReader reader;
    reader.m_path = path;

    // 头。
    std::vector<std::uint8_t> header;
    ReadExact(stream, header, kHeaderBytes);
    if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error(Describe(path) + "：魔数错误（不是预制地图文件）");
    }
    reader.m_schemaVersion = ReadU32(header, 8);
    if (reader.m_schemaVersion != kSchemaVersion) {
        throw std::runtime_error(Describe(path) + "：schema_version 不匹配（期望 " +
                                 std::to_string(kSchemaVersion) + "，实际 " +
                                 std::to_string(reader.m_schemaVersion) + "）");
    }
    reader.m_tileRadiusX = ReadI32(header, 12);
    reader.m_tileRadiusZ = ReadI32(header, 16);
    reader.m_seed        = ReadU64(header, 20);
    const std::uint32_t chunkCount = ReadU32(header, 28);
    if (chunkCount > kMaxChunkCount) {
        throw std::runtime_error(Describe(path) + "：块数非法（" + std::to_string(chunkCount) + "）");
    }

    // 文件总大小（用于校验索引项的范围）。
    stream.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(stream.tellg());
    stream.seekg(static_cast<std::streamoff>(kHeaderBytes), std::ios::beg);

    // 索引。
    std::vector<std::uint8_t> entry;
    for (std::uint32_t i = 0; i < chunkCount; ++i) {
        ReadExact(stream, entry, kIndexEntryBytes);
        PremadeChunkKey key;
        key.kind = static_cast<PremadeChunkKind>(entry[0]);
        key.x    = ReadI32(entry, 1);
        key.y    = ReadI32(entry, 5);
        key.z    = ReadI32(entry, 9);
        Entry record;
        record.offset         = ReadU64(entry, 13);
        record.compressedSize = ReadU64(entry, 21);
        record.rawSize        = ReadU64(entry, 29);
        if (record.rawSize > kMaxChunkRawBytes || record.compressedSize > kMaxChunkRawBytes ||
            record.offset < kHeaderBytes || record.offset + record.compressedSize > fileSize) {
            throw std::runtime_error(Describe(path) + "：索引项损坏（第 " + std::to_string(i) + " 项）");
        }
        if (reader.m_index.find(key) != reader.m_index.end()) {
            throw std::runtime_error(Describe(path) + "：索引存在重复块键（第 " + std::to_string(i) + " 项）");
        }
        reader.m_index.emplace(key, record);
    }
    return reader;
}

std::vector<std::uint8_t> PremadeMapReader::ReadChunk(const PremadeChunkKey& key) const {
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        throw std::runtime_error(Describe(m_path) + "：请求的块不存在（kind=" +
                                 std::to_string(static_cast<unsigned>(key.kind)) + "，坐标 (" +
                                 std::to_string(key.x) + ", " + std::to_string(key.y) + ", " +
                                 std::to_string(key.z) + ")）");
    }

    std::ifstream stream(m_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(Describe(m_path) + "：无法打开以读取块");
    }
    stream.seekg(static_cast<std::streamoff>(it->second.offset), std::ios::beg);

    std::vector<std::uint8_t> compressed;
    ReadExact(stream, compressed, static_cast<std::size_t>(it->second.compressedSize));

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(it->second.rawSize));
    const std::size_t decompressed =
        ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(decompressed)) {
        throw std::runtime_error(Describe(m_path) + "：zstd 解压失败（" +
                                 std::string(ZSTD_getErrorName(decompressed)) + "）");
    }
    if (decompressed != raw.size()) {
        throw std::runtime_error(Describe(m_path) + "：解压大小与记录不符（记录 " + std::to_string(raw.size()) +
                                 "，实际 " + std::to_string(decompressed) + "）");
    }
    return raw;
}

}  // namespace vx
