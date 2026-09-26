#pragma once

#include "dig/dig_region.hpp"
#include "dig/volume_mesher.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace vx {

class TerrainWorld;

/// 体积块的**填充分类**（T30）：整块全实心 / 全空时，它不可能参与塌落的支撑判定，
/// 可被整块排除出邻域（见 `ApplyCollapse` 的竖直范围推导）。缓存它把"塌落邻域"从
/// 整个体积的高度收到真正可能失支撑的那几块上。
enum class BlockFill : std::uint8_t {
    Solid,  ///< 含共享边界的全部采样都实心（内部不可能有空腔）
    Air,    ///< 含共享边界的全部采样都空（内部不可能有实心）
    Mixed,  ///< 两者都有
};

/// 一个体积块的密度数据与网格（ADR 0008：33³ 采样、`int8` 密度，单块约 36 KB）。
struct VolumeBlock {
    BlockCoord               coord {};
    std::vector<std::int8_t> density;  ///< `33³`，索引 `i + N * (j + N * k)`（`N = kVolumeSampleCount`）
    MeshData                 mesh;     ///< 块内局部坐标（格）；世界定位由 `coord` 承担（红线 6）
    bool                     carved = false;  ///< 是否被挖过（诊断 / 面板）
    BlockFill                fill   = BlockFill::Mixed;  ///< 与 `density` 同步的分类（网格化 / 写回时更新）
};

/// 一个**世界空间体素矩形区域**的密度视图（T29 塌落等体素级规则用）。
///
/// 坐标是**世界整数坐标**（采样点坐标，1 格间距），不是块坐标。`values` 的索引为
/// `x + sizeX * (y + sizeY * z)`（各 `x/y/z` 为相对 `min*` 的偏移）。
struct DensityRegion {
    int                      minX  = 0;
    int                      minY  = 0;
    int                      minZ  = 0;
    int                      sizeX = 0;
    int                      sizeY = 0;
    int                      sizeZ = 0;
    std::vector<std::int8_t> values;

    [[nodiscard]] std::size_t Index(int x, int y, int z) const noexcept {
        return static_cast<std::size_t>(x) + static_cast<std::size_t>(sizeX) *
                                                 (static_cast<std::size_t>(y) + static_cast<std::size_t>(sizeY) *
                                                                                   static_cast<std::size_t>(z));
    }
    [[nodiscard]] bool Contains(int x, int y, int z) const noexcept {
        return x >= 0 && x < sizeX && y >= 0 && y < sizeY && z >= 0 && z < sizeZ;
    }
};

/// 世界整数坐标（格）的闭区间 AABB。`max < min` 表示**空**。
struct VoxelBounds {
    int minX = 0;
    int minY = 0;
    int minZ = 0;
    int maxX = -1;
    int maxY = -1;
    int maxZ = -1;

    [[nodiscard]] bool Empty() const noexcept { return maxX < minX || maxY < minY || maxZ < minZ; }
};

/// 可挖体积世界（ADR 0004 层 ②）：**只在被标记区域内存在**的有界 SDF 体积。
///
/// 职责与口径：
///   - 块集合 = `DigRegionTable::Blocks()`（**固定网格对齐**，块间共享边界采样 ⇒ 无接缝）；
///   - 初始密度**由地表高度场推导**：`d = clamp((y − 地表高度) × 127, ±127)` ⇒ 地下为负（实心）、
///     空中为正（空）、地表处跨零。因此**未挖状态下**体积表面与地表网格重合（量级为 SN 的平滑误差）；
///   - **区域外**的采样回退到同一公式 ⇒ 体积在区域边界处与地表**连续**（不会出现凭空封口的墙）；
///   - 挖除 = CSG 取 `max` 的**球体减去**（球内变空）；球面本身就是光滑面，故无台阶感。
///     `int8` 的 ±1 格饱和不影响挖除精度（挖除区域附近的密度都落在 ±1 格内）。
///
/// **未做（见 `docs/plans/v0.1.md` 的 T8 降级项）**：体积的物理碰撞、流式加载、存档。
///
/// 线程约定：只在逻辑线程（主线程）使用；与地表世界同属"单写者"。
class DigVolumeWorld {
public:
    /// 前置条件：`terrain` 与 `regions` 的生命周期覆盖本对象。
    DigVolumeWorld(const TerrainWorld& terrain, const DigRegionTable& regions);

    DigVolumeWorld(const DigVolumeWorld&) = delete;
    DigVolumeWorld& operator=(const DigVolumeWorld&) = delete;
    DigVolumeWorld(DigVolumeWorld&&) = delete;
    DigVolumeWorld& operator=(DigVolumeWorld&&) = delete;

    /// 由地表高度场生成全部块的初始密度，并网格化一次。可在世界加载完成后调用一次。
    void InitFromHeightField();

    [[nodiscard]] bool Empty() const noexcept { return m_blocks.empty(); }

    /// 世界坐标（格）是否落在可挖区域内（= `DigRegionTable::IsDiggable`）。
    [[nodiscard]] bool IsInsideRegion(double x, double y, double z) const noexcept;

    /// 采样密度（**密度单位**，±127）：区域内读块数据（含已挖改动），区域外回退到高度场推导值。
    [[nodiscard]] float SampleDensity(double x, double y, double z) const noexcept;

    /// 采样**材质槽位**（ADR 0014）；`kNoMaterialSlot` = 未指定。
    ///
    /// 材质**不随块存储**：它只在初始化时按**列**从地表派生，挖除不删、塌落不搬 ⇒
    /// 任意高度的材质恒等于该列地表派生的材质（纯函数）。故这里直接委托地形查询；
    /// `worldY` 保留在签名里，供将来"按深度分层"或"可编辑体素材质"使用（那时才需要存储）。
    [[nodiscard]] std::uint8_t SampleMaterialSlot(int worldX, int worldY, int worldZ) const noexcept;

    /// 该点是否为实心（密度 < 0）。光球命中检测用它。
    [[nodiscard]] bool IsSolid(double x, double y, double z) const noexcept {
        return SampleDensity(x, y, z) < 0.0F;
    }

    /// 球体挖除：`center` 为世界坐标（格）、`radiusBlocks` 为半径（格）。
    /// 返回是否有实际改动；被改动的块追加到 `dirtyOut`（可能重复，调用方或 `RemeshDirtyBlocks` 去重）。
    /// `boundsOut` 非空时写出**被改动采样的世界 AABB**（闭区间）—— 塌落用它把邻域收到真正相关的范围（T30）。
    bool CarveSphere(const glm::dvec3& center, float radiusBlocks, std::vector<BlockCoord>& dirtyOut,
                     VoxelBounds* boundsOut = nullptr);

    /// 重网格 `dirty` 中列出的块（去重），返回实际重网格的块数。未创建的块跳过。
    [[nodiscard]] std::size_t RemeshDirtyBlocks(const std::vector<BlockCoord>& dirty);

    // ---- 体素级读写（T29 塌落等规则用）----

    /// 读出一个世界空间矩形区域的密度采样；**区域外**（区域落在块集合之外）的采样填 `+127`（空）。
    [[nodiscard]] DensityRegion ReadDensityRegion(int minX, int minY, int minZ, int sizeX, int sizeY, int sizeZ) const;

    /// 把区域写回同布局的采样：只写**该处确实存在体积块**的样本，其余跳过；
    /// 写过的块标记为"已改动"（面板计数）。边界样本与邻块共享，故必须经本函数而不是直改数组。
    void WriteDensityRegion(const DensityRegion& region);

    [[nodiscard]] const std::map<BlockCoord, VolumeBlock>& Blocks() const noexcept { return m_blocks; }

    /// 该块的填充分类；**不存在的块返回 `Air`**（与 `ReadDensityRegion` 的填充口径一致）。
    [[nodiscard]] BlockFill FillOf(const BlockCoord& coord) const noexcept;

    [[nodiscard]] const MeshData* FindMesh(const BlockCoord& coord) const noexcept;

    [[nodiscard]] std::size_t CarvedBlockCount() const noexcept;

    /// 密度数据总字节数（面板 / 记账用）。
    [[nodiscard]] std::size_t VoxelBytes() const noexcept;

private:
    /// 高度场推导的密度（区域外回退路径；无地形数据 ⇒ 视为空）。
    [[nodiscard]] float TerrainDerivedDensity(double x, double y, double z) const noexcept;

    /// 地表高度（格）：优先查**足迹缓存**，未命中时回落到 `TerrainWorld::QueryHeight`。
    [[nodiscard]] bool SurfaceHeight(double x, double z, float& outHeight) const noexcept;

    /// 建立足迹范围内的地表高度缓存（整数列，含 1 格外扩）。
    void BuildSurfaceHeightCache();

    const TerrainWorld&      m_terrain;
    const DigRegionTable&    m_regions;
    std::map<BlockCoord, VolumeBlock> m_blocks;

    // 足迹缓存：覆盖 `[footprintMin − 1, footprintMax + 1]` 的整数列。
    std::vector<float> m_surfaceCache;
    int                m_cacheMinX = 0;
    int                m_cacheMinZ = 0;
    int                m_cacheWidth  = 0;
    int                m_cacheDepth  = 0;
};

}  // namespace vx
