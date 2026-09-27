#pragma once

#include "dig/collapse_table.hpp"
#include "dig/dig_volume.hpp"
#include "terrain/material_table.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// 一个即将倒塌的**整体**（T33）：一组失去支撑的实心体素的 **6 邻域连通分量**。
///
/// "整体"的语义 = **同一分量在倒塌时一起动**（同生共死），而不是各自沿本列下落。
/// 权威解释与备选方案见 [ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md)。
///
/// 为什么不再逐列下落：逐列模型**在原理上**只能产生"原地垂直下沉 / 局部陷落"，
/// 无法产生**倾斜与旋转**（人工实测第 7 轮的两塔即如此）；项目所有者 2026-09-27 明确**严禁**该效果。
struct CollapseUnit {
    /// 体素坐标（**世界整数坐标**；体素 = 该点向 +x/+y/+z 各 1 格的立方格）。
    struct Voxel {
        int x = 0;
        int y = 0;
        int z = 0;

        [[nodiscard]] bool operator<(const Voxel& other) const noexcept {
            if (x != other.x) {
                return x < other.x;
            }
            if (y != other.y) {
                return y < other.y;
            }
            return z < other.z;
        }
        [[nodiscard]] bool operator==(const Voxel& other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    std::vector<Voxel> voxels;      ///< 该整体的全部实心体素（世界坐标，**确定序**：按 x → z → y 升序）
    VoxelBounds        bounds;      ///< 体素范围（闭区间）
    glm::dvec3         centroid { 0.0 };  ///< 体素中心均值 ⇒ 刚体**局部原点**与渲染顶点基准
    std::vector<float> hullPoints;  ///< `3 * N` 个**局部坐标**（相对 `centroid`）——`ConvexHullDesc` 用
    /// `hullPoints` 的**局部 AABB**（T46）：光球命中判定用的盒（随姿态旋转 = OBB），见 `LocalAabbContainsPoint`。
    glm::vec3          hullMinLocal { 0.0F };
    glm::vec3          hullMaxLocal { 0.0F };
    /// ---- T42：**体素补丁**（抽出前抓取；供"与地形同源"的区域网格化）----
    ///
    /// 为什么需要：倒塌整体原先自造"逐体素方块面 + 面法线"，与地形 / 洞的等值面口径不同 ⇒
    /// 掉落中棱角明显、落地后又变回平滑。补丁让 `BuildCollapseUnitMesh` 用的是**同一份 Surface Nets**，
    /// 于是抽出来的那一块**就是它原本的那一片等值面**（顶点与原地形网格在重叠处逐位一致）。
    /// 补丁比体素包围盒**向外各扩 1 格采样**：等值面在边界处要用到外围采样（与块间共享边界同理）。
    VoxelBounds               patchBounds;   ///< 补丁的**世界采样范围**（闭区间，= `bounds` ± 1）
    int                       patchSizeX = 0;  ///< 补丁每轴采样数（= 体素数 + 2，见 `patchBounds`）
    int                       patchSizeY = 0;
    int                       patchSizeZ = 0;
    std::vector<std::int8_t>  patchDensity;   ///< `patchSizeX × Y × Z`，索引 `x + NX * (y + NY * z)`
    std::vector<std::uint8_t> patchMaterial;  ///< 同布局的**有效材质槽位**（ADR 0014 / T42）
    /// 同布局的**归属掩码**（T46 修订）：1 = 该采样属于本整体（落在 `voxels` 里），0 = 不属于。
    ///
    /// 用途（缺陷修复）：网格化时把"**非本整体的实心采样**"当作**空气**，否则碎块**还连在未塌岩体上**
    /// 的那些面不会有密度变号 ⇒ **不生成等值面** ⇒ 掉落中能从断口处**看穿**它（物理却有碰撞体）。
    /// 只改"外观网格"的输入：**原始就暴露在空气中的那些面**顶点与改前**逐位一致**（同源口径不变）。
    std::vector<std::uint8_t> patchIsUnit;
    /// ---- T43 / [ADR 0016](../../docs/adr/0016-collapse-realism-impulse-material-debris.md)：刚体属性 ----
    ///
    /// 全部在 world 层算好（**可脱离 GPU / Jolt 单测**），`game` 只把它们填进 `ConvexHullDesc`。
    float       mass        = 1.0F;  ///< 质量 = Σ（各体素材质的 `density`）—— 混合材质因此正确
    float       friction    = 0.5F;  ///< 摩擦 = **多数材质**的值（同票按槽位序号升序）
    float       restitution = 0.0F;  ///< 弹性 = **多数材质**的值
    /// 是否有**爆心冲量**：`impulseSpeed > 0`、种子带有效半径、且折算出的 `V` / `ω` **非零**
    /// （整个分量都在爆心半径之外时为零 ⇒ 不算有冲量）。false ⇒ 调用方退回人工倾斜。
    bool        hasImpulse  = false;
    glm::vec3   impulseLinearVelocity { 0.0F };   ///< 由冲量分布折算的初始线速度（格/秒）
    glm::vec3   impulseAngularVelocity { 0.0F };  ///< 由冲量分布折算的初始角速度（rad/s）
    /// ---- T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)：落地后的表示 ----
    ///
    /// **刚性碎块**：true ⇒ 落定后**保留几何体**（不回写、形状不变），false ⇒ 落定后体素化回写并与地面融合
    /// （**接地沉降**，不允许悬空）。
    ///
    /// **判据 = 碎块「露在外面的皮」上是否出现刚性材质**（见 `surfaceMaterialCounts`），
    /// 而**不是**全体体素的多数材质 —— 后者会被"派生材质"带偏：一处陡壁的内部体素按该列地表派生出来
    /// 往往是土，于是"看着是白岩的一片"会被判成散体、落地后被量化融合（项目所有者实测的缺陷）。
    /// "岩石落地后不允许发生任何形状变化"是**硬约束**，故取"表面含刚性即保持形状"。
    bool        rigidDebris = false;
    /// 该整体**表面 cell**（会生成渲染网格顶点的那些 cell）的材质直方图 —— 与渲染口径**同源**：
    /// 对每个"8 角有实有空的 cell"取实体侧各角材质的众数，再统计全体表面 cell。
    /// 语义 = "这块碎块**露在外面的皮**是用什么材质做的"（= 玩家真正看到的东西）。用于判 `rigidDebris`。
    int         surfaceMaterialCounts[kMaterialSlotCount] = {};
    /// 体素总数（= `voxels.size()`；单独留着便于日志 / 判据阅读）。
    [[nodiscard]] std::size_t VoxelCount() const noexcept { return voxels.size(); }
};

/// 把一个倒塌整体网格化成**它的体素补丁的等值面**（T42）——与地形 / 洞**同源**：
/// Surface Nets 平滑顶点 + 密度梯度法线 + 逐样本材质众数（`BuildRegionMesh`）。
///
/// 输出：`MeshData`（**索引网格**），顶点是**相对 `unit.centroid` 的局部坐标**（与 `hullPoints` 同一坐标系
/// ⇒ 渲染与物理同源，可由同一个 `mat4` 变换驱动）。补丁为空时返回空网格。
[[nodiscard]] MeshData BuildCollapseUnitMesh(const CollapseUnit& unit);

/// 一次"整体倒塌"规划的产物（T33）。
struct CollapsePlan {
    std::vector<CollapseUnit> units;    ///< 各失支撑的连通分量（**已从体积中抽出**）
    std::vector<BlockCoord>   dirty;    ///< 抽出后需要重网格 / 重建碰撞体的块（未去重）
    std::size_t unsupportedVoxels = 0;  ///< 失去支撑的实心体素总数（诊断，含未抽出的）
    std::size_t skippedUnits      = 0;  ///< 因超出 `maxUnits` 而**未抽出**的分量数（诊断 / 告警）
    /// **小碎片清除**（T43 / ADR 0016 决策三）：体素数 ≤ `debris_delete_max_voxels` 且不含不可破坏材质的分量
    /// 被**直接清除**（"当炸没了"），不计入 `units`；这两个计数供日志 / 面板核对。
    std::size_t deletedUnits      = 0;
    std::size_t deletedVoxels     = 0;
    std::size_t regionSamples     = 0;  ///< 支撑检查邻域的采样数（T30 / T38 计时口径）
};

/// 塌落的**种子**（T30 / T43）：本次挖除改动过的采样范围 + （可选）**爆心**。
/// 邻域**只**围绕 `bounds` 展开 —— 挖一个 6 格半径的球，绝不该让"整段地下 + 几十格外的山体 / 塔"都进邻域。
struct CollapseSeed {
    /// 被改动采样的世界 AABB（闭区间）；默认值（`Empty()`）表示没有改动。
    VoxelBounds bounds;

    /// **爆心**（世界坐标，格）与**影响半径**（格）—— T43 的冲量方向与衰减基准。
    /// `radius ≤ 0` = 本次没有冲量来源（退回人工倾斜）：见 `CollapseUnit::hasImpulse`。
    glm::dvec3 epicenter { 0.0 };
    double     radius = 0.0;

    /// 由块范围构造"块对齐的种子"（保守：邻域会略大于真实改动范围；**不含爆心**）。
    /// 供测试与"只知道块、不知道采样"的调用方使用。
    [[nodiscard]] static CollapseSeed FromBlocks(const BlockCoord& minBlock, const BlockCoord& maxBlock) noexcept {
        CollapseSeed seed;
        seed.bounds = VoxelBounds { BlockOriginBlocks(minBlock.x), BlockOriginBlocks(minBlock.y),
                                    BlockOriginBlocks(minBlock.z),
                                    BlockOriginBlocks(maxBlock.x) + kVolumeBlockSize,
                                    BlockOriginBlocks(maxBlock.y) + kVolumeBlockSize,
                                    BlockOriginBlocks(maxBlock.z) + kVolumeBlockSize };
        return seed;
    }
};

/// 在 `seed` 的邻域内做一次「**支撑检查 → 连通分量分组 → 抽出**」，把改动写回 `volumes`。
/// [ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md)「决策 一 / 三」。
///
/// 算法（全部在**世界整数坐标**的体素场上做，**确定性**：红线 7 / 11）：
///   ① **支撑体素** = "载荷能沿实心一路传到地底"的实心体素：
///      先逐列自下而上取**纵向**判据（只有"从邻域底面起连续实心"的那一段算接地；邻域底面视作地面 ——
///      它的下方已被证明全是整块实心）；
///      再把接地沿**同一高度层的实心连通**横向传播 `floor(maxCantileverBlocks)` 步（= 悬挑 / 拱效应）；
///   ② **失去支撑** = 实心但不在①里 —— 它的下方通路已被破坏，且离最近的接地处超出允许跨度；
///   ③ 失去支撑的体素按 **6 邻域连通分量**分组 ⇒ **每个分量 = 一个整体**；每个整体：
///      算出质心与"逐列 8 个角点"的凸包点集，登记材质槽位，并**立即从体积中清空**这些体素
///      （⇒ 返回的 `units` 即可交给物理层转成动态刚体，见 `PhysicsWorld::AddDynamicConvexHull`）。
///
/// 邻域（T30）：**水平** = 被改动采样的世界范围外扩 (悬挑 + 1) 格 —— 悬挑最多跨
/// `maxCantileverBlocks` 步，更远的列不可能被本次挖除影响；
/// **竖直** = 自种子块向下到"最低的非全实心块"、向上到"最高的非全空块"。
///
/// **不做连锁**（单次判定、不迭代）：迭代到稳定会让山体里一条 12 格宽的隧道把整座山连锁塌掉，
/// 与真实不符；代价是"本次未判定的部分保持原状"，已登记为后续项（ADR 0015「后果」）。
///
/// `maxUnits`：本次最多抽出几个整体（= 同时活跃的刚体上限，受渲染网格池约束）。
/// 超出的分量**保持原状**（不抽出、不消失），只计数并告警 —— 宁可"这次没倒"，
/// 也不能让体素被抽出后又没有刚体可承载（那会凭空消失，破坏世界自洽）。按**体素数降序 + 坐标升序**
/// 的确定序选取，故与线程 / 时序无关。
///
/// 前置条件：`seed.bounds` 若非空，则它必须来自"刚被挖除的采样"（通常是 `CarveSphere` 的 `boundsOut`）；
/// `maxUnits >= 1`。
/// 线程约定：只在逻辑线程（主线程）调用，不得与体积的其它写操作并发。
[[nodiscard]] CollapsePlan ApplyCollapse(DigVolumeWorld& volumes, const CollapseSeed& seed,
                                        const CollapseSpec& spec, std::size_t maxUnits);

/// 倒塌整体落定时的**位姿**（局部原点 = `CollapseUnit::centroid`，与物理层的刚体约定一致）。
struct CollapsePose {
    glm::dvec3 position { 0.0 };
    glm::quat  rotation { 1.0F, 0.0F, 0.0F, 0.0F };

    /// 位姿的"倾角"（度）：局部 +Y 与世界上方的夹角 —— 判"是倒了还是只是原地落下"。
    [[nodiscard]] float TiltDegrees() const noexcept;
};

/// 该世界点是否落在「`unit` 的局部 AABB 按 `pose` 旋转后的盒」内（T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)）。
///
/// 用途：**保留中的刚性残骸**不在体素里（落定后不回写）⇒ 必须让"光球打得到它"（否则会穿过看着是实心的岩石）。
/// 判据是**局部 AABB 的 OBB**（不是凸包本身）：凸包的真实边界比它小 ⇒ 可能"早一点"命中（ADR 0017 后果 5）。
/// 纯函数、无 GPU / Jolt 依赖 ⇒ 可单测。
[[nodiscard]] bool LocalAabbContainsPoint(const CollapseUnit& unit, const CollapsePose& pose,
                                          const glm::dvec3& point) noexcept;

/// `unit` 在 `pose` 下的**世界 AABB**（= 局部 AABB 的 8 角旋转后取包围盒）—— **保守**包含真实 OBB。
/// 用途（T46）：某体积块的碰撞体被重建后"唤醒与它相交的保留残骸"（宁可多唤醒一次，也不漏唤醒）。
/// 纯函数、无 GPU / Jolt 依赖 ⇒ 可单测。
void UnitWorldAabb(const CollapseUnit& unit, const CollapsePose& pose, glm::dvec3& outMin,
                   glm::dvec3& outMax) noexcept;

/// **体素化回写**的结果（[ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md) 决策 三.5）。
struct CollapseWriteback {
    std::vector<BlockCoord> dirty;              ///< 需要重网格 / 重建碰撞体的块（未去重）
    std::size_t             writtenVoxels = 0;  ///< 成功写回的体素数
    /// 目标格被占 / 越界而丢弃的体素数。**已知放松**：按最终姿态落位到最近格，重叠时找邻近空格，
    /// 仍可能少量丢弃 —— 与 T29 的"严格质量守恒"口径不同（ADR 0015 后果 3 已登记）。
    std::size_t             droppedVoxels = 0;
    /// **接地沉降**（T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md) 决策三）：
    /// 散体分量回写后把"下方为空"的体素沿本列下落所移动的体素数 / 达到下落上限仍未接地的体素数。
    /// 刚性分量不做沉降（保持形状）⇒ 恒为 0。
    std::size_t             settledVoxels = 0;
    std::size_t             stuckVoxels   = 0;
    VoxelBounds bounds;                         ///< 回写涉及的体素范围（诊断）
};

/// 把一个抽出的整体按**落定位姿**体素化回写为地形（残骸可站、可继续挖）。
///
/// 作用域只在 `volumes` 上：不碰物理与渲染 ⇒ 可脱离 GPU / Jolt 单测（这正是"体素 ↔ 刚体转换"的核心数学）。
/// 线程约定：只在逻辑线程（主线程）调用。
[[nodiscard]] CollapseWriteback WritebackCollapseUnit(DigVolumeWorld& volumes, const CollapseUnit& unit,
                                                     const CollapsePose& pose);

}  // namespace vx
