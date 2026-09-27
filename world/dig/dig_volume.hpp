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
class TerrainMaterialTable;

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
    /// **体素级材质持久化**（T42 / [ADR 0014](../../docs/adr/0014-voxel-material-index.md) 的切换条件）：
    /// `33³` 槽位、与 `density` **同布局**；`kNoMaterialSlot` = "未写入" ⇒ 回落该列地表的派生材质。
    ///
    /// **懒分配**（`empty()` = 该块从未被写过材质 ⇒ 完全零内存，行为与引入本数组之前**逐位一致**）：
    /// 只有"塌落残骸落在此块"时才分配并写入 —— 塌落会把材质**搬走**（塔的岩体落到泥土区），
    /// 此时"材质 = 该列地表的纯函数"不再成立，必须记住它**原本是什么**。
    std::vector<std::uint8_t> material;
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
    ///
    /// 等价于 `BeginInitFromHeightField()` + `StepInitFromHeightField(全部步骤)`；保留此便捷入口给
    /// 单测与"不关心分帧"的调用方。**启动路径**应改用下面两个分步接口：块总数可达数百，
    /// 一次跑完会让画面停下等待（见 SKILL「不冻结画面」）。
    void InitFromHeightField();

    /// 开始一次**分步**初始化：清空块、建立地表高度足迹缓存、把游标复位。
    ///
    /// 之后反复调用 `StepInitFromHeightField` 推进；调用方可在两次调用之间出一帧画面，
    /// 窗口因此始终响应、加载进度可连续刷新。
    void BeginInitFromHeightField();

    /// 推进分步初始化：执行至多 `maxSteps` 个步骤，返回**是否全部完成**。
    ///
    /// 步骤总数为 `2 × 块数`：**先逐块填充密度、再逐块网格化**。分成两轮的原因是不可交换 ——
    /// 网格化会读取块外一层采样，只有**全部**密度就位后才与"一次性初始化"的结果**逐位一致**
    /// （否则邻块尚未填充时边界采样会回落到未取整的高度场推导值）。未先调用
    /// `BeginInitFromHeightField` 时游标为 0 —— 此时等价于从头开始。
    bool StepInitFromHeightField(std::size_t maxSteps);

    /// 分步初始化的总步数（= `2 × 块数`）。
    [[nodiscard]] std::size_t InitTotalSteps() const noexcept { return m_regions.Blocks().size() * 2U; }

    /// 分步初始化已完成的步数。
    [[nodiscard]] std::size_t InitCompletedSteps() const noexcept { return m_initCursor; }

    [[nodiscard]] bool Empty() const noexcept { return m_blocks.empty(); }

    /// 世界坐标（格）是否落在可挖区域内（= `DigRegionTable::IsDiggable`）。
    [[nodiscard]] bool IsInsideRegion(double x, double y, double z) const noexcept;

    /// 采样密度（**密度单位**，±127）：区域内读块数据（含已挖改动），区域外回退到高度场推导值。
    [[nodiscard]] float SampleDensity(double x, double y, double z) const noexcept;

    /// 采样**材质槽位**（ADR 0014 / T42）；`kNoMaterialSlot` = 未指定。
    ///
    /// 两级口径：① 该块**已被写入过**体素材质（`VolumeBlock::material` 非空且该样本非
    /// `kNoMaterialSlot`）⇒ 返回**存下来的**值（塌落搬来的残骸就是这样保留它原本的材质）；
    /// ② 否则回落"该列地表派生的可挖材质"（纯函数，零内存路径，与引入存储之前**逐位一致**）。
    [[nodiscard]] std::uint8_t SampleMaterialSlot(int worldX, int worldY, int worldZ) const noexcept;

    /// 按 `density` 同布局读出一片区域的**有效材质槽位**（已写入的优先，未写入的回落列派生）。
    ///
    /// 索引与 `DensityRegion` 一致：`x + sizeX * (y + sizeY * z)`（相对 `min*` 的偏移）。
    /// 逐列只做一次地表派生查询 ⇒ 可用于"整块倒塌整体的体素补丁"（T42）。
    [[nodiscard]] std::vector<std::uint8_t> ReadMaterialRegion(int minX, int minY, int minZ, int sizeX, int sizeY,
                                                              int sizeZ) const;

    /// 写入**单个样本**的材质槽位（T42 塌落回写用）：块不存在则忽略；块尚无材质数组时**懒分配**
    /// （一次 `33³`，此后该块走"已写入优先"路径）。`kNoMaterialSlot` 也会被写入（表示"显式回落列派生"）。
    void SetMaterialSlot(int worldX, int worldY, int worldZ, std::uint8_t slot);

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

    /// 重网格**单个**块（T37：延后破坏队列按"一个单位"推进时用，避免为单块构造临时 vector）。
    /// 返回该块是否存在（不存在 ⇒ 无操作）。语义与 `RemeshDirtyBlocks({coord})` 完全一致。
    bool RemeshBlock(const BlockCoord& coord);

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

    /// **体素材质数据**的总字节数（T42 记账用）：只统计**已分配**（被塌落写过材质）的块 ⇒
    /// 未发生倒塌时恒为 0（懒分配的实际占用可查，上限 = 块数 × 33³ ≈ 15.5 MB）。
    [[nodiscard]] std::size_t MaterialBytes() const noexcept;

    /// 本世界所用的**材质表**（启动期加载的同一份；地表与体积共用）。
    /// 用途（T43 / [ADR 0016](../../docs/adr/0016-collapse-realism-impulse-material-debris.md)）：倒塌整体的
    /// 质量 / 摩擦 / 弹性按 `SampleMaterialSlot` 得到的槽位查本表 ⇒ **石 ≠ 土 ≠ 草 ≠ 沙**。
    [[nodiscard]] const TerrainMaterialTable& Materials() const noexcept;

private:
    /// 高度场推导的密度（区域外回退路径；无地形数据 ⇒ 视为空）。
    [[nodiscard]] float TerrainDerivedDensity(double x, double y, double z) const noexcept;

    /// 初始化**单个**块的密度数组（分步初始化的第一轮：只填密度、不网格化）。
    void FillBlockDensity(const BlockCoord& coord);

    /// 网格化**单个**已填充密度的块，并同步填充分类（分步初始化的第二轮）。
    void MeshBlock(const BlockCoord& coord);

    /// 地表高度（格）：优先查**足迹缓存**，未命中时回落到 `TerrainWorld::QueryHeight`。
    [[nodiscard]] bool SurfaceHeight(double x, double z, float& outHeight) const noexcept;

    /// 建立足迹范围内的地表高度缓存（整数列，含 1 格外扩）。
    void BuildSurfaceHeightCache();

    const TerrainWorld&      m_terrain;
    const DigRegionTable&    m_regions;
    std::map<BlockCoord, VolumeBlock> m_blocks;

    /// 分步初始化的游标：已完成的步数（`InitCompletedSteps` 暴露给加载画面）。
    std::size_t m_initCursor = 0;

    // 足迹缓存：覆盖 `[footprintMin − 1, footprintMax + 1]` 的整数列。
    std::vector<float> m_surfaceCache;
    int                m_cacheMinX = 0;
    int                m_cacheMinZ = 0;
    int                m_cacheWidth  = 0;
    int                m_cacheDepth  = 0;
};

}  // namespace vx
