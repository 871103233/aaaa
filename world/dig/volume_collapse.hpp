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
    /// `hullPoints` 的**局部 AABB**（T46）：保留残骸的"唤醒判据"用的盒（见 `UnitWorldAabb`）。
    /// T48 起**不再**用于命中判定（命中改由物理引擎回答，见 [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）。
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
    /// **判据（T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策四）= 该子块自己的材质**
    /// （`materials.toml` 的 `rigid_debris`）：抽出的连通分量会先按**材质一致性**切成子块，
    /// 每个子块只有一种材质 ⇒ 岩子块保持形状、土 / 草 / 沙子块回写融合（"一条长条里岩土共存"因此各自正确）。
    /// T46 曾用"**露在外面的皮**上是否出现刚性材质"来判（见 `surfaceMaterialCounts`）——
    /// 那是在"整块一个 bool"的前提下最接近的近似；按材质拆子块后不再需要它来折中。
    bool        rigidDebris = false;
    /// 该整体**表面 cell**（会生成渲染网格顶点的那些 cell）的材质直方图 —— 与渲染口径**同源**：
    /// 对每个"8 角有实有空的 cell"取实体侧各角材质的众数，再统计全体表面 cell。
    /// 语义 = "这块碎块**露在外面的皮**是用什么材质做的"（= 玩家真正看到的东西）。
    /// **T50 起只作诊断**（刚体化日志里如实报出），不再参与 `rigidDebris` 的判定（见上）。
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

/// ---- T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策二：
/// **破坏时不切换表示** —— 在碎块**自身补丁**上雕刻并重网格 ----

/// 在`unit`自身的 `patchDensity` 上做一次**与地形同口径的球体平滑挖除**（T50）。
///
/// 口径（复用 `DigVolumeWorld::RasterizeBall` 的数学，一字不改）：对"距球心 < 半径 + 过渡带"的采样，
/// 按 `clamp(round((半径 − 距离) × 每格单位))` 抬高密度（只抬不降）；**不可破坏材质保持原状**。
/// 只写 `patchIsUnit != 0` 的采样（非本整体的采样在网格化时本就当空气，不该被改）。
///
/// 为什么不能沿用"惰性体素化回写"（缺陷 BUG2）：体素化是**中心 → 最近整数格**的硬量化 + 找空位落格，
/// 会把整块轮廓改掉 —— 而"岩石不允许任何形状变化"是硬约束。改在补丁上雕刻后，
/// 等值面是补丁采样的**纯函数** ⇒ **未被挖到的区域顶点逐位不变**（可证伪的不变量，见判据）。
///
/// 返回 false 表示没有任何采样被改动（球没碰到这块碎块）。
bool CarveCollapseUnitPatch(CollapseUnit& unit, const glm::dvec3& center, float radiusBlocks,
                            const TerrainMaterialTable& materials);

/// 由**补丁**重算该整体的体素清单 / 包围盒 / 凸包点集 / 刚体属性（T50：雕刻后必须执行）。
///
/// **质心保持不变**（局部坐标系不变）⇒ 刚体与渲染网格**原地不动**，只有被挖掉的部分消失。
/// 返回 false 表示**剩余体素为空**（整块被挖光）⇒ 调用方应删除该整体。
///
/// **划分口径**（T50 / ADR 0018 决策四）：`rigidDebris` 由该整体（子块）的**材质**决定
/// （`materials.toml` 的 `rigid_debris`）—— 每个子块只有一种材质，故不再需要"表面直方图"来折中；
/// `surfaceMaterialCounts` 退化为**诊断**（日志里"玩家看到的皮是什么材质"）。
[[nodiscard]] bool RefreshCollapseUnitFromPatch(CollapseUnit& unit, const TerrainMaterialTable& materials);

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
    /// ---- T49 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一：连通域 ----
    std::size_t domainVoxels   = 0;      ///< 本次**连通域**的体素数（= 可能受影响的整个结构，诊断用）
    bool        domainNarrowed = false;  ///< 是否真的按连通域收窄（false = 超界**保守回退固定窗口**）
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
/// 邻域（T49 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一）：
/// **scope = 以被改动采样为起点的实心连通域**（洪泛至结构边界），不再用固定窗口截断 ——
/// 固定窗口下"距破坏点超过窗口的远端中段"从不进入任何一次求解，于是长条被炸断两端后**中段永远悬空**。
/// 窗口从「被改动采样 ± (悬挑 + 1) 格」开始，连通域触到窗口边界就**翻倍扩张**；
/// 采样数一旦超过 `kMaxRegionSamples`（或扩张次数用尽）⇒ **告警 + 保守回退固定窗口**（显式例外，
/// 切换条件 = 持久结构图落地）。竖直范围仍按块级填充分类（T30 口径不变）。
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

/// `unit` 在 `pose` 下的**世界 AABB**（= 局部 AABB 的 8 角旋转后取包围盒）—— **保守**包含真实 OBB。
/// 用途（T46）：某体积块的碰撞体被重建后"唤醒与它相交的保留残骸"（宁可多唤醒一次，也不漏唤醒）。
/// 纯函数、无 GPU / Jolt 依赖 ⇒ 可单测。
///
/// **不再承担命中判定**（T48 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）：
/// 光球是否打到某个整体已改由物理引擎回答（`PhysicsWorld::RayCastDynamic`，落在真实凸包表面），
/// 原先的手工 OBB 判据（`LocalAabbContainsPoint`）因此**已下线**。
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
    /// 散体分量回写后把"下方为空"的体素沿本列下落所移动的体素数（刚性分量不做沉降 ⇒ 恒为 0）。
    std::size_t             settledVoxels = 0;
    /// **回写后仍悬空、已清除**的体素数（T51，2026-09-28）：散体沉降有 32 格上限，降不到支撑的残留体素
    /// 一律**直接从回写结果里去掉**（所有者指定："悬空的小土块可以直接删除或者降落到地上"）⇒
    /// 回写后**区域内不存在"下方为空"的实心体素**（区域底面除外）。刚性分量不做这一步（形状不变）。
    /// 名字曾为 `stuckVoxels` —— 改名的原因：旧实现只**计数 + 告警**、体素仍留在空中，与"不悬空"的契约不符。
    std::size_t             removedFloatingVoxels = 0;
    VoxelBounds bounds;                         ///< 回写涉及的体素范围（诊断）
};

/// 把一个抽出的整体按**落定位姿**体素化回写为地形（残骸可站、可继续挖）。
///
/// 作用域只在 `volumes` 上：不碰物理与渲染 ⇒ 可脱离 GPU / Jolt 单测（这正是"体素 ↔ 刚体转换"的核心数学）。
/// 线程约定：只在逻辑线程（主线程）调用。
[[nodiscard]] CollapseWriteback WritebackCollapseUnit(DigVolumeWorld& volumes, const CollapseUnit& unit,
                                                     const CollapsePose& pose);

}  // namespace vx
