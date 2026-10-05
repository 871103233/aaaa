#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace vx {

/// 预制地图的块类型（[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
///
/// 当前只落**宏地形高度场 tile**；体积壳网格 / 水体在 W4~W6 扩展（新增枚举值 + 升级 `schema_version`）。
enum class PremadeChunkKind : std::uint8_t {
    MacroHeightTile = 1,  ///< 宏地形高度场 tile：`kTerrainTileSize²` 个 `int16`（1/16 格，行主序）
};

/// 一个预制块的键：类型 + 三维块坐标。
///
/// 坐标**含义随类型解释**：`MacroHeightTile` 用 `(tileX, 0, tileZ)`（y 恒为 0）。
struct PremadeChunkKey {
    PremadeChunkKind kind = PremadeChunkKind::MacroHeightTile;
    int              x    = 0;
    int              y    = 0;
    int              z    = 0;

    [[nodiscard]] friend constexpr bool operator==(const PremadeChunkKey& lhs,
                                                   const PremadeChunkKey& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
    /// 升序（类型 → x → y → z）：**写盘的确定性顺序**由它钉死（红线 7）。
    [[nodiscard]] friend constexpr bool operator<(const PremadeChunkKey& lhs,
                                                  const PremadeChunkKey& rhs) noexcept {
        if (lhs.kind != rhs.kind) {
            return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
        }
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        if (lhs.y != rhs.y) {
            return lhs.y < rhs.y;
        }
        return lhs.z < rhs.z;
    }
};

/// 预制地图**写入器**（离线烘焙工具与单测用）。
///
/// 语义：
///   - 先 `SetWorldInfo` 记录世界范围与种子，再逐块 `SetChunk`，最后 `WriteToFile`；
///   - **确定性**：块按 `PremadeChunkKey` **升序**写盘，同一输入两次写出的文件**逐字节相同**（红线 7）；
///   - 每个块的原始字节在写盘时**逐块 zstd 压缩**（[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）；
///   - 非法输入（空块）**即抛**，不产出半成品（ADR 0005 口径）。
class PremadeMapWriter {
public:
    /// 文件格式版本；写入文件头的 `schema_version` 与读取器的期望值必须相等。
    static constexpr std::uint32_t kSchemaVersion = 1;

    /// 记录世界范围（tile 半径，两轴各自）与世界种子（仅记录，供上层核对）。
    void SetWorldInfo(int tileRadiusX, int tileRadiusZ, std::uint64_t seed) noexcept;

    /// 添加 / 覆盖一个块。`bytes` 为该块的**原始未压缩**字节（含义由 `key.kind` 约定）。
    /// 前置条件：`!bytes.empty()`；空块抛 `std::runtime_error`。
    void SetChunk(const PremadeChunkKey& key, const std::vector<std::uint8_t>& bytes);

    /// 写盘。**确定性**：块按 key 升序 + 逐块 zstd；同一输入 ⇒ 文件逐字节相同。
    /// 失败（无法创建文件 / 写失败 / 压缩失败）抛 `std::runtime_error`。
    void WriteToFile(const std::filesystem::path& path) const;

    [[nodiscard]] std::size_t ChunkCount() const noexcept { return m_chunks.size(); }

private:
    int                  m_tileRadiusX = 0;
    int                  m_tileRadiusZ = 0;
    std::uint64_t        m_seed        = 0;
    std::map<PremadeChunkKey, std::vector<std::uint8_t>> m_chunks;  ///< 已按 key 升序
};

/// 预制地图**读取器**：打开时校验魔数与 `schema_version`，索引常驻内存，按块**随机访问**。
///
/// 语义（与其它加载器同口径，[ADR 0005](../../docs/adr/0005-config-parsing.md) / ADR 0026）：
///   - 文件缺失 / 魔数错 / `schema_version` 不符 / 索引越界 ⇒ `Open` **抛异常**，不静默回退；
///   - `ReadChunk` 只读该块附近的数据（按索引偏移 seek），**不读整份文件**；块不存在 ⇒ 抛。
class PremadeMapReader {
public:
    static constexpr std::uint32_t kSchemaVersion = 1;

    /// 打开并校验。失败抛 `std::runtime_error`（文件缺失 / 魔数错 / 版本不符 / 结构损坏）。
    [[nodiscard]] static PremadeMapReader Open(const std::filesystem::path& path);

    [[nodiscard]] std::uint32_t SchemaVersion() const noexcept { return m_schemaVersion; }
    [[nodiscard]] int           TileRadiusX() const noexcept { return m_tileRadiusX; }
    [[nodiscard]] int           TileRadiusZ() const noexcept { return m_tileRadiusZ; }
    [[nodiscard]] std::uint64_t Seed() const noexcept { return m_seed; }
    [[nodiscard]] std::size_t   ChunkCount() const noexcept { return m_index.size(); }
    [[nodiscard]] bool          HasChunk(const PremadeChunkKey& key) const noexcept {
        return m_index.find(key) != m_index.end();
    }

    /// 读取并**解压**一个块，返回其原始字节。块不存在 / 读取失败 / 解压失败 ⇒ 抛。
    [[nodiscard]] std::vector<std::uint8_t> ReadChunk(const PremadeChunkKey& key) const;

private:
    struct Entry {
        std::uint64_t offset         = 0;  ///< 压缩数据在文件中的偏移
        std::uint64_t compressedSize = 0;
        std::uint64_t rawSize        = 0;  ///< 解压后的原始大小（用于分配与校验）
    };

    std::filesystem::path                        m_path;
    std::uint32_t                                m_schemaVersion = 0;
    int                                          m_tileRadiusX   = 0;
    int                                          m_tileRadiusZ   = 0;
    std::uint64_t                                m_seed          = 0;
    std::map<PremadeChunkKey, Entry>             m_index;
};

}  // namespace vx
