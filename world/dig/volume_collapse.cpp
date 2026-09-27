#include "dig/volume_collapse.hpp"

#include "core/log.hpp"
#include "terrain/material_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

/// 统计该整体**表面 cell** 的材质直方图（T46 修订 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)）。
///
/// 口径与**渲染网格逐字同源**（`volume_mesher.cpp` 的顶点材质规则）：遍历与网格器**同一批 cell**
/// （含外围一格，`i ∈ [-1, size)`），对"8 角有实有空的 cell"取**实体侧各角材质**的众数（同票取更小槽位），
/// 再统计全体表面 cell。语义 = "这块碎块**露在外面的皮**用什么材质做的" —— 也就是玩家真正看到的东西。
///
/// 为什么不能用"全体体素的多数材质"：碎块内部大量体素是**派生**出来的（该列地表表层 → 次表层），
/// 一处陡壁的内部往往被判成土 ⇒ "看着是白岩的一片"会被判成散体、落地后被量化融合
/// （项目所有者 2026-09-27 实测的缺陷）。**"岩石落地后不允许发生任何形状变化"是硬约束**，
/// 故取"**表面出现任何刚性材质即保持形状**"。
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

/// 算出一个倒塌整体的**刚体属性**（T43 / [ADR 0016](../../docs/adr/0016-collapse-realism-impulse-material-debris.md)）：
/// 质量（= Σ 各体素材质的 `density`）、摩擦 / 弹性（= **多数材质**，同票取更小槽位 ⇒ 确定性），
/// 以及（可选）**爆心冲量**折算出的初线速度 / 初角速度。
///
/// 前置条件：`unit.patchMaterial` 已抓（`CaptureUnitPatch`）。**无 GPU / Jolt 依赖** ⇒ 可单测。
void ComputeUnitPhysics(CollapseUnit& unit, const CollapseSeed& seed, const CollapseSpec& spec,
                        const TerrainMaterialTable& materials) {
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

    // ---- T46 修订（ADR 0017）：落地后的表示由**表面的材质**决定，而不是全体体素的多数材质 ----
    // 判据见 `CountUnitSurfaceMaterials`：表面（= 会生成渲染网格顶点的那些 cell）里**出现任何刚性材质**
    // 就保持形状；一个都没有才与地面融合。退化情形（没有表面 cell，正常不可达）回退到体素多数材质。
    CountUnitSurfaceMaterials(unit);
    int  surfaceCells  = 0;
    bool anyRigidShown = false;
    for (int slot = 0; slot < kMaterialSlotCount; ++slot) {
        surfaceCells += unit.surfaceMaterialCounts[slot];
        if (unit.surfaceMaterialCounts[slot] > 0 && materials.Layer(slot).rigidDebris) {
            anyRigidShown = true;
        }
    }
    unit.rigidDebris = (surfaceCells > 0) ? anyRigidShown : materials.Layer(static_cast<int>(best)).rigidDebris;

    // ---- 爆心冲量（ADR 0016 决策一）：逐体素向外、按距离线性衰减；再折算成刚体的 V / ω ----
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
/// 取 32 的依据：落定时刚体已被物理判为"静止"，故落点与真实支撑的差通常 ≤ 2 格；32 是极宽裕的上界，
/// 真正把它走满会**计入 `stuckVoxels` 并告警**（不静默留悬空体素）。
constexpr int kSettleMaxDropBlocks = 32;

}  // namespace

float CollapsePose::TiltDegrees() const noexcept {
    const glm::vec3 up     = rotation * glm::vec3(0.0F, 1.0F, 0.0F);
    const float     cosine = std::min(1.0F, std::max(-1.0F, up.y));
    return std::acos(cosine) * 57.29577951308232F;  // 弧度 → 度（不引 glm 的三角函数头，避免多余依赖）
}

bool LocalAabbContainsPoint(const CollapseUnit& unit, const CollapsePose& pose, const glm::dvec3& point) noexcept {
    // 世界点 → 该整体的**局部坐标系**（局部 AABB 随姿态旋转 ⇒ 等价于"朝任意方向的盒"）。
    const glm::dvec3 delta = point - pose.position;
    const glm::vec3  local = glm::inverse(pose.rotation) * glm::vec3(delta);
    return local.x >= unit.hullMinLocal.x && local.x <= unit.hullMaxLocal.x && local.y >= unit.hullMinLocal.y &&
           local.y <= unit.hullMaxLocal.y && local.z >= unit.hullMinLocal.z && local.z <= unit.hullMaxLocal.z;
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
                    break;  // 到达区域底面（已向下扩的余量用尽）⇒ 计入 stuck
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
            if (drop == kSettleMaxDropBlocks) {
                ++result.stuckVoxels;  // 极宽裕的上界被走满 ⇒ 仍然悬空（由调用方告警）
            }
        }
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

    // ---- 邻域（T30：围绕**被改动的采样**展开，而不是整个体积 / 整个块）----
    //
    // 水平：悬挑判定最多跨 `maxCantileverBlocks` 步，故外扩 (悬挑 + 1) 格即可覆盖所有可能被影响的列；
    //       `neighborhood_margin_blocks` 仍保留为"在此之上再额外外扩的块数"（默认 0）。
    // 竖直：载荷通路是纵向的，但**整块实心**与**整块空**的块都能整块排除 —— 故竖直 = 自种子块向下到
    //       "最低的非全实心块"、向上到"最高的非全空块"。
    const int reach      = static_cast<int>(std::ceil(std::max(spec.maxCantileverBlocks, 0.0F))) + 1;
    const int marginWorld = std::max(spec.neighborhoodMarginBlocks, 0) * kVolumeBlockSize;

    const int minX = seed.bounds.minX - reach - marginWorld;
    const int maxX = seed.bounds.maxX + reach + marginWorld;
    const int minZ = seed.bounds.minZ - reach - marginWorld;
    const int maxZ = seed.bounds.maxZ + reach + marginWorld;

    const int horizontalMinX = BlockIndexOfWorld(minX);
    const int horizontalMaxX = BlockIndexOfWorld(maxX);
    const int horizontalMinZ = BlockIndexOfWorld(minZ);
    const int horizontalMaxZ = BlockIndexOfWorld(maxZ);

    int volumeMinBlockY = BlockIndexOfWorld(seed.bounds.minY);
    int volumeMaxBlockY = BlockIndexOfWorld(seed.bounds.maxY);
    for (const auto& entry : volumes.Blocks()) {
        volumeMinBlockY = std::min(volumeMinBlockY, entry.first.y);
        volumeMaxBlockY = std::max(volumeMaxBlockY, entry.first.y);
    }

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

    const int minY = BlockOriginBlocks(lowBlockY);
    const int maxY = BlockOriginBlocks(highBlockY) + kVolumeBlockSize;
    const int sizeX = maxX - minX + 1;
    const int sizeY = maxY - minY + 1;
    const int sizeZ = maxZ - minZ + 1;

    const std::size_t sampleCount = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY) *
                                    static_cast<std::size_t>(sizeZ);
    plan.regionSamples = sampleCount;
    if (sampleCount > kMaxRegionSamples) {
        VX_LOG_WARN("塌落跳过：支撑检查邻域过大（%d × %d × %d = %zu 个采样，上限 %zu）", sizeX, sizeY, sizeZ,
                    sampleCount, kMaxRegionSamples);
        return plan;
    }

    DensityRegion region = volumes.ReadDensityRegion(minX, minY, minZ, sizeX, sizeY, sizeZ);

    // ---- ① 支撑体素 = "载荷能沿实心一路传到地底"的实心体素 ----
    //
    // 先做**纵向**判据（逐列自下而上）：只有"从区域底面起连续实心"的那一段才算接地 ——
    // 区域底面视作地面（本项目体积覆盖到世界 y = 0，其下处处实心）。
    // 再把"接地"沿**同一高度层的实心连通**横向传播 `floor(maxCantileverBlocks)` 步：
    // 这一步就是"悬挑 / 拱效应"——离接地处不超过该跨度的岩体仍算有支撑。
    std::vector<std::uint8_t> reached(region.values.size(), 0);
    std::vector<std::uint32_t> queue;
    queue.reserve(region.values.size() / 2U + 16U);

    for (int z = 0; z < sizeZ; ++z) {
        for (int x = 0; x < sizeX; ++x) {
            bool chain = true;  // 该列自底向上是否仍"连着地面"
            for (int y = 0; y < sizeY; ++y) {
                const std::size_t index = region.Index(x, y, z);
                if (!IsSolidValue(region.values[index])) {
                    chain = false;  // 出现空腔 ⇒ 其上的一切都失去了这条路
                    continue;
                }
                if (chain) {
                    reached[index] = 1;
                    queue.push_back(static_cast<std::uint32_t>(index));
                }
            }
        }
    }

    // 同层 4 邻域 BFS：最多走 floor(maxCantilever) 步。
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
                const std::size_t neighbor = region.Index(nx[dir], y, nz[dir]);
                if (reached[neighbor] != 0 || !IsSolidValue(region.values[neighbor])) {
                    continue;
                }
                reached[neighbor] = 1;
                queue.push_back(static_cast<std::uint32_t>(neighbor));
            }
        }
    }

    // ---- ② 失去支撑的实心体素按 6 邻域连通分量分组 ----
    //
    // 每个分量 = 一个"整体"（倒塌时一起动）。BFS 采用**确定的扫描 / 邻接顺序**（x → y → z，±x/±y/±z），
    // 故同一输入永远得到同一分组（红线 7）。
    std::vector<std::uint8_t> visited(region.values.size(), 0);
    std::vector<CollapseUnit> found;
    queue.clear();

    for (int z = 0; z < sizeZ; ++z) {
        for (int y = 0; y < sizeY; ++y) {
            for (int x = 0; x < sizeX; ++x) {
                const std::size_t start = region.Index(x, y, z);
                if (visited[start] != 0 || reached[start] != 0 || !IsSolidValue(region.values[start])) {
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
                    if (unit.voxels.size() == 1) {
                        unit.bounds = VoxelBounds { minX + vx, minY + vy, minZ + vz, minX + vx, minY + vy, minZ + vz };
                    } else {
                        unit.bounds.minX = std::min(unit.bounds.minX, minX + vx);
                        unit.bounds.minY = std::min(unit.bounds.minY, minY + vy);
                        unit.bounds.minZ = std::min(unit.bounds.minZ, minZ + vz);
                        unit.bounds.maxX = std::max(unit.bounds.maxX, minX + vx);
                        unit.bounds.maxY = std::max(unit.bounds.maxY, minY + vy);
                        unit.bounds.maxZ = std::max(unit.bounds.maxZ, minZ + vz);
                    }

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
                        if (visited[neighbor] != 0 || reached[neighbor] != 0 ||
                            !IsSolidValue(region.values[neighbor])) {
                            continue;
                        }
                        visited[neighbor] = 1;
                        queue.push_back(static_cast<std::uint32_t>(neighbor));
                    }
                }

                // 体素按 (x, z, y) 排序：同一 (x, z) 列由此连续 ⇒ 一趟扫出逐列最低 / 最高角点。
                std::sort(unit.voxels.begin(), unit.voxels.end(), VoxelLessByColumn);

                glm::dvec3 sum(0.0);
                for (const CollapseUnit::Voxel& voxel : unit.voxels) {
                    sum += glm::dvec3(static_cast<double>(voxel.x) + 0.5, static_cast<double>(voxel.y) + 0.5,
                                      static_cast<double>(voxel.z) + 0.5);
                }
                unit.centroid = sum / static_cast<double>(unit.voxels.size());

                // 凸包点集：每个 (x, z) 列取 [minY, maxY] 的**包围盒 8 个角点**（比逐体素点少几个量级，
                // 且形状构建开销因此有上界）。凹形会被填平（ADR 0015 后果 1）。
                unit.hullPoints.reserve(unit.voxels.size() >= 8 ? 8U * 8U : 24U);
                std::size_t columnStart = 0;
                while (columnStart < unit.voxels.size()) {
                    std::size_t columnEnd = columnStart;
                    while (columnEnd + 1 < unit.voxels.size() &&
                           unit.voxels[columnEnd + 1].x == unit.voxels[columnStart].x &&
                           unit.voxels[columnEnd + 1].z == unit.voxels[columnStart].z) {
                        ++columnEnd;
                    }
                    const int columnX  = unit.voxels[columnStart].x;
                    const int columnZ  = unit.voxels[columnStart].z;
                    const int columnMinY = unit.voxels[columnStart].y;      // 已按 y 升序
                    const int columnMaxY = unit.voxels[columnEnd].y + 1;    // 体素上表面
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

                plan.unsupportedVoxels += unit.voxels.size();

                // T46：`hullPoints` 的**局部 AABB**（光球命中"保留中的刚性残骸"的判据，见 `LocalAabbContainsPoint`）。
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
                found.push_back(std::move(unit));
            }
        }
    }

    if (found.empty()) {
        return plan;
    }

    const TerrainMaterialTable& materials = volumes.Materials();

    int changedMinX = maxX;
    int changedMinY = maxY;
    int changedMinZ = maxZ;
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
        // T43：再按补丁里的材质算质量 / 摩擦 / 弹性与（可选）爆心冲量。
        ComputeUnitPhysics(unit, seed, spec, materials);
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

}  // namespace vx
