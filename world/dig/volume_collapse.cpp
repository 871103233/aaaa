#include "dig/volume_collapse.hpp"

#include "core/log.hpp"
#include "terrain/material_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace vx {
namespace {

/// 支撑检查邻域的采样数上限（8³ 百万）：超过即放弃本次塌落并告警 —— 防止异常大的挖除把内存拉爆。
constexpr std::size_t kMaxRegionSamples = 8U * 1024U * 1024U;

/// 世界坐标（格）→ 所在块索引（向下取整，负数也正确）。
[[nodiscard]] int BlockIndexOfWorld(int coordinate) noexcept {
    return (coordinate >= 0) ? (coordinate / kVolumeBlockSize)
                             : -((-coordinate + kVolumeBlockSize - 1) / kVolumeBlockSize);
}

/// 密度值是否表示"实心"（与 `DigVolumeWorld::IsSolid` 同口径：负 = 实心）。
[[nodiscard]] bool IsSolidValue(std::int8_t value) noexcept { return value < 0; }

/// 体素排序：先 x、再 z、最后 y —— 这样**同一 (x, z) 列的体素在序列里连续**，
/// 便于一趟扫出"逐列最低 / 最高"，且序完全确定（红线 7）。
[[nodiscard]] bool VoxelLessByColumn(const CollapseUnit::Voxel& left, const CollapseUnit::Voxel& right) noexcept {
    if (left.x != right.x) {
        return left.x < right.x;
    }
    if (left.z != right.z) {
        return left.z < right.z;
    }
    return left.y < right.y;
}

/// 分量选取的确定序：**体素数降序**，同数时按最小角点（x, y, z）升序。
[[nodiscard]] bool UnitPreferred(const CollapseUnit& left, const CollapseUnit& right) noexcept {
    if (left.voxels.size() != right.voxels.size()) {
        return left.voxels.size() > right.voxels.size();
    }
    if (left.bounds.minX != right.bounds.minX) {
        return left.bounds.minX < right.bounds.minX;
    }
    if (left.bounds.minY != right.bounds.minY) {
        return left.bounds.minY < right.bounds.minY;
    }
    return left.bounds.minZ < right.bounds.minZ;
}

/// 世界位置是否落在**已存在的体积块**内（只有这些采样能被 `WriteDensityRegion` 写进去）。
[[nodiscard]] bool CoveredByVolumes(const DigVolumeWorld& volumes, int x, int y, int z) {
    const BlockCoord block { BlockIndexOfWorld(x), BlockIndexOfWorld(y), BlockIndexOfWorld(z) };
    return volumes.Blocks().find(block) != volumes.Blocks().end();
}

/// 体素补丁的扁平下标（布局与 `DensityRegion` 相同：`x + NX * (y + NY * z)`）。
[[nodiscard]] std::size_t PatchIndex(const CollapseUnit& unit, int x, int y, int z) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(unit.patchSizeX) *
               (static_cast<std::size_t>(y) + static_cast<std::size_t>(unit.patchSizeY) * static_cast<std::size_t>(z));
}

/// 抓取一个倒塌整体的**体素补丁**（T42）：密度 + 有效材质（ADR 0014），范围 = `bounds` **向外各扩 1 格采样**。
///
/// 必须在把该整体的体素清空**之前**调用 —— 清空后这里的密度就变空了（补丁是"它原本的样子"的证据）。
/// 多扩的一圈与块网格化"越界取外围采样"同源：等值面在补丁边界处才不会缺料。
void CaptureUnitPatch(const DigVolumeWorld& volumes, CollapseUnit& unit) {
    unit.patchBounds = VoxelBounds { unit.bounds.minX - 1, unit.bounds.minY - 1, unit.bounds.minZ - 1,
                                     unit.bounds.maxX + 1, unit.bounds.maxY + 1, unit.bounds.maxZ + 1 };
    unit.patchSizeX  = unit.patchBounds.maxX - unit.patchBounds.minX + 1;
    unit.patchSizeY  = unit.patchBounds.maxY - unit.patchBounds.minY + 1;
    unit.patchSizeZ  = unit.patchBounds.maxZ - unit.patchBounds.minZ + 1;

    DensityRegion density = volumes.ReadDensityRegion(unit.patchBounds.minX, unit.patchBounds.minY,
                                                      unit.patchBounds.minZ, unit.patchSizeX, unit.patchSizeY,
                                                      unit.patchSizeZ);
    unit.patchDensity = std::move(density.values);
    unit.patchMaterial = volumes.ReadMaterialRegion(unit.patchBounds.minX, unit.patchBounds.minY, unit.patchBounds.minZ,
                                                    unit.patchSizeX, unit.patchSizeY, unit.patchSizeZ);

    // T46 修订（断口闭合）：标出"哪些采样属于本整体" —— 网格化时**非本整体的实心采样**要当空气。
    unit.patchIsUnit.assign(static_cast<std::size_t>(unit.patchSizeX) * static_cast<std::size_t>(unit.patchSizeY) *
                                static_cast<std::size_t>(unit.patchSizeZ),
                            0U);
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        const int px = voxel.x - unit.patchBounds.minX;
        const int py = voxel.y - unit.patchBounds.minY;
        const int pz = voxel.z - unit.patchBounds.minZ;
        if (px >= 0 && px < unit.patchSizeX && py >= 0 && py < unit.patchSizeY && pz >= 0 && pz < unit.patchSizeZ) {
            unit.patchIsUnit[PatchIndex(unit, px, py, pz)] = 1U;
        }
    }
}

/// 该分量的体素里是否出现**不可破坏**材质（T43 的"清除守卫"；ADR 0013 的完整语义**未实现**）。
///
/// 直接按体素查材质（**不经补丁**）：清除候选都是小分量（≤ `debris_delete_max_voxels`），代价可忽略；
/// 这样"被跳过的分量"不必白抓一次补丁。
[[nodiscard]] bool ContainsIndestructibleMaterial(const DigVolumeWorld& volumes, const TerrainMaterialTable& materials,
                                                  const CollapseUnit& unit) {
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        const std::uint8_t slot = volumes.SampleMaterialSlot(voxel.x, voxel.y, voxel.z);
        if (slot != kNoMaterialSlot && materials.Layer(static_cast<int>(slot)).indestructible) {
            return true;
        }
    }
    return false;
}

/// 体素补丁 → `IVolumeSampler`（T42）：采样索引 `-1 .. size` ↔ 补丁索引 `0 .. size + 1`。
///
/// 之所以能直接复用 `BuildRegionMesh`：补丁就是"以该整体为中心的一小片密度场"，且已向外多取一圈
/// ⇒ 与块网格化的边界口径完全一致，等值面无接缝、与原地形网格在重叠处逐位一致。
class UnitPatchSampler final : public IVolumeSampler {
public:
    explicit UnitPatchSampler(const CollapseUnit& unit) noexcept : m_unit(unit) {}

    /// **采样索引与补丁索引一一对应**（T46 修订：不再 +1）。
    ///
    /// 为什么改：网格器"每条网格棱由 u/v 下侧的 cell 发射一次"——独立碎块**没有相邻块**替它发射边界棱，
    /// 于是越界那一圈面会**整个缺失**（实测：四周全是空气的悬空石板有 36 条边界边 ⇒ 掉落中能看穿它）。
    /// 把采样器改成"区域 0 号采样 = 补丁 0 号采样（= `bounds` 外扩的那一圈）"，并让区域**多覆盖一格 cell**，
    /// 边界棱就有了归属 ⇒ 等值面闭合。补丁外一律返回空气（那是真空气，不是缺数据）。
    [[nodiscard]] float Sample(int i, int j, int k) const override {
        const int x = i;
        const int y = j;
        const int z = k;
        if (x < 0 || x >= m_unit.patchSizeX || y < 0 || y >= m_unit.patchSizeY || z < 0 || z >= m_unit.patchSizeZ) {
            return static_cast<float>(kDensityMax);  // 补丁之外 = 空气
        }
        const std::size_t index = PatchIndex(m_unit, x, y, z);
        // T46 修订：**非本整体的实心采样一律当空气** —— 只影响外观网格；见 `CollapseUnit::patchIsUnit` 的说明。
        // 后果：碎块在"断口"（原本连着未塌岩体的那几面）上也会生成等值面 ⇒ 闭合、不再被看穿；
        //       而**原本就暴露在空气中**的面，其两侧采样与改前完全相同 ⇒ 顶点逐位不变（"与地形同源"不变）。
        if (!m_unit.patchIsUnit.empty() && m_unit.patchIsUnit[index] == 0U &&
            IsSolidValue(m_unit.patchDensity[index])) {
            return static_cast<float>(kDensityMax);
        }
        return static_cast<float>(m_unit.patchDensity[index]);
    }

    [[nodiscard]] std::uint8_t SampleMaterial(int i, int j, int k) const override {
        const int x = i;
        const int y = j;
        const int z = k;
        if (x < 0 || x >= m_unit.patchSizeX || y < 0 || y >= m_unit.patchSizeY || z < 0 || z >= m_unit.patchSizeZ) {
            return kNoMaterialSlot;
        }
        return m_unit.patchMaterial[PatchIndex(m_unit, x, y, z)];
    }

private:
    const CollapseUnit& m_unit;
};

/// Surface Nets 的 cell 角偏移（与 `volume_mesher.cpp` 的 `kCornerOffsets` 同序同义）。
constexpr int kCellCornerOffsets[8][3] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
    { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 },
};

/// 统计该整体**表面 cell** 的材质直方图（T46 引入 / T50 起**只作诊断**）。
///
/// 口径与**渲染网格逐字同源**（`volume_mesher.cpp` 的顶点材质规则）：遍历与网格器**同一批 cell**
/// （含外围一格，`i ∈ [-1, size)`），对"8 角有实有空的 cell"取**实体侧各角材质**的众数（同票取更小槽位），
/// 再统计全体表面 cell。语义 = "这块碎块**露在外面的皮**用什么材质做的" —— 也就是玩家真正看到的东西。
///
/// T46 曾用它判 `rigidDebris`（"皮上出现任何刚性材质即保持形状"）；**T50 起不再是判据** ——
/// 抽出后已按材质拆成子块（决策四），每个子块只有一种材质，直接取该材质的 `rigid_debris` 更准。
/// 保留它是因为刚体化日志仍要如实报出"玩家看到的是什么"。
void CountUnitSurfaceMaterials(CollapseUnit& unit) {
    const UnitPatchSampler sampler(unit);
    // 与 `BuildCollapseUnitMesh` 用**同一批 cell**（= 网格器会产生顶点的那一圈，见 `UnitPatchSampler`）。
    const int sizeX = unit.patchSizeX - 1;
    const int sizeY = unit.patchSizeY - 1;
    const int sizeZ = unit.patchSizeZ - 1;
    if (sizeX < 1 || sizeY < 1 || sizeZ < 1) {
        return;
    }
    for (int k = -1; k < sizeZ; ++k) {
        for (int j = -1; j < sizeY; ++j) {
            for (int i = -1; i < sizeX; ++i) {
                int solidCorners           = 0;
                int counts[kMaterialSlotCount] = {};
                for (int corner = 0; corner < 8; ++corner) {
                    const int dx = kCellCornerOffsets[corner][0];
                    const int dy = kCellCornerOffsets[corner][1];
                    const int dz = kCellCornerOffsets[corner][2];
                    if (sampler.Sample(i + dx, j + dy, k + dz) >= 0.0F) {
                        continue;  // 空侧不参与（与网格器一致）
                    }
                    ++solidCorners;
                    const std::uint8_t slot = sampler.SampleMaterial(i + dx, j + dy, k + dz);
                    if (slot != kNoMaterialSlot && slot < kMaterialSlotCount) {
                        ++counts[slot];
                    }
                }
                if (solidCorners == 0 || solidCorners == 8) {
                    continue;  // 全空 / 全实心 ⇒ 该 cell 不产生顶点，也就不是"暴露表面"
                }
                int best = 0;
                for (int slot = 1; slot < kMaterialSlotCount; ++slot) {
                    if (counts[slot] > counts[best]) {
                        best = slot;  // 只取"严格更多"⇒ 同票保留更小槽位（确定性）
                    }
                }
                ++unit.surfaceMaterialCounts[best];
            }
        }
    }
}

/// 算出一个整体的**刚体属性**（T43 / [ADR 0016](../../docs/adr/0016-collapse-realism-impulse-material-debris.md)）：
/// 质量（= Σ 各体素材质的 `density`）、摩擦 / 弹性（= **多数材质**，同票取更小槽位 ⇒ 确定性），
/// 以及**落地口径**（T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策四：
/// 每个子块只有一种材质 ⇒ 直接取该材质的 `rigid_debris`）。
///
/// 前置条件：`unit.patchMaterial` 已抓（`CaptureUnitPatch`）。**无 GPU / Jolt 依赖** ⇒ 可单测。
void ComputeUnitPhysics(CollapseUnit& unit, const TerrainMaterialTable& materials) {
    std::array<std::size_t, static_cast<std::size_t>(kMaterialSlotCount)> counts {};
    double                                                               mass = 0.0;
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        const int px = voxel.x - unit.patchBounds.minX;
        const int py = voxel.y - unit.patchBounds.minY;
        const int pz = voxel.z - unit.patchBounds.minZ;
        std::uint8_t slot = kNoMaterialSlot;
        if (px >= 0 && px < unit.patchSizeX && py >= 0 && py < unit.patchSizeY && pz >= 0 &&
            pz < unit.patchSizeZ) {
            slot = unit.patchMaterial[PatchIndex(unit, px, py, pz)];
        }
        // 未知材质（`kNoMaterialSlot`）按槽位 0 计 —— 体素在区域内正常拿得到材质，故这属防御分支。
        const int index = (slot == kNoMaterialSlot) ? 0 : static_cast<int>(slot);
        ++counts[static_cast<std::size_t>(index)];
        mass += static_cast<double>(materials.Layer(index).density);
    }
    unit.mass = static_cast<float>(mass);

    std::size_t best = 0;
    for (std::size_t slot = 1; slot < counts.size(); ++slot) {
        if (counts[slot] > counts[best]) {
            best = slot;  // 只取"严格更多"⇒ 同票保留更小槽位（确定性）
        }
    }
    unit.friction    = materials.Layer(static_cast<int>(best)).friction;
    unit.restitution = materials.Layer(static_cast<int>(best)).restitution;

    // ---- T50（ADR 0018 决策四）：落地口径 = **该子块自己的材质** ----
    // 抽出的连通分量已按材质一致性切成子块 ⇒ 一个子块只有一种材质，`best` 就是它。
    // "岩石落地后不允许发生任何形状变化"是硬约束 ⇒ 岩子块 `rigid_debris = true`（保留几何体），
    // 土 / 草 / 沙子块 `false`（回写融合 + 接地沉降）。
    unit.rigidDebris = materials.Layer(static_cast<int>(best)).rigidDebris;

    // T46 起保留的**表面材质直方图**（T50 起只作诊断）：日志里如实报出"玩家看到的皮是什么材质"。
    CountUnitSurfaceMaterials(unit);
}

/// **爆心冲量**（T43 / ADR 0016 决策一）：逐体素向外、按距离线性衰减 ⇒ 折算成刚体的 V / ω。
///
/// 只在"抽出那一刻"算一次（`ApplyCollapse`）；T50 的"在自身补丁上雕刻后重算"**不重算冲量**
/// （碎块是被打中的，不是被新炸出来的；速度由物理引擎继承）。
void ComputeUnitImpulse(CollapseUnit& unit, const CollapseSeed& seed, const CollapseSpec& spec) {
    unit.hasImpulse            = false;
    unit.impulseLinearVelocity  = glm::vec3(0.0F);
    unit.impulseAngularVelocity = glm::vec3(0.0F);
    if (!(spec.impulseSpeed > 0.0F) || !(seed.radius > 0.0)) {
        return;  // 无冲量来源 ⇒ 交给调用方退回"人工倾斜"（initial_tilt_speed）
    }

    const double count = static_cast<double>(unit.voxels.size());
    glm::dvec3   sumVelocity(0.0);
    glm::dvec3   sumMoment(0.0);
    double       sumRadiusSquared = 0.0;
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        const glm::dvec3 point(static_cast<double>(voxel.x) + 0.5, static_cast<double>(voxel.y) + 0.5,
                               static_cast<double>(voxel.z) + 0.5);
        const glm::dvec3 offset   = point - seed.epicenter;
        const double     distance = glm::length(offset);
        const double     falloff  = std::clamp(1.0 - distance / seed.radius, 0.0, 1.0);
        if (falloff <= 0.0) {
            continue;  // 半径之外：不参与冲量（但仍计入上面的"平均"分母 ⇒ 口径 = 该整体的平均初速）
        }
        const glm::dvec3 direction = (distance > 1.0e-6) ? (offset / distance) : glm::dvec3(0.0, 1.0, 0.0);
        const glm::dvec3 velocity  = direction * (static_cast<double>(spec.impulseSpeed) * falloff);
        sumVelocity += velocity;
        const glm::dvec3 arm = point - unit.centroid;
        sumMoment += glm::cross(arm, velocity);
        sumRadiusSquared += glm::dot(arm, arm);
    }

    const glm::dvec3 linear  = sumVelocity / count;
    const glm::dvec3 angular = (sumRadiusSquared > 1.0e-12) ? (sumMoment / sumRadiusSquared) : glm::dvec3(0.0);
    // 防御：绝不把 NaN / Inf 喂给物理（退化几何下才可能，正常不可达）。
    if (!std::isfinite(linear.x) || !std::isfinite(linear.y) || !std::isfinite(linear.z) ||
        !std::isfinite(angular.x) || !std::isfinite(angular.y) || !std::isfinite(angular.z)) {
        return;
    }
    // 冲量在数值上为零（整个分量都落在爆心半径之外 / 正好对称抵消）⇒ 视作**没有冲量来源**，
    // 让调用方退回人工倾斜。否则细长的塔会"原地垂直落下"而不倒 —— 那正是要避免的观感
    // （与 `hasImpulse` 的语义一致："有冲量用冲量，没有才退回人工不对称"）。
    constexpr double kMinImpulse = 1.0e-6;
    if (glm::length(linear) < kMinImpulse && glm::length(angular) < kMinImpulse) {
        return;
    }
    unit.hasImpulse             = true;
    unit.impulseLinearVelocity  = glm::vec3(linear);
    unit.impulseAngularVelocity = glm::vec3(angular);
}

/// 落定回写时"找空位"的**确定顺序**（相对目标格的偏移）：先原位、再向上 3 格，再 4 个水平邻格，最后向下 1 格。
/// 取第一个"空 + 在体积块覆盖内"的格；全都不可用则丢弃该体素并计数（ADR 0015 后果 3 已登记的放松）。
constexpr int kPlacementOffsets[9][3] = {
    { 0, 0, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 1, 0, 0 }, { -1, 0, 0 },
    { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, 3, 0 },
};

/// **接地沉降的最大下落格数**（T46 / ADR 0017 决策三）：散体分量回写时把目标区域**向下扩这么多格**，
/// 供"下方为空"的体素沿本列下落（回写按"最近格"落位、候选偏移只向上 / 水平 ⇒ 落点下方可能是空腔）。
/// 取 32 的依据：落定时刚体已被物理判为"静止"，故落点与真实支撑的差通常 ≤ 2 格；32 是极宽裕的上界。
/// **走满 / 落到区域之外并不再留下悬空体素**（T51，2026-09-28）：沉降之后有一次"仍悬空 ⇒ 清除"的扫描，
/// 把降不到支撑的残留体素**直接去掉**（见 `WritebackCollapseUnit`）。
constexpr int kSettleMaxDropBlocks = 32;

// ---------------------------------------------------------------------------
// T49（[ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一）：
// 支撑求解的 **scope 从"固定窗口"改为"实心连通域"**。判据（纵向接地 + 同层悬挑传播）**一字未改**，
// 改的只是"哪些体素进入这次求解"。
// ---------------------------------------------------------------------------

/// 连通域窗口的**扩张次数上限**（每次翻倍）：12 次 = 4096 倍起始半宽，足以覆盖任何真实结构；
/// 到不了边界即按"超界"回退固定窗口（ADR 0018 决策一的显式例外）。
constexpr int kMaxExpandIterations = 12;

/// 一次"支撑求解窗口"的探测结果（T49）。
struct CollapseProbe {
    DensityRegion             region;                ///< 窗口内的密度
    std::vector<std::uint8_t> reached;               ///< **有支撑** = ① 纵向接地 + ③ 同层悬挑传播
    std::vector<std::uint8_t> domain;                ///< **连通域** = 可能受本次破坏影响的实心体素
    std::size_t               domainVoxels    = 0;   ///< 连通域体素数（诊断）
    /// 连通域触到窗口的哪一侧**水平**边界（需要向那一侧扩张窗口）。
    /// 分侧记录是为了**只扩需要扩的那几侧**：细长结构（长条 / 塔）只会触到两端，
    /// 窗口因此保持"细长"，每轮分析的成本与实际结构形状成正比，而不是变成一个大立方体。
    bool                      touchesMinX = false;
    bool                      touchesMaxX = false;
    bool                      touchesMinZ = false;
    bool                      touchesMaxZ = false;

    [[nodiscard]] bool TouchesBoundary() const noexcept {
        return touchesMinX || touchesMaxX || touchesMinZ || touchesMaxZ;
    }
};

/// 在给定窗口上做一次「① 纵向接地 → ②（可选）连通域洪泛 → ③ 同层悬挑传播」。
///
/// 顺序不可交换：洪泛必须在**悬挑传播之前**做 —— 那一刻 `reached` 恰好只表示"纵向接地"，
/// 而洪泛排除的正是"纵向接地"的体素（它们仍连着地面 ⇒ 属于静态承载部分，不是本次受影响的域）。
/// 若先做悬挑传播，被"拱效应"救回来的体素也会被排除，域就被切碎了。
///
/// `narrowByDomain = false` = **保守回退**：不按连通域收窄（= T49 之前的固定窗口语义）。
[[nodiscard]] CollapseProbe AnalyzeWindow(const CollapseSeed& seed, const CollapseSpec& spec,
                                          DensityRegion region, bool narrowByDomain) {
    CollapseProbe probe;
    probe.region = std::move(region);

    const int         sizeX = probe.region.sizeX;
    const int         sizeY = probe.region.sizeY;
    const int         sizeZ = probe.region.sizeZ;
    const std::size_t count = probe.region.values.size();
    probe.reached.assign(count, 0);
    probe.domain.assign(count, 0);

    std::vector<std::uint32_t> queue;
    queue.reserve(count / 2U + 16U);

    // ---- ① 纵向接地：自窗口底面起连续实心（底面视作地面 —— 其下已被证明是整块实心）----
    for (int z = 0; z < sizeZ; ++z) {
        for (int x = 0; x < sizeX; ++x) {
            bool chain = true;
            for (int y = 0; y < sizeY; ++y) {
                const std::size_t index = probe.region.Index(x, y, z);
                if (!IsSolidValue(probe.region.values[index])) {
                    chain = false;  // 出现空腔 ⇒ 其上的一切都失去了这条路
                    continue;
                }
                if (chain) {
                    probe.reached[index] = 1;
                    queue.push_back(static_cast<std::uint32_t>(index));
                }
            }
        }
    }

    // ---- ② 连通域洪泛（起点 = 被改动采样及其 6 邻域里"实心且未接地"的体素）----
    if (narrowByDomain) {
        const int seedMinX = std::max(seed.bounds.minX - 1, probe.region.minX);
        const int seedMaxX = std::min(seed.bounds.maxX + 1, probe.region.minX + sizeX - 1);
        const int seedMinY = std::max(seed.bounds.minY - 1, probe.region.minY);
        const int seedMaxY = std::min(seed.bounds.maxY + 1, probe.region.minY + sizeY - 1);
        const int seedMinZ = std::max(seed.bounds.minZ - 1, probe.region.minZ);
        const int seedMaxZ = std::min(seed.bounds.maxZ + 1, probe.region.minZ + sizeZ - 1);

        std::vector<std::uint32_t> frontier;
        for (int z = seedMinZ; z <= seedMaxZ; ++z) {
            for (int y = seedMinY; y <= seedMaxY; ++y) {
                for (int x = seedMinX; x <= seedMaxX; ++x) {
                    const std::size_t index = probe.region.Index(x - probe.region.minX, y - probe.region.minY,
                                                                 z - probe.region.minZ);
                    if (probe.domain[index] != 0 || probe.reached[index] != 0 ||
                        !IsSolidValue(probe.region.values[index])) {
                        continue;
                    }
                    probe.domain[index] = 1;
                    frontier.push_back(static_cast<std::uint32_t>(index));
                }
            }
        }

        const std::size_t strideZ = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY);
        for (std::size_t head = 0; head < frontier.size(); ++head) {
            const std::size_t index = frontier[head];
            const int         vz    = static_cast<int>(index / strideZ);
            const std::size_t rem   = index % strideZ;
            const int         vy    = static_cast<int>(rem / static_cast<std::size_t>(sizeX));
            const int         vx    = static_cast<int>(rem % static_cast<std::size_t>(sizeX));
            if (vx == 0) {
                probe.touchesMinX = true;
            }
            if (vx == sizeX - 1) {
                probe.touchesMaxX = true;
            }
            if (vz == 0) {
                probe.touchesMinZ = true;
            }
            if (vz == sizeZ - 1) {
                probe.touchesMaxZ = true;
            }

            const int nxc[6] = { vx - 1, vx + 1, vx, vx, vx, vx };
            const int nyc[6] = { vy, vy, vy - 1, vy + 1, vy, vy };
            const int nzc[6] = { vz, vz, vz, vz, vz - 1, vz + 1 };
            for (int dir = 0; dir < 6; ++dir) {
                if (nxc[dir] < 0 || nxc[dir] >= sizeX || nyc[dir] < 0 || nyc[dir] >= sizeY || nzc[dir] < 0 ||
                    nzc[dir] >= sizeZ) {
                    continue;
                }
                const std::size_t neighbor = probe.region.Index(nxc[dir], nyc[dir], nzc[dir]);
                if (probe.domain[neighbor] != 0 || probe.reached[neighbor] != 0 ||
                    !IsSolidValue(probe.region.values[neighbor])) {
                    continue;
                }
                probe.domain[neighbor] = 1;
                frontier.push_back(static_cast<std::uint32_t>(neighbor));
            }
        }
        probe.domainVoxels = frontier.size();
    } else {
        // 回退：候选集 = 全部"实心且未接地"的体素（不按连通域收窄）⇒ 与 T49 之前逐体素同结论。
        for (std::size_t index = 0; index < count; ++index) {
            if (probe.reached[index] == 0 && IsSolidValue(probe.region.values[index])) {
                probe.domain[index] = 1;
                ++probe.domainVoxels;
            }
        }
    }

    // ---- ③ 同层 4 邻域 BFS：最多走 floor(maxCantileverBlocks) 步（悬挑 / 拱效应）----
    const int   maxSteps = static_cast<int>(std::floor(std::max(spec.maxCantileverBlocks, 0.0F)));
    std::size_t head     = 0;
    for (int step = 0; step < maxSteps; ++step) {
        const std::size_t levelEnd = queue.size();
        if (head >= levelEnd) {
            break;
        }
        for (; head < levelEnd; ++head) {
            const std::size_t index = queue[head];
            const int z = static_cast<int>(index / (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY)));
            const std::size_t rem = index % (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY));
            const int         y   = static_cast<int>(rem / static_cast<std::size_t>(sizeX));
            const int         x   = static_cast<int>(rem % static_cast<std::size_t>(sizeX));

            const int nx[4] = { x - 1, x + 1, x, x };
            const int nz[4] = { z, z, z - 1, z + 1 };
            for (int dir = 0; dir < 4; ++dir) {
                if (nx[dir] < 0 || nx[dir] >= sizeX || nz[dir] < 0 || nz[dir] >= sizeZ) {
                    continue;
                }
                const std::size_t neighbor = probe.region.Index(nx[dir], y, nz[dir]);
                if (probe.reached[neighbor] != 0 || !IsSolidValue(probe.region.values[neighbor])) {
                    continue;
                }
                probe.reached[neighbor] = 1;
                queue.push_back(static_cast<std::uint32_t>(neighbor));
            }
        }
    }
    return probe;
}

/// 由 `unit.voxels` 重算**包围盒 / 质心 / 凸包点集 / 局部 AABB**。
///
/// 抽成公用（T49）是为了让 T50 的"在自身补丁上雕刻后重算"复用**同一段**几何口径 ——
/// 两处各写一份迟早会漂移，而"未触及区域顶点逐位不变"正是靠这段一致性成立的。
///
/// 前置条件：`unit.voxels` **非空**且已按 **(x, z, y) 升序**排好（下面的凸包点集按列扫描，依赖这个序）。
/// `keepCentroid = true` ⇒ **不重算质心**：局部坐标系保持不变 ⇒ 刚体与渲染网格**原地不动**
/// （T50 的口径：被雕刻时只有被挖掉的部分消失，整体不该整体平移）。
void RebuildUnitGeometry(CollapseUnit& unit, bool keepCentroid) {
    if (unit.voxels.empty()) {
        return;
    }

    unit.bounds = VoxelBounds { unit.voxels.front().x, unit.voxels.front().y, unit.voxels.front().z,
                                unit.voxels.front().x, unit.voxels.front().y, unit.voxels.front().z };
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        unit.bounds.minX = std::min(unit.bounds.minX, voxel.x);
        unit.bounds.minY = std::min(unit.bounds.minY, voxel.y);
        unit.bounds.minZ = std::min(unit.bounds.minZ, voxel.z);
        unit.bounds.maxX = std::max(unit.bounds.maxX, voxel.x);
        unit.bounds.maxY = std::max(unit.bounds.maxY, voxel.y);
        unit.bounds.maxZ = std::max(unit.bounds.maxZ, voxel.z);
    }

    if (!keepCentroid) {
        glm::dvec3 sum(0.0);
        for (const CollapseUnit::Voxel& voxel : unit.voxels) {
            sum += glm::dvec3(static_cast<double>(voxel.x) + 0.5, static_cast<double>(voxel.y) + 0.5,
                              static_cast<double>(voxel.z) + 0.5);
        }
        unit.centroid = sum / static_cast<double>(unit.voxels.size());
    }

    // 凸包点集：每个 (x, z) 列取 [minY, maxY] 的**包围盒 8 个角点**（比逐体素点少几个量级，
    // 且形状构建开销因此有上界）。凹形会被填平（ADR 0015 后果 1）。
    unit.hullPoints.clear();
    unit.hullPoints.reserve(unit.voxels.size() >= 8 ? 8U * 8U : 24U);
    std::size_t columnStart = 0;
    while (columnStart < unit.voxels.size()) {
        std::size_t columnEnd = columnStart;
        while (columnEnd + 1 < unit.voxels.size() && unit.voxels[columnEnd + 1].x == unit.voxels[columnStart].x &&
               unit.voxels[columnEnd + 1].z == unit.voxels[columnStart].z) {
            ++columnEnd;
        }
        const int columnX    = unit.voxels[columnStart].x;
        const int columnZ    = unit.voxels[columnStart].z;
        const int columnMinY = unit.voxels[columnStart].y;    // 已按 y 升序
        const int columnMaxY = unit.voxels[columnEnd].y + 1;  // 体素上表面
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dz = 0; dz <= 1; ++dz) {
                for (int dy = 0; dy <= 1; ++dy) {
                    const double px = static_cast<double>(columnX + dx);
                    const double py = static_cast<double>(dy == 0 ? columnMinY : columnMaxY);
                    const double pz = static_cast<double>(columnZ + dz);
                    unit.hullPoints.push_back(static_cast<float>(px - unit.centroid.x));
                    unit.hullPoints.push_back(static_cast<float>(py - unit.centroid.y));
                    unit.hullPoints.push_back(static_cast<float>(pz - unit.centroid.z));
                }
            }
        }
        columnStart = columnEnd + 1;
    }

    // `hullPoints` 的**局部 AABB**（T46）：保留残骸的"唤醒判据"用的盒（见 `UnitWorldAabb`）。
    if (!unit.hullPoints.empty()) {
        glm::vec3 low(unit.hullPoints[0], unit.hullPoints[1], unit.hullPoints[2]);
        glm::vec3 high = low;
        for (std::size_t i = 3; i + 2 < unit.hullPoints.size(); i += 3) {
            const glm::vec3 point(unit.hullPoints[i], unit.hullPoints[i + 1], unit.hullPoints[i + 2]);
            low  = glm::min(low, point);
            high = glm::max(high, point);
        }
        unit.hullMinLocal = low;
        unit.hullMaxLocal = high;
    }
}

/// 把一个连通分量按**材质一致性**切成子块（T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策四）。
///
/// 判据：**同一材质槽位**（`kNoMaterialSlot` 归入槽位 0，与 `ComputeUnitPhysics` 的兜底同口径）
/// 且 **6 邻域连通** ⇒ 一个子块。扫描序 = 体素的 **x → z → y** 升序（`unit.voxels` 已是该序）⇒ 划分**确定**（红线 7）。
///
/// 为什么需要：`rigidDebris` 曾是一个整体一个 bool ⇒ "一条长条里岩石与泥土同时存在"时必有一半表现不对
/// （岩要形状不变、土要下坠到支撑）。拆成子块后各自按自己的材质表现，而"整体"的定义没变
/// —— 子块只是原分量内**材质一致**的部分。
[[nodiscard]] std::vector<CollapseUnit> SplitUnitByMaterial(const DigVolumeWorld& volumes, CollapseUnit unit) {
    std::vector<CollapseUnit> subs;
    const std::size_t         count = unit.voxels.size();
    if (count == 0) {
        return subs;
    }

    std::vector<std::uint8_t> slot(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t raw = volumes.SampleMaterialSlot(unit.voxels[i].x, unit.voxels[i].y, unit.voxels[i].z);
        slot[i]                = (raw == kNoMaterialSlot) ? 0U : raw;
    }
    // 快路径：整块同材质（绝大多数情况）⇒ 原样返回，连重算都省掉（几何与划分前**逐位相同**）。
    const bool uniform = std::all_of(slot.begin(), slot.end(), [&slot](std::uint8_t value) { return value == slot.front(); });
    if (uniform) {
        subs.push_back(std::move(unit));
        return subs;
    }

    // 体素 → 下标（6 邻域查找用；`Voxel` 自带确定序的 `operator<`）。
    std::map<CollapseUnit::Voxel, std::size_t> lookup;
    for (std::size_t i = 0; i < count; ++i) {
        lookup.emplace(unit.voxels[i], i);
    }

    std::vector<int>          assigned(count, -1);
    std::vector<std::size_t>  frontier;
    for (std::size_t start = 0; start < count; ++start) {
        if (assigned[start] >= 0) {
            continue;
        }
        const std::uint8_t here = slot[start];
        CollapseUnit       sub;
        frontier.clear();
        frontier.push_back(start);
        assigned[start] = 1;
        for (std::size_t head = 0; head < frontier.size(); ++head) {
            const std::size_t i  = frontier[head];
            const int         vx = unit.voxels[i].x;
            const int         vy = unit.voxels[i].y;
            const int         vz = unit.voxels[i].z;
            sub.voxels.push_back(unit.voxels[i]);

            const int nxc[6] = { vx - 1, vx + 1, vx, vx, vx, vx };
            const int nyc[6] = { vy, vy, vy - 1, vy + 1, vy, vy };
            const int nzc[6] = { vz, vz, vz, vz, vz - 1, vz + 1 };
            for (int dir = 0; dir < 6; ++dir) {
                const auto found = lookup.find(CollapseUnit::Voxel { nxc[dir], nyc[dir], nzc[dir] });
                if (found == lookup.end()) {
                    continue;
                }
                const std::size_t j = found->second;
                if (assigned[j] >= 0 || slot[j] != here) {
                    continue;
                }
                assigned[j] = 1;
                frontier.push_back(j);
            }
        }
        std::sort(sub.voxels.begin(), sub.voxels.end(), VoxelLessByColumn);
        RebuildUnitGeometry(sub, /*keepCentroid*/ false);
        subs.push_back(std::move(sub));
    }
    return subs;
}

}  // namespace

float CollapsePose::TiltDegrees() const noexcept {
    const glm::vec3 up     = rotation * glm::vec3(0.0F, 1.0F, 0.0F);
    const float     cosine = std::min(1.0F, std::max(-1.0F, up.y));
    return std::acos(cosine) * 57.29577951308232F;  // 弧度 → 度（不引 glm 的三角函数头，避免多余依赖）
}

void UnitWorldAabb(const CollapseUnit& unit, const CollapsePose& pose, glm::dvec3& outMin, glm::dvec3& outMax) noexcept {
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) != 0 ? unit.hullMaxLocal.x : unit.hullMinLocal.x,
                              (corner & 2) != 0 ? unit.hullMaxLocal.y : unit.hullMinLocal.y,
                              (corner & 4) != 0 ? unit.hullMaxLocal.z : unit.hullMinLocal.z);
        const glm::dvec3 world = pose.position + glm::dvec3(pose.rotation * local);
        if (corner == 0) {
            outMin = outMax = world;
            continue;
        }
        outMin = glm::min(outMin, world);
        outMax = glm::max(outMax, world);
    }
}

CollapseWriteback WritebackCollapseUnit(DigVolumeWorld& volumes, const CollapseUnit& unit, const CollapsePose& pose) {
    CollapseWriteback result;

    // ---- 目标区域 = 原始体素范围 ∪ 落定后的世界包围盒（把局部 AABB 的 8 个角旋转后取包）----
    const glm::dvec3 localMin(static_cast<double>(unit.bounds.minX) - unit.centroid.x,
                              static_cast<double>(unit.bounds.minY) - unit.centroid.y,
                              static_cast<double>(unit.bounds.minZ) - unit.centroid.z);
    const glm::dvec3 localMax(static_cast<double>(unit.bounds.maxX + 1) - unit.centroid.x,
                              static_cast<double>(unit.bounds.maxY + 1) - unit.centroid.y,
                              static_cast<double>(unit.bounds.maxZ + 1) - unit.centroid.z);
    double           finalMin[3] = { 0.0, 0.0, 0.0 };
    double           finalMax[3] = { 0.0, 0.0, 0.0 };
    for (int corner = 0; corner < 8; ++corner) {
        const glm::dvec3 local(corner & 1 ? localMax.x : localMin.x, corner & 2 ? localMax.y : localMin.y,
                               corner & 4 ? localMax.z : localMin.z);
        const glm::vec3  rotated = pose.rotation * glm::vec3(local);
        const glm::dvec3 world(pose.position.x + static_cast<double>(rotated.x),
                               pose.position.y + static_cast<double>(rotated.y),
                               pose.position.z + static_cast<double>(rotated.z));
        if (corner == 0) {
            finalMin[0] = finalMax[0] = world.x;
            finalMin[1] = finalMax[1] = world.y;
            finalMin[2] = finalMax[2] = world.z;
            continue;
        }
        finalMin[0] = std::min(finalMin[0], world.x);
        finalMin[1] = std::min(finalMin[1], world.y);
        finalMin[2] = std::min(finalMin[2], world.z);
        finalMax[0] = std::max(finalMax[0], world.x);
        finalMax[1] = std::max(finalMax[1], world.y);
        finalMax[2] = std::max(finalMax[2], world.z);
    }

    const int minX = std::min(unit.bounds.minX, static_cast<int>(std::floor(finalMin[0]))) - 1;
    const int maxX = std::max(unit.bounds.maxX, static_cast<int>(std::ceil(finalMax[0]))) + 1;
    const int minY = std::min(unit.bounds.minY, static_cast<int>(std::floor(finalMin[1]))) - 1;
    const int maxY = std::max(unit.bounds.maxY, static_cast<int>(std::ceil(finalMax[1]))) + 1;
    const int minZ = std::min(unit.bounds.minZ, static_cast<int>(std::floor(finalMin[2]))) - 1;
    const int maxZ = std::max(unit.bounds.maxZ, static_cast<int>(std::ceil(finalMax[2]))) + 1;

    const int sizeX = maxX - minX + 1;
    // T46 / ADR 0017：**散体**需要向下的沉降余量（见 `kSettleMaxDropBlocks`）；刚性保持形状、不扩也不沉降。
    const int settleBottom = unit.rigidDebris ? minY : (minY - kSettleMaxDropBlocks);
    const int sizeY = maxY - settleBottom + 1;
    const int sizeZ = maxZ - minZ + 1;
    DensityRegion region = volumes.ReadDensityRegion(minX, settleBottom, minZ, sizeX, sizeY, sizeZ);

    // 放置结果（与 `unit.voxels` 逐一对应；`hasMaterial` = 补丁提供了该体素原本的材质）。
    // 为什么先只记坐标：**沉降会改坐标**，故材质写入与"标脏块"都必须留到沉降之后。
    struct Placed {
        int          x = 0;
        int          y = 0;
        int          z = 0;
        std::uint8_t material    = kNoMaterialSlot;
        bool         hasMaterial = false;
    };
    std::vector<Placed> placed;
    placed.reserve(unit.voxels.size());

    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        // 体素中心（局部）→ 世界 → 最近的格（体素格的"最小角" = 中心 − 0.5）。
        const glm::vec3 localCenter(static_cast<float>(static_cast<double>(voxel.x) + 0.5 - unit.centroid.x),
                                    static_cast<float>(static_cast<double>(voxel.y) + 0.5 - unit.centroid.y),
                                    static_cast<float>(static_cast<double>(voxel.z) + 0.5 - unit.centroid.z));
        const glm::vec3 rotated = pose.rotation * localCenter;
        const double    worldX  = pose.position.x + static_cast<double>(rotated.x);
        const double    worldY  = pose.position.y + static_cast<double>(rotated.y);
        const double    worldZ  = pose.position.z + static_cast<double>(rotated.z);
        const int       baseX   = static_cast<int>(std::lround(worldX - 0.5));
        const int       baseY   = static_cast<int>(std::lround(worldY - 0.5));
        const int       baseZ   = static_cast<int>(std::lround(worldZ - 0.5));

        // T42：该体素**原本的材质**（补丁提供）—— 否则残骸落地后会按"落点那一列的地表材质"重新着色。
        Placed cell;
        if (!unit.patchMaterial.empty()) {
            const int px = voxel.x - unit.patchBounds.minX;
            const int py = voxel.y - unit.patchBounds.minY;
            const int pz = voxel.z - unit.patchBounds.minZ;
            if (px >= 0 && px < unit.patchSizeX && py >= 0 && py < unit.patchSizeY && pz >= 0 &&
                pz < unit.patchSizeZ) {
                cell.material    = unit.patchMaterial[PatchIndex(unit, px, py, pz)];
                cell.hasMaterial = true;
            }
        }

        bool done = false;
        for (const int(&offset)[3] : kPlacementOffsets) {
            const int x = baseX + offset[0];
            const int y = baseY + offset[1];
            const int z = baseZ + offset[2];
            if (!CoveredByVolumes(volumes, x, y, z)) {
                continue;
            }
            const int rx = x - minX;
            const int ry = y - settleBottom;
            const int rz = z - minZ;
            if (!region.Contains(rx, ry, rz) || region.values[region.Index(rx, ry, rz)] < 0) {
                continue;  // 已被占（本次前面的体素 / 未塌的实心）⇒ 试下一个候选
            }
            region.values[region.Index(rx, ry, rz)] = static_cast<std::int8_t>(-kDensityUnitsPerBlock);
            cell.x = x;
            cell.y = y;
            cell.z = z;
            placed.push_back(cell);
            ++result.writtenVoxels;
            done = true;
            break;
        }
        if (!done) {
            ++result.droppedVoxels;
        }
    }

    // ---- T46 / ADR 0017 决策三：**散体接地沉降**（刚性不做 ⇒ 形状不变）----
    //
    // 为什么需要：回写按"最近格"落位，若落点**下方是空腔**就会留下"悬空的泥土"（项目所有者明确不允许）。
    // 做法：把"下方为空"的体素沿本列下落 1 格、直到下方有支撑（地形 / 已沉降的体素）。
    // 确定性：`unit.voxels` 的序是 (x, z, y) 升序 ⇒ 同一 (x, z) 列内**自下而上** ⇒
    //         处理到某体素时，它下方要么是地形、要么是**已沉降完**的体素 ⇒ 一次扫描即收敛。
    if (!unit.rigidDebris) {
        for (Placed& cell : placed) {
            const int originalY = cell.y;
            int       drop      = 0;
            while (drop < kSettleMaxDropBlocks) {
                const int below = cell.y - 1;
                if (below < settleBottom) {
                    break;  // 已降到回写区域之外 ⇒ 交下面的"仍悬空 ⇒ 清除"扫描处理
                }
                const std::size_t belowIndex = region.Index(cell.x - minX, below - settleBottom, cell.z - minZ);
                if (region.values[belowIndex] < 0) {
                    break;  // 下方有支撑
                }
                // **先把当前格腾空、再落到下一格** —— 否则会在列里留下一串"实心轨迹"
                // （那些格本该是空的；有轨迹的话，同列的其它体素会被假支撑挡住）。
                region.values[region.Index(cell.x - minX, cell.y - settleBottom, cell.z - minZ)] =
                    static_cast<std::int8_t>(kDensityMax);
                region.values[belowIndex] = static_cast<std::int8_t>(-kDensityUnitsPerBlock);
                cell.y                    = below;
                ++drop;
            }
            if (cell.y != originalY) {
                ++result.settledVoxels;  // 该体素发生了沉降（计**体素数**，不是下落格数）
            }
        }

        // ---- T51（2026-09-28）：**仍悬空的散体一律清除**（所有者指定："悬空的小土块可以直接删除"）----
        //
        // 为什么需要：沉降有 32 格上限，而"已经降到回写区域之外"（`below < settleBottom`，即 32 格内
        // 没有任何采样可查）过去**既不计数也不告警** ⇒ 落点下方 32+ 格全空时会**静默留下悬空体素**，
        // 这正是所有者实测到的"悬空的泥土元素"。
        // 做法：**自下而上**（y 升序）扫一遍，凡"下方为空 / 下方已在区域之外"的体素**直接从回写结果里去掉**
        // —— 自下而上保证"删掉一个后，其上方的判定用的是最新状态"（同列连锁悬空会被一并清除），
        // 一次扫描即收敛。区域之外（`below < settleBottom`）**也算悬空**：32 格内都没有采样，
        // 不应按"接地"处理（与"支撑检查把邻域底面视作地面"不同 —— 那里的底面之外已被证明是整块实心）。
        std::sort(placed.begin(), placed.end(), [](const Placed& left, const Placed& right) {
            if (left.y != right.y) {
                return left.y < right.y;
            }
            if (left.x != right.x) {
                return left.x < right.x;
            }
            return left.z < right.z;
        });
        std::vector<Placed> kept;
        kept.reserve(placed.size());
        for (const Placed& cell : placed) {
            const int below = cell.y - 1;
            bool      grounded = false;
            if (below >= settleBottom) {
                grounded = region.values[region.Index(cell.x - minX, below - settleBottom, cell.z - minZ)] < 0;
            }
            if (grounded) {
                kept.push_back(cell);
                continue;
            }
            region.values[region.Index(cell.x - minX, cell.y - settleBottom, cell.z - minZ)] =
                static_cast<std::int8_t>(kDensityMax);  // 清除：不许留在空中
            ++result.removedFloatingVoxels;
        }
        placed = std::move(kept);
        // 被清掉的体素不算"写回成功"（否则守恒记账会虚高）。
        result.writtenVoxels -= result.removedFloatingVoxels;
    }

    int changedMin[3] = { maxX, maxY, maxZ };
    int changedMax[3] = { minX, minY, minZ };
    const auto markChanged = [&](int x, int y, int z) {
        changedMin[0] = std::min(changedMin[0], x);
        changedMin[1] = std::min(changedMin[1], y);
        changedMin[2] = std::min(changedMin[2], z);
        changedMax[0] = std::max(changedMax[0], x);
        changedMax[1] = std::max(changedMax[1], y);
        changedMax[2] = std::max(changedMax[2], z);
    };

    // 沉降之后：标脏块 + 把材质写到**最终**落点（顺序与 `unit.voxels` 一致 ⇒ 确定）。
    for (const Placed& cell : placed) {
        markChanged(cell.x, cell.y, cell.z);
        if (cell.hasMaterial) {
            volumes.SetMaterialSlot(cell.x, cell.y, cell.z, cell.material);
        }
    }

    volumes.WriteDensityRegion(region);

    result.bounds = VoxelBounds { changedMin[0], changedMin[1], changedMin[2], changedMax[0], changedMax[1],
                                  changedMax[2] };
    const BlockCoord dirtyMin { BlockIndexOfWorld(changedMin[0]), BlockIndexOfWorld(changedMin[1]),
                                BlockIndexOfWorld(changedMin[2]) };
    const BlockCoord dirtyMax { BlockIndexOfWorld(changedMax[0]), BlockIndexOfWorld(changedMax[1]),
                                BlockIndexOfWorld(changedMax[2]) };
    for (int bz = dirtyMin.z; bz <= dirtyMax.z; ++bz) {
        for (int by = dirtyMin.y; by <= dirtyMax.y; ++by) {
            for (int bx = dirtyMin.x; bx <= dirtyMax.x; ++bx) {
                result.dirty.push_back(BlockCoord { bx, by, bz });
            }
        }
    }
    return result;
}

CollapsePlan ApplyCollapse(DigVolumeWorld& volumes, const CollapseSeed& seed, const CollapseSpec& spec,
                           std::size_t maxUnits) {
    CollapsePlan plan;
    if (!spec.enabled || seed.bounds.Empty() || maxUnits == 0) {
        return plan;
    }

    // ---- 邻域（T49 / ADR 0018 决策一）：**以被改动采样为起点的实心连通域**，而不是固定窗口 ----
    //
    // 起始窗口仍是 T30 的口径（被改动采样 ± (悬挑 + 1) 格；`neighborhood_margin_blocks` 可再额外外扩），
    // 但连通域一旦**触到窗口的水平边界**就把窗口**翻倍扩张** —— 固定窗口下"距破坏点超过窗口的远端中段"
    // 从不进入任何一次求解，于是长条两端支点都被破坏后**中段永远悬空**（BUG1）。
    // 竖直范围（块级填充分类）与支撑判据**一字未改**；采样数超 `kMaxRegionSamples` 或扩张次数用尽时
    // **告警 + 保守回退固定窗口**（显式例外，切换条件 = 持久结构图落地）。
    const int reach       = static_cast<int>(std::ceil(std::max(spec.maxCantileverBlocks, 0.0F))) + 1;
    const int marginWorld = std::max(spec.neighborhoodMarginBlocks, 0) * kVolumeBlockSize;
    const int baseHalf    = reach + marginWorld;

    if (volumes.Blocks().empty()) {
        return plan;
    }

    // 体积的块级范围：竖直推导用（向上 / 向下扫块），窗口扩张的终止上界也用它。
    int  volumeMinBlockX = 0;
    int  volumeMaxBlockX = 0;
    int  volumeMinBlockY = 0;
    int  volumeMaxBlockY = 0;
    int  volumeMinBlockZ = 0;
    int  volumeMaxBlockZ = 0;
    bool firstBlock      = true;
    for (const auto& entry : volumes.Blocks()) {
        const BlockCoord& coord = entry.first;
        if (firstBlock) {
            volumeMinBlockX = volumeMaxBlockX = coord.x;
            volumeMinBlockY = volumeMaxBlockY = coord.y;
            volumeMinBlockZ = volumeMaxBlockZ = coord.z;
            firstBlock      = false;
            continue;
        }
        volumeMinBlockX = std::min(volumeMinBlockX, coord.x);
        volumeMaxBlockX = std::max(volumeMaxBlockX, coord.x);
        volumeMinBlockY = std::min(volumeMinBlockY, coord.y);
        volumeMaxBlockY = std::max(volumeMaxBlockY, coord.y);
        volumeMinBlockZ = std::min(volumeMinBlockZ, coord.z);
        volumeMaxBlockZ = std::max(volumeMaxBlockZ, coord.z);
    }

    // 由当前的**窗口水平范围**算出完整窗口：**竖直 = 自种子块向下到"最低的非全实心块"、
    // 向上到"最高的非全空块"**（T30 口径不变：整块实心 / 整块空的块都不可能参与支撑判定，可整块排除）。
    int         windowMinX    = seed.bounds.minX - baseHalf;
    int         windowMaxX    = seed.bounds.maxX + baseHalf;
    int         windowMinZ    = seed.bounds.minZ - baseHalf;
    int         windowMaxZ    = seed.bounds.maxZ + baseHalf;
    int         windowMinY    = 0;
    int         windowMaxY    = 0;
    std::size_t windowSamples = 0;
    const auto  computeWindow = [&]() {
        const int horizontalMinX = BlockIndexOfWorld(windowMinX);
        const int horizontalMaxX = BlockIndexOfWorld(windowMaxX);
        const int horizontalMinZ = BlockIndexOfWorld(windowMinZ);
        const int horizontalMaxZ = BlockIndexOfWorld(windowMaxZ);

        int lowBlockY  = BlockIndexOfWorld(seed.bounds.minY);
        int highBlockY = BlockIndexOfWorld(seed.bounds.maxY);
        for (int bz = horizontalMinZ; bz <= horizontalMaxZ; ++bz) {
            for (int bx = horizontalMinX; bx <= horizontalMaxX; ++bx) {
                for (int by = volumeMinBlockY; by <= volumeMaxBlockY; ++by) {
                    const BlockFill fill = volumes.FillOf(BlockCoord { bx, by, bz });
                    if (fill != BlockFill::Solid) {
                        lowBlockY = std::min(lowBlockY, by);
                    }
                    if (fill != BlockFill::Air) {
                        highBlockY = std::max(highBlockY, by);
                    }
                }
            }
        }
        windowMinY = BlockOriginBlocks(lowBlockY);
        windowMaxY = BlockOriginBlocks(highBlockY) + kVolumeBlockSize;
        windowSamples = static_cast<std::size_t>(windowMaxX - windowMinX + 1) *
                        static_cast<std::size_t>(windowMaxY - windowMinY + 1) *
                        static_cast<std::size_t>(windowMaxZ - windowMinZ + 1);
    };

    const auto readWindow = [&]() {
        return volumes.ReadDensityRegion(windowMinX, windowMinY, windowMinZ, windowMaxX - windowMinX + 1,
                                         windowMaxY - windowMinY + 1, windowMaxZ - windowMinZ + 1);
    };

    const int volumeMinWorldX = BlockOriginBlocks(volumeMinBlockX);
    const int volumeMaxWorldX = BlockOriginBlocks(volumeMaxBlockX) + kVolumeBlockSize;
    const int volumeMinWorldZ = BlockOriginBlocks(volumeMinBlockZ);
    const int volumeMaxWorldZ = BlockOriginBlocks(volumeMaxBlockZ) + kVolumeBlockSize;

    // 终止条件（三个，任一命中即退出）：① 连通域被窗口完整包住 ⇒ 收窄成立（正常路径）；
    // ② 采样数超上限 / ③ 窗口已覆盖整个体积的水平范围但连通域仍触界 ⇒ **保守回退固定窗口**。
    // 扩张**只发生在触界的那些侧**（每侧把"离种子的距离"翻倍）：细长结构因此保持细长的窗口，
    // 每轮分析的成本与结构形状成正比（不加"半宽上限"之类的预判 —— 连通域可能一直延伸到数据边界）。
    CollapseProbe probe;
    bool          narrowed      = false;
    std::size_t   probedSamples = 0;  // 洪泛的**累计**采样数：上界 = `kMaxRegionSamples`（见下方 WARN）
    for (int iteration = 0; iteration < kMaxExpandIterations; ++iteration) {
        computeWindow();
        // 上界按**累计**算（而不只是"单个窗口不超"）：每次扩张都要把整个窗口重扫一遍，
        // 只限单窗的话总成本可达上限的两倍以上。累计有界 ⇒ 这条路径的单帧成本有界（不冻结）。
        if (windowSamples > kMaxRegionSamples || probedSamples + windowSamples > kMaxRegionSamples) {
            break;
        }
        probedSamples += windowSamples;
        probe              = AnalyzeWindow(seed, spec, readWindow(), /*narrowByDomain*/ true);
        plan.regionSamples = windowSamples;
        if (probe.domainVoxels == 0 || !probe.TouchesBoundary()) {
            narrowed = true;  // 连通域被完整包住（含"没有脱离支撑的实心"这一情形）⇒ 本次求解成立
            break;
        }
        // 触界：窗口已经把整个体积的水平范围包进来了 ⇒ 结构真的延伸到数据边界，收窄到此为止。
        if (windowMinX <= volumeMinWorldX && windowMaxX >= volumeMaxWorldX && windowMinZ <= volumeMinWorldZ &&
            windowMaxZ >= volumeMaxWorldZ) {
            break;
        }
        if (probe.touchesMinX) {
            windowMinX = seed.bounds.minX - 2 * (seed.bounds.minX - windowMinX);
        }
        if (probe.touchesMaxX) {
            windowMaxX = seed.bounds.maxX + 2 * (windowMaxX - seed.bounds.maxX);
        }
        if (probe.touchesMinZ) {
            windowMinZ = seed.bounds.minZ - 2 * (seed.bounds.minZ - windowMinZ);
        }
        if (probe.touchesMaxZ) {
            windowMaxZ = seed.bounds.maxZ + 2 * (windowMaxZ - seed.bounds.maxZ);
        }
    }

    if (!narrowed) {
        // ---- 保守回退（T49 的显式例外）：现行**固定窗口**，不按连通域收窄 ----
        windowMinX = seed.bounds.minX - baseHalf;
        windowMaxX = seed.bounds.maxX + baseHalf;
        windowMinZ = seed.bounds.minZ - baseHalf;
        windowMaxZ = seed.bounds.maxZ + baseHalf;
        computeWindow();
        plan.regionSamples = windowSamples;
        if (windowSamples > kMaxRegionSamples) {
            VX_LOG_WARN("塌落跳过：支撑检查邻域过大（%d × %d × %d = %zu 个采样，上限 %zu）",
                        windowMaxX - windowMinX + 1, windowMaxY - windowMinY + 1, windowMaxZ - windowMinZ + 1,
                        windowSamples, kMaxRegionSamples);
            return plan;
        }
        probe = AnalyzeWindow(seed, spec, readWindow(), /*narrowByDomain*/ false);
        VX_LOG_WARN("塌落：支撑求解的连通域超出上限（起始半宽 %d 格、累计采样上限 %zu / 扩张上限 %d 次）"
                    "⇒ 按 ADR 0018 决策一**保守回退固定窗口**（本次可能漏掉远端中段；切换条件 = 持久结构图）",
                    baseHalf, kMaxRegionSamples, kMaxExpandIterations);
    }
    plan.domainVoxels   = probe.domainVoxels;
    plan.domainNarrowed = narrowed;

    // ---- ① 支撑体素 / ② 连通域：都由 `AnalyzeWindow` 算完（T49）----
    //
    // `reached` = "载荷能沿实心一路传到地底"（**纵向**：自窗口底面起连续实心 ⇒ 接地）**再沿同一高度层
    // 的实心连通横向传播 `floor(maxCantileverBlocks)` 步**（= 悬挑 / 拱效应）。判据与 T29 / T33 一字未改。
    // `domain` = 以被改动采样为起点的**实心连通域**（T49 的 scope）：脱离纵向接地的实心体素，
    // 6 邻域连通到本次破坏处 —— 只有它才可能在这次破坏中失去支撑（静态承载部分不进来，域因此有界）。
    const std::vector<std::uint8_t>& reached = probe.reached;
    const std::vector<std::uint8_t>& domain  = probe.domain;
    DensityRegion&                   region  = probe.region;  // 可变：抽出 / 清除要把它写回体积
    const int                        minX    = region.minX;
    const int                        minY    = region.minY;
    const int                        minZ    = region.minZ;
    const int                        sizeX   = region.sizeX;
    const int                        sizeY   = region.sizeY;
    const int                        sizeZ   = region.sizeZ;

    // ---- ② 失去支撑的实心体素按 6 邻域连通分量分组（scope = **连通域 ∩ 无支撑**）----
    //
    // 每个分量 = 一个"整体"（倒塌时一起动）。BFS 采用**确定的扫描 / 邻接顺序**（x → y → z，±x/±y/±z），
    // 故同一输入永远得到同一分组（红线 7）。T49 起多一个条件：**必须在连通域内** ——
    // 窗口里"与本次破坏无关"的其它悬空结构不该被这次爆炸连带塌掉（scope = 可能受本次影响的结构）。
    std::vector<std::uint8_t>  visited(region.values.size(), 0);
    std::vector<CollapseUnit>  found;
    std::vector<std::uint32_t> queue;

    for (int z = 0; z < sizeZ; ++z) {
        for (int y = 0; y < sizeY; ++y) {
            for (int x = 0; x < sizeX; ++x) {
                const std::size_t start = region.Index(x, y, z);
                if (visited[start] != 0 || reached[start] != 0 || domain[start] == 0 ||
                    !IsSolidValue(region.values[start])) {
                    continue;
                }

                CollapseUnit unit;
                queue.clear();
                queue.push_back(static_cast<std::uint32_t>(start));
                visited[start] = 1;

                std::size_t head2 = 0;
                while (head2 < queue.size()) {
                    const std::size_t index = queue[head2++];
                    const int vz =
                        static_cast<int>(index / (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY)));
                    const std::size_t rem =
                        index % (static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY));
                    const int vy = static_cast<int>(rem / static_cast<std::size_t>(sizeX));
                    const int vx = static_cast<int>(rem % static_cast<std::size_t>(sizeX));

                    unit.voxels.push_back(CollapseUnit::Voxel { minX + vx, minY + vy, minZ + vz });

                    // 6 邻域（确定顺序：-x, +x, -y, +y, -z, +z）。
                    const int nxc[6] = { vx - 1, vx + 1, vx, vx, vx, vx };
                    const int nyc[6] = { vy, vy, vy - 1, vy + 1, vy, vy };
                    const int nzc[6] = { vz, vz, vz, vz, vz - 1, vz + 1 };
                    for (int dir = 0; dir < 6; ++dir) {
                        if (nxc[dir] < 0 || nxc[dir] >= sizeX || nyc[dir] < 0 || nyc[dir] >= sizeY || nzc[dir] < 0 ||
                            nzc[dir] >= sizeZ) {
                            continue;
                        }
                        const std::size_t neighbor = region.Index(nxc[dir], nyc[dir], nzc[dir]);
                        if (visited[neighbor] != 0 || reached[neighbor] != 0 || domain[neighbor] == 0 ||
                            !IsSolidValue(region.values[neighbor])) {
                            continue;
                        }
                        visited[neighbor] = 1;
                        queue.push_back(static_cast<std::uint32_t>(neighbor));
                    }
                }

                // 体素按 (x, z, y) 排序（同列连续 ⇒ 凸包点集按列扫描成立），再统一重算几何。
                std::sort(unit.voxels.begin(), unit.voxels.end(), VoxelLessByColumn);
                RebuildUnitGeometry(unit, /*keepCentroid*/ false);
                plan.unsupportedVoxels += unit.voxels.size();
                found.push_back(std::move(unit));
            }
        }
    }

    if (found.empty()) {
        return plan;
    }

    const TerrainMaterialTable& materials = volumes.Materials();

    // ---- T50 / ADR 0018 决策四：把每个连通分量按**材质一致性**再切成子块 ----
    //
    // 一个子块 = 一种材质 ⇒ 各自按自己的材质决定落地行为（岩保持形状、土回写融合并接地沉降），
    // 于是"一条长条里岩石与泥土同时存在"两侧都对。子块数上界由后面的 `maxUnits`（超出者保持原状并计数）
    // 与小碎片清除共同承担 ⇒ 不会出现"刚体名额被碎块吃光、新结构不再倒塌"。
    {
        std::vector<CollapseUnit> subunits;
        subunits.reserve(found.size());
        for (CollapseUnit& unit : found) {
            std::vector<CollapseUnit> parts = SplitUnitByMaterial(volumes, std::move(unit));
            for (CollapseUnit& part : parts) {
                subunits.push_back(std::move(part));
            }
        }
        found = std::move(subunits);
    }

    const int regionMaxX = minX + sizeX - 1;
    const int regionMaxY = minY + sizeY - 1;
    const int regionMaxZ = minZ + sizeZ - 1;
    int changedMinX = regionMaxX;
    int changedMinY = regionMaxY;
    int changedMinZ = regionMaxZ;
    int changedMaxX = minX;
    int changedMaxY = minY;
    int changedMaxZ = minZ;
    const auto accumulateChanged = [&](const CollapseUnit& unit) {
        changedMinX = std::min(changedMinX, unit.bounds.minX);
        changedMinY = std::min(changedMinY, unit.bounds.minY);
        changedMinZ = std::min(changedMinZ, unit.bounds.minZ);
        changedMaxX = std::max(changedMaxX, unit.bounds.maxX);
        changedMaxY = std::max(changedMaxY, unit.bounds.maxY);
        changedMaxZ = std::max(changedMaxZ, unit.bounds.maxZ);
    };

    // ---- ③a 小碎片清除（T43 / ADR 0016 决策三）----
    //
    // 先做、且**不受 `maxUnits` 限制**：否则"碎渣"会因为刚体名额用尽而永远悬在空中（那正是要避免的观感）。
    // 守卫：分量只要含 `indestructible` 材质就**不删**（ADR 0013 的完整"不可破坏"语义尚未实现，
    // 故这里只做"不清除"这一步）。
    if (spec.debrisDeleteMaxVoxels > 0) {
        const std::size_t limit = static_cast<std::size_t>(spec.debrisDeleteMaxVoxels);
        for (auto unit = found.begin(); unit != found.end();) {
            if (unit->voxels.size() > limit || ContainsIndestructibleMaterial(volumes, materials, *unit)) {
                ++unit;
                continue;
            }
            for (const CollapseUnit::Voxel& voxel : unit->voxels) {
                region.values[region.Index(voxel.x - minX, voxel.y - minY, voxel.z - minZ)] =
                    static_cast<std::int8_t>(kDensityMax);  // 清除 = 变空（"当炸没了"）
            }
            accumulateChanged(*unit);
            plan.deletedUnits += 1;
            plan.deletedVoxels += unit->voxels.size();
            unit = found.erase(unit);
        }
    }

    // ---- ③b 抽出（最多 maxUnits 个；超出的保持原状并计数）----
    std::sort(found.begin(), found.end(), UnitPreferred);
    const std::size_t extracted = std::min(found.size(), maxUnits);
    plan.skippedUnits = found.size() - extracted;

    for (std::size_t i = 0; i < extracted; ++i) {
        CollapseUnit& unit = found[i];
        // T42：先抓"体素补丁"（密度 + 材质）——它必须在下面的清空**之前**读，否则拿到的是空密度。
        CaptureUnitPatch(volumes, unit);
        // T43：再按补丁里的材质算质量 / 摩擦 / 弹性 / 落地口径，并算这次爆炸的爆心冲量。
        ComputeUnitPhysics(unit, materials);
        ComputeUnitImpulse(unit, seed, spec);
        for (const CollapseUnit::Voxel& voxel : unit.voxels) {
            region.values[region.Index(voxel.x - minX, voxel.y - minY, voxel.z - minZ)] =
                static_cast<std::int8_t>(kDensityMax);  // 抽出 = 变空
        }
        accumulateChanged(unit);
        plan.units.push_back(unit);
    }

    // 边界样本与邻块共享 ⇒ 必须经 `WriteDensityRegion` 写回（直改数组会漏掉邻块的副本）。
    volumes.WriteDensityRegion(region);

    // 脏块 = 被抽出体素所覆盖的块范围（不做整邻域重网格）。
    const BlockCoord dirtyMin { BlockIndexOfWorld(changedMinX), BlockIndexOfWorld(changedMinY),
                                BlockIndexOfWorld(changedMinZ) };
    const BlockCoord dirtyMax { BlockIndexOfWorld(changedMaxX), BlockIndexOfWorld(changedMaxY),
                                BlockIndexOfWorld(changedMaxZ) };
    for (int bz = dirtyMin.z; bz <= dirtyMax.z; ++bz) {
        for (int by = dirtyMin.y; by <= dirtyMax.y; ++by) {
            for (int bx = dirtyMin.x; bx <= dirtyMax.x; ++bx) {
                plan.dirty.push_back(BlockCoord { bx, by, bz });
            }
        }
    }
    return plan;
}

MeshData BuildCollapseUnitMesh(const CollapseUnit& unit) {
    // 区域体素 cell 数 = 补丁采样数 − 1：多出的那一格 cell（基采样 = 补丁末采样）正是**边界棱的归属**，
    // 见 `UnitPatchSampler` 的说明（独立碎块没有相邻块替它发射边界四边形）。
    const int sizeX = unit.patchSizeX - 1;
    const int sizeY = unit.patchSizeY - 1;
    const int sizeZ = unit.patchSizeZ - 1;
    if (unit.patchDensity.empty() || unit.patchMaterial.empty() || sizeX < 1 || sizeY < 1 || sizeZ < 1) {
        return {};  // 未抓补丁（或尺寸退化）⇒ 调用方按"无网格"处理
    }

    MeshData mesh = BuildRegionMesh(UnitPatchSampler(unit), sizeX, sizeY, sizeZ);

    // 顶点坐标以"补丁 0 号采样"为原点，而该采样 = `bounds` 外扩的那一圈 ⇒ 世界 (bounds.min − 1)；
    // 平移到**相对质心**后与 `hullPoints` 处在同一坐标系 ⇒ 渲染与物理共用同一个 `mat4`。
    const float offsetX = static_cast<float>(static_cast<double>(unit.bounds.minX - 1) - unit.centroid.x);
    const float offsetY = static_cast<float>(static_cast<double>(unit.bounds.minY - 1) - unit.centroid.y);
    const float offsetZ = static_cast<float>(static_cast<double>(unit.bounds.minZ - 1) - unit.centroid.z);
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.position[0] += offsetX;
        vertex.position[1] += offsetY;
        vertex.position[2] += offsetZ;
    }
    return mesh;
}

bool CarveCollapseUnitPatch(CollapseUnit& unit, const glm::dvec3& center, float radiusBlocks,
                            const TerrainMaterialTable& materials) {
    if (unit.patchDensity.empty() || unit.patchMaterial.empty() || unit.patchIsUnit.empty() ||
        !(radiusBlocks > 0.0F)) {
        return false;
    }

    // 与 `DigVolumeWorld::RasterizeBall` **同一套数学**（含同一过渡带宽 `kCarveSdfBandBlocks`）：
    // 这样"在碎块上挖的缺口"与"在地形上挖的洞"是同一口径的曲面，且等值面仍是补丁采样的纯函数。
    const double radius  = static_cast<double>(radiusBlocks);
    const double outer   = radius + kCarveSdfBandBlocks;
    const double outerSq = outer * outer;

    bool changed = false;
    for (int z = 0; z < unit.patchSizeZ; ++z) {
        for (int y = 0; y < unit.patchSizeY; ++y) {
            for (int x = 0; x < unit.patchSizeX; ++x) {
                const std::size_t index = PatchIndex(unit, x, y, z);
                if (unit.patchIsUnit[index] == 0U) {
                    continue;  // 非本整体的采样在网格化时本就被当空气 ⇒ 改了也无意义（不改，保持数据干净）
                }
                const double dx = static_cast<double>(unit.patchBounds.minX + x) - center.x;
                const double dy = static_cast<double>(unit.patchBounds.minY + y) - center.y;
                const double dz = static_cast<double>(unit.patchBounds.minZ + z) - center.z;
                const double distanceSq = dx * dx + dy * dy + dz * dz;
                if (distanceSq >= outerSq) {
                    continue;
                }

                // 不可破坏材质保持原状（与 `IsIndestructibleSample` 同口径：`kNoMaterialSlot` 也算不可破坏）。
                const std::uint8_t slot = unit.patchMaterial[index];
                if (slot == kNoMaterialSlot) {
                    continue;
                }
                const MaterialLayer& layer = materials.Layer(static_cast<int>(slot));
                if (layer.indestructible || !(layer.toughness > 0.0F)) {
                    continue;
                }

                // CSG 取 max：球内 `半径 − 距离` 为正（挖空），球外为负（保留原值）—— 只抬不降。
                const int carved = std::clamp(
                    static_cast<int>(std::lround((radius - std::sqrt(distanceSq)) *
                                                 static_cast<double>(kDensityUnitsPerBlock))),
                    kDensityMin, kDensityMax);
                std::int8_t& density = unit.patchDensity[index];
                if (carved > static_cast<int>(density)) {
                    density = static_cast<std::int8_t>(carved);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool RefreshCollapseUnitFromPatch(CollapseUnit& unit, const TerrainMaterialTable& materials) {
    if (unit.patchDensity.empty() || unit.patchMaterial.empty() || unit.patchIsUnit.empty()) {
        return false;
    }

    std::vector<CollapseUnit::Voxel> voxels;
    for (int z = 0; z < unit.patchSizeZ; ++z) {
        for (int y = 0; y < unit.patchSizeY; ++y) {
            for (int x = 0; x < unit.patchSizeX; ++x) {
                const std::size_t index = PatchIndex(unit, x, y, z);
                if (unit.patchIsUnit[index] == 0U || !IsSolidValue(unit.patchDensity[index])) {
                    continue;  // 不属于本整体 / 已被挖成空气 ⇒ 不再是它的体素
                }
                voxels.push_back(CollapseUnit::Voxel { unit.patchBounds.minX + x, unit.patchBounds.minY + y,
                                                       unit.patchBounds.minZ + z });
            }
        }
    }
    if (voxels.empty()) {
        unit.voxels.clear();
        return false;  // 整块被挖光
    }
    std::sort(voxels.begin(), voxels.end(), VoxelLessByColumn);
    unit.voxels = std::move(voxels);

    // **质心保持不变**（`keepCentroid = true`）⇒ 局部坐标系不变 ⇒ 刚体与渲染网格原地不动。
    RebuildUnitGeometry(unit, /*keepCentroid*/ true);
    ComputeUnitPhysics(unit, materials);
    return true;
}

}  // namespace vx
