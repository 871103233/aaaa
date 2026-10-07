#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace vx {

/// `.voxr` v2 的**块类型**（[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策二，**已冻结**）。
///
/// 新增块类型 = **改冻结格式** ⇒ 必须提升 `WorldSaveWriter::kSchemaVersion` 并配套迁移函数 + 迁移测试（红线 8）。
enum class WorldSaveChunkKind : std::uint8_t {
    HeightDirtyTile  = 1,  ///< 高度场脏列（键含义：`(tileX, 0, tileZ)`）
    VolumeDirtyBlock = 2,  ///< 脏体积块（键含义：体积块坐标）
};

/// 一个存档块的键（类型 + 三维块坐标）。
///
/// 坐标**含义随类型解释**（`HeightDirtyTile` 的 `y` 恒为 0）。
struct WorldSaveChunkKey {
    WorldSaveChunkKind kind = WorldSaveChunkKind::HeightDirtyTile;
    int                x    = 0;
    int                y    = 0;
    int                z    = 0;

    [[nodiscard]] friend constexpr bool operator==(const WorldSaveChunkKey& lhs,
                                                   const WorldSaveChunkKey& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
    /// 升序（类型 → x → y → z）：**写盘顺序**由它钉死 ⇒ 同输入两次写出的文件逐字节相同（红线 7）。
    [[nodiscard]] friend constexpr bool operator<(const WorldSaveChunkKey& lhs,
                                                  const WorldSaveChunkKey& rhs) noexcept {
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

/// 头部 `flags` 位（ADR 0037 冻结）：由**实际写入的块种类**推导，读取时**必须**与索引一致。
inline constexpr std::uint32_t kWorldSaveFlagHasHeightDirty = 1U << 0U;
inline constexpr std::uint32_t kWorldSaveFlagHasVolumeDirty = 1U << 1U;

/// 头部里的**世界定义一致性**字段（ADR 0037 决策六）。
///
/// 读档时与当前世界定义比对：**不一致 ⇒ 提示**（"未改动部分重建后可能与原档不同"），
/// **禁止**静默迁移（口径同 [ADR 0006](../../docs/adr/0006-diggable-region-marking.md) / `references/save-and-serialization.md` §5）。
struct WorldSaveHeader {
    std::uint32_t version             = 0;  ///< 存档布局版本（写入时由实现置 `kSchemaVersion`）
    std::uint32_t flags               = 0;  ///< 见 `kWorldSaveFlag*`（写入时按实际块种类推导）
    std::uint64_t generatorVersion    = 0;  ///< **地形生成参数的内容哈希**（`Fnv1a64`；与 `version` **分离**）
    int           tileRadiusX         = 0;  ///< 世界 tile 半径（两轴）
    int           tileRadiusZ         = 0;
    std::int64_t  worldSeed           = 0;  ///< 世界种子（十进制无损）
    std::int32_t  digRegionSchemaVersion = 0;  ///< 可挖区域表 `schema_version`
    std::uint64_t digRegionContentHash   = 0;  ///< 可挖区域表**内容哈希**
};

/// `.voxr` v2 **写入器**：先 `SetHeader` 记录世界定义字段，再逐块 `SetChunk`（原始未压缩载荷），最后 `WriteToFile`。
///
/// **确定性**：块按 `WorldSaveChunkKey` 升序写盘 + 逐块 zstd（固定级别）⇒ 同一输入两次写出的文件**逐字节相同**。
/// **写入安全**：`WriteToFile` 走"临时文件 + `rename`"**原子替换**（口径同 [ADR 0030](../../docs/adr/0030-instance-save-slot.md)）。
class WorldSaveWriter {
public:
    /// 文件布局版本；写入文件头的 `version` 恒等于它，读取器要求相等（不等即拒，**不静默迁移**）。
    static constexpr std::uint32_t kSchemaVersion = 2;

    /// 记录世界定义字段（`version` / `flags` 由实现覆盖，调用方无需关心）。
    void SetHeader(const WorldSaveHeader& header) noexcept { m_header = header; }

    /// 添加 / 覆盖一个块。`raw` 为该块的**原始未压缩**载荷（格式由 `key.kind` 约定，见 ADR 0037）。
    /// 前置条件：`!raw.empty()`；空载荷抛 `std::runtime_error`（空块没有语义）。
    void SetChunk(const WorldSaveChunkKey& key, const std::vector<std::uint8_t>& raw);

    /// 写盘（原子替换）。失败（无法创建临时文件 / 写失败 / 压缩失败 / 替换失败）抛 `std::runtime_error`。
    void WriteToFile(const std::filesystem::path& path) const;

    [[nodiscard]] std::size_t ChunkCount() const noexcept { return m_chunks.size(); }

private:
    WorldSaveHeader                                      m_header;
    std::map<WorldSaveChunkKey, std::vector<std::uint8_t>> m_chunks;  ///< 已按 key 升序
};

/// `.voxr` v2 **读取器**：`Open` 时校验魔数 / 版本 / 头部校验和 / 索引区间与 `flags` 一致性，索引常驻内存，按块随机读。
///
/// 语义（与其它加载器同口径，ADR 0037 判据④）：文件缺失 / 魔数错 / 版本不符 / 校验和不符 / 索引越界 / 解压失败
/// **一律抛异常**，**不静默回退**。
class WorldSaveReader {
public:
    static constexpr std::uint32_t kSchemaVersion = 2;

    /// 打开并校验。失败抛 `std::runtime_error`。
    [[nodiscard]] static WorldSaveReader Open(const std::filesystem::path& path);

    [[nodiscard]] const WorldSaveHeader& Header() const noexcept { return m_header; }
    [[nodiscard]] std::size_t            ChunkCount() const noexcept { return m_index.size(); }
    [[nodiscard]] bool                   HasChunk(const WorldSaveChunkKey& key) const noexcept {
        return m_index.find(key) != m_index.end();
    }

    /// 索引里的**全部块键**（按 `operator<` **升序** ⇒ 与写盘顺序一致）。
    ///
    /// 用途（S4）：读档时把整档搬到内存会话（"有哪些块"必须先知道，见 `WorldStateSave::LoadFromFile`）。
    [[nodiscard]] std::vector<WorldSaveChunkKey> Keys() const;

    /// 读取并**解压**一个块，返回其原始载荷。块不存在 / 读取失败 / 解压失败（含长度不符）⇒ 抛。
    [[nodiscard]] std::vector<std::uint8_t> ReadChunk(const WorldSaveChunkKey& key) const;

private:
    struct Entry {
        std::uint64_t offset         = 0;
        std::uint32_t compressedSize = 0;
        std::uint32_t rawSize        = 0;
    };

    std::filesystem::path            m_path;
    WorldSaveHeader                  m_header;
    std::map<WorldSaveChunkKey, Entry> m_index;
};

// ---------------------------------------------------------------------------
// 载荷编解码（ADR 0037 冻结格式；**纯函数、紧凑小端、无填充**）
// ---------------------------------------------------------------------------

/// 一个高度场脏列的 tile 尺寸（列数 = 64 × 64 = 4096，与 [ADR 0008](../../docs/adr/0008-sizes-precision-budget.md) 一致）。
inline constexpr std::uint32_t kHeightDirtyColumnCount = 64U * 64U;

/// 一条脏列：列号（行主序 0..4095）+ **相对生成结果的高度差**（1/16 格单位）。
struct HeightDirtyEntry {
    std::uint16_t columnIndex = 0;
    std::int16_t  heightDelta = 0;
};

/// 编码 `HeightDirtyTile` 载荷。**前置条件**：`entries` 按 `columnIndex` **严格升序**且都 `< 4096`
/// （升序 = 写盘确定性；违反 ⇒ `std::invalid_argument`）。
[[nodiscard]] std::vector<std::uint8_t> EncodeHeightDirtyTile(const std::vector<HeightDirtyEntry>& entries);

/// 解码 `HeightDirtyTile` 载荷（格式 / 越界 / 非升序 ⇒ `std::runtime_error`）。
[[nodiscard]] std::vector<HeightDirtyEntry> DecodeHeightDirtyTile(const std::vector<std::uint8_t>& raw);

/// 一个体积块的体素数（32³，与 [ADR 0008](../../docs/adr/0008-sizes-precision-budget.md) 一致）。
inline constexpr std::uint32_t kVolumeSaveVoxelCount = 32U * 32U * 32U;

/// 脏体积块载荷：密度（必填）+ **懒分配**材质（`materialPresent == false` 时材质按列派生）。
struct VolumeDirtyPayload {
    bool                     materialPresent = false;
    std::vector<std::int8_t> density;   ///< 长度必须 = `kVolumeSaveVoxelCount`
    std::vector<std::uint8_t> material; ///< `materialPresent == true` 时长度必须 = `kVolumeSaveVoxelCount`
};

/// 编码 `VolumeDirtyBlock` 载荷（尺寸不符 ⇒ `std::invalid_argument`）。
[[nodiscard]] std::vector<std::uint8_t> EncodeVolumeDirtyBlock(const VolumeDirtyPayload& payload);

/// 解码 `VolumeDirtyBlock` 载荷（长度 / `materialPresent` 非 0·1 ⇒ `std::runtime_error`）。
[[nodiscard]] VolumeDirtyPayload DecodeVolumeDirtyBlock(const std::vector<std::uint8_t>& raw);

/// FNV-1a 64（世界定义一致性哈希用；**纯函数、确定性**）。
/// `size == 0` 时返回偏移基值（`data` 可为 `nullptr`）。
[[nodiscard]] std::uint64_t Fnv1a64(const void* data, std::size_t size) noexcept;

/// FNV-1a 64 的参数（公开常量，供 `Fnv1a64` 与 `Fnv1a64Builder` 共用）。
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnv1a64Prime       = 1099511628211ULL;

/// `Fnv1a64` 的**增量版**（世界定义内容哈希用）：逐字段喂入，最后取 `Value()`。
///
/// 为什么需要它（而不是把结构体裸字节喂给 `Fnv1a64`）：C++ 结构体有**填充字节**、其内容不确定
/// ⇒ 裸 hash **不稳定**（同输入可能得不同值）。本类只编码**显式字段**，每字段按**固定宽度小端**写入
/// ⇒ 同输入必得同值（红线 7）。**纯函数式用法**：构造 → 若干 `Feed*` → `Value()`。
class Fnv1a64Builder {
public:
    void Bytes(const void* data, std::size_t size) noexcept;  ///< 原样追加（字符串等；`size == 0` 合法）
    void U8(std::uint8_t value) noexcept;
    void U32(std::uint32_t value) noexcept;
    void U64(std::uint64_t value) noexcept;
    void I32(std::int32_t value) noexcept;  ///< 二进制补码 ⇒ 与 `static_cast<u32>` 逐位等价
    void F32(float value) noexcept;         ///< 以 IEEE-754 位模式编码（同值 ⇒ 同位模式）
    void Bool(bool value) noexcept { U8(value ? 1U : 0U); }

    [[nodiscard]] std::uint64_t Value() const noexcept { return m_value; }

private:
    std::uint64_t m_value = kFnv1a64OffsetBasis;
};

}  // namespace vx
