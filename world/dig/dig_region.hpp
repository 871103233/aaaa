#pragma once

#include "dig/volume_mesher.hpp"
#include "terrain/terrain_mesher.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 一个可挖区域标记（ADR 0006 的**数据文件**部分）：块对齐的整数包围盒 + 模式 + 优先级。
///
/// `blockMin` / `blockMax` 是**闭区间**的块索引。文件里给的是世界坐标（格）的闭区间，
/// 装载时按 `kVolumeBlockSize` **向外吸附**到块边界（体积块必须与固定网格对齐，ADR 0004 硬约束 3）——
/// 因此**实际生效的区域是作者所给范围的块对齐超集**，这一点在配置文件头部与 `file-index.md` 都写明。
struct DigRegion {
    std::string name;
    bool        diggable = true;  ///< false = `sealed`（强制不可挖；命中时**覆盖**低优先级的 diggable）
    int         priority = 0;     ///< 大者优先；同优先级时文件顺序在后者优先（与 ADR 0006 一致）
    BlockCoord  blockMin {};      ///< 闭区间下界（块）
    BlockCoord  blockMax {};      ///< 闭区间上界（块）

    /// 该块的**世界范围**（格，半开区间 `[min, max)`）。
    [[nodiscard]] int WorldMinX() const noexcept { return BlockOriginBlocks(blockMin.x); }
    [[nodiscard]] int WorldMinY() const noexcept { return BlockOriginBlocks(blockMin.y); }
    [[nodiscard]] int WorldMinZ() const noexcept { return BlockOriginBlocks(blockMin.z); }
    [[nodiscard]] int WorldMaxX() const noexcept { return BlockOriginBlocks(blockMax.x + 1); }
    [[nodiscard]] int WorldMaxY() const noexcept { return BlockOriginBlocks(blockMax.y + 1); }
    [[nodiscard]] int WorldMaxZ() const noexcept { return BlockOriginBlocks(blockMax.z + 1); }

    [[nodiscard]] bool Contains(double x, double y, double z) const noexcept {
        return x >= static_cast<double>(WorldMinX()) && x < static_cast<double>(WorldMaxX()) &&
               y >= static_cast<double>(WorldMinY()) && y < static_cast<double>(WorldMaxY()) &&
               z >= static_cast<double>(WorldMinZ()) && z < static_cast<double>(WorldMaxZ());
    }
};

/// 可挖区域表：`assets/config/dig_regions.toml` 的装载结果（ADR 0005 的 TOML 口径）。
///
/// 加载约定（与 `MapPreset` / `TerrainMaterialTable` 一致，唯一例外是"文件缺失"）：
///   - **文件缺失** ⇒ 返回**空表**并**不报错**（ADR 0006：只用程序化规则也能起步；
///     本轮程序化规则未实现，故空表 = 没有任何可挖区域）；
///   - **文件存在但解析 / 校验失败** ⇒ 抛异常中止启动（**禁止静默回退**）。
///
/// 本类同时是地表网格化的**层间交接过滤器**（`ITerrainQuadFilter`）：四角全部可挖的地表四边形
/// 交给体积网格渲染（见 ADR 0011）。
class DigRegionTable final : public ITerrainQuadFilter {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    /// 体积块总数上限（防一份配置把内存 / 启动时间拉爆）：按每块 36 KB 计，512 块 ≈ 18 MB。
    /// 超出即报错，不静默截断。
    static constexpr std::size_t kMaxTotalBlocks = 512;

    /// 从 TOML 装载并校验；文件缺失返回空表，非法抛 `std::runtime_error`。
    [[nodiscard]] static DigRegionTable LoadFromFile(const std::filesystem::path& path);

    /// 直接构造（单元测试用；不做文件 IO）。会做与装载相同的校验与块枚举。
    /// `bandDownBlocks` / `bandUpBlocks` 见 `BandDownBlocks()`。
    [[nodiscard]] static DigRegionTable FromRegions(std::vector<DigRegion> regions, int bandDownBlocks = 0,
                                                    int bandUpBlocks = 0);

    /// 空表：没有任何可挖区域。
    [[nodiscard]] static DigRegionTable Default();

    [[nodiscard]] bool Empty() const noexcept { return m_regions.empty(); }

    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

    /// **竖向带宽**（T59 / [ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策一）：
    /// 可挖体积只允许存在于 `[该列地表高度 − BandDownBlocks, 该列地表高度 + BandUpBlocks]` 之内。
    ///
    /// **`0` = 不裁剪**（引入本字段之前的旧口径）⇒ 可向后兼容地省略，故 `schema_version` **不变**。
    /// 真正起作用的是 **`band_down`**：地表以上本来就是空气、没有可挖之物，它决定玩家**向下能挖多深**
    /// —— 这正是"只有地表部分深度可挖"的唯一旋钮。
    [[nodiscard]] int BandDownBlocks() const noexcept { return m_bandDown; }
    [[nodiscard]] int BandUpBlocks() const noexcept { return m_bandUp; }

    /// 全部区域（已按 `(priority 升序, 文件顺序)` 排列）。
    [[nodiscard]] const std::vector<DigRegion>& Regions() const noexcept { return m_regions; }

    /// 全部**可挖**块（去重、升序）——可挖体积世界的块集合就是它。
    [[nodiscard]] const std::vector<BlockCoord>& Blocks() const noexcept { return m_blocks; }

    /// 世界坐标（格）是否属于**可挖**区域：按优先级从高到低取第一个命中的区域；
    /// 命中 `sealed` 即不可挖；**没有任何区域命中 ⇒ 不可挖**（ADR 0004 硬约束 2）。
    [[nodiscard]] bool IsDiggable(double x, double y, double z) const noexcept;

    /// `ITerrainQuadFilter`：**四角全部可挖**时返回 true（该四边形交给体积网格渲染）。
    [[nodiscard]] bool SkipQuad(const TerrainQuad& quad) const override;

private:
    /// 由区域列表重算 `m_blocks`（去重、升序）并校验块数上限；非法抛异常。
    void RebuildBlocks();

    std::vector<DigRegion>  m_regions;  ///< 已按 (priority 升序, 文件顺序) 排列
    std::vector<BlockCoord> m_blocks;   ///< 全部可挖块
    int                     m_schemaVersion = kSchemaVersion;
    int                     m_bandDown = 0;  ///< 地表以下可挖深度（格）；0 = 不裁剪（旧口径）
    int                     m_bandUp   = 0;  ///< 地表以上可挖高度（格）；0 = 不裁剪（旧口径）
};

/// **可挖区域表的内容哈希**（世界定义一致性；[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策六）。
///
/// 口径 = 区域的**语义内容**：逐区域（按 `Regions()` 的 `(priority 升序, 文件顺序)` —— 该顺序本身有意义）编码
/// `name`（**长度前缀**）/ `diggable` / `priority` / 块包围盒 `blockMin` / `blockMax`，外加竖向带宽 `band_down` / `band_up`。
/// **`schema_version` 不在此哈希内**（它是存档头部的独立字段 `digRegionSchemaVersion`）。
/// **纯函数、确定性**：逐字段规范编码（不 hash 结构体裸字节）；同输入必得同值（红线 7）。
[[nodiscard]] std::uint64_t DigRegionContentHash(const DigRegionTable& table) noexcept;

}  // namespace vx
