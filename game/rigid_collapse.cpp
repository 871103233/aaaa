#include "rigid_collapse.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace vx {
namespace {

/// 确定性哈希（红线 7：禁止随机数）。用于给倒塌整体一个**确定但不对称**的初始倾覆方向。
[[nodiscard]] std::uint32_t Hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    std::uint32_t hash = a * 0x9E3779B1U ^ b * 0x85EBCA6BU ^ c * 0xC2B2AE35U;
    hash ^= hash >> 15;
    hash *= 0x2545F491U;
    hash ^= hash >> 13;
    return hash;
}

/// 把一批体素**原样写回**（`Spawn` 失败时的兜底：绝不让体素凭空消失）。
void RestoreVoxels(DigVolumeWorld& volumes, const CollapseUnit& unit) {
    const VoxelBounds bounds { unit.bounds.minX - 1, unit.bounds.minY - 1, unit.bounds.minZ - 1,
                               unit.bounds.maxX + 1, unit.bounds.maxY + 1, unit.bounds.maxZ + 1 };
    const int sizeX = bounds.maxX - bounds.minX + 1;
    const int sizeY = bounds.maxY - bounds.minY + 1;
    const int sizeZ = bounds.maxZ - bounds.minZ + 1;
    DensityRegion region = volumes.ReadDensityRegion(bounds.minX, bounds.minY, bounds.minZ, sizeX, sizeY, sizeZ);
    for (const CollapseUnit::Voxel& voxel : unit.voxels) {
        const int x = voxel.x - bounds.minX;
        const int y = voxel.y - bounds.minY;
        const int z = voxel.z - bounds.minZ;
        if (region.Contains(x, y, z)) {
            region.values[region.Index(x, y, z)] = static_cast<std::int8_t>(-kDensityUnitsPerBlock);
        }
    }
    volumes.WriteDensityRegion(region);
}

/// 把一个槽位清成**不可见**（T42）：空网格 ⇒ `usedIndexCount = 0`，既不绘制也不上传。
void HideSlot(MeshRenderer& renderer, MeshHandle mesh) {
    if (!mesh.IsValid()) {
        return;
    }
    (void)renderer.UpdateMeshGeometry(mesh, MeshData {}, glm::dvec3(0.0));
}

/// T47（[ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策五）：
/// **外观网格不闭合时**打出可归因的 WARN —— 让"某些面透明"这条现象**指名**到候选 ①②③ 之一。
///
/// 判据（**靠"截断前 / 截断后"两组数字分开**，这是本插桩的全部意义）：
///   · `full == 0` 且被截断 ⇒ 洞由**容量截断**造成（**候选①**）；
///   · `full != 0` 且未截断 ⇒ 洞由**网格生成本身**造成（**候选②敞口场景 / 候选③地形↔体积交界缝**），
///     与容量无关；
///   · 两者都命中 ⇒ 两种成因并存（先修①再复测②/③）。
///
/// 闭合且未被截断时不打日志（稳态零噪声）。
void WarnIfMeshOpen(const CollapseUnit& unit, std::size_t fullBoundary, std::size_t finalBoundary, bool truncated) {
    if (fullBoundary == 0 && finalBoundary == 0) {
        return;
    }
    const char* cause = "**网格生成本身**（候选②敞口场景 / 候选③地形↔体积交界缝）";
    if (truncated) {
        cause = fullBoundary == 0 ? "**网格池容量截断**（候选①）"
                                  : "**容量截断 + 网格生成本身**（候选① 与 ②/③ 并存）";
    }
    VX_LOG_WARN("整体外观网格缺面（T47 自检）：体素 %zu 个、patch %d×%d×%d；"
                "**截断前**边界边 %zu 条、**实际上传**网格边界边 %zu 条、容量截断 = %s ⇒ 成因 = %s",
                unit.voxels.size(), unit.patchSizeX, unit.patchSizeY, unit.patchSizeZ, fullBoundary, finalBoundary,
                truncated ? "是" : "否", cause);
}

}  // namespace

void RigidCollapseRuntime::Init(MeshRenderer& renderer, std::size_t slotCount, std::size_t capacityVerts) {
    if (m_capacityVerts != 0) {
        return;  // 只初始化一次
    }
    m_capacityVerts   = capacityVerts;
    m_capacityIndices = (capacityVerts / 4U) * 6U;  // 每个四边形 6 个索引（Surface Nets 的四边形输出）
    m_pool.reserve(slotCount);
    m_active.reserve(slotCount);

    // 槽位 = **满容量**的空网格：GPU 缓冲一次建好，此后只就地写用到的前缀（不再创建 GPU 资源）。
    // 初始索引内容无意义（`usedIndexCount = 0` ⇒ 不绘制），故全部填 0 即可。
    MeshData empty;
    empty.vertices.assign(m_capacityVerts, MeshVertex {});
    empty.indices.assign(m_capacityIndices, 0U);
    for (std::size_t slot = 0; slot < slotCount; ++slot) {
        const MeshHandle handle = renderer.UploadMesh(empty, glm::dvec3(0.0));
        if (!handle.IsValid()) {
            VX_LOG_ERROR("倒塌网格池：第 %zu 个槽位创建失败 ⇒ 池只有 %zu 个槽位", slot, m_pool.size());
            break;
        }
        m_pool.push_back(handle);
        HideSlot(renderer, handle);  // 初始不可见（索引数 0）
    }

    VX_LOG_INFO("倒塌网格池已建立（T33 / T42 等值面）：%zu 个槽位 × 每槽 %zu 顶点 + %zu 索引"
                "（每槽约 %.2f MB 顶点 + %.2f MB 索引；只上传用到的前缀）",
                m_pool.size(), m_capacityVerts, m_capacityIndices,
                static_cast<double>(m_capacityVerts * sizeof(MeshVertex)) / (1024.0 * 1024.0),
                static_cast<double>(m_capacityIndices * sizeof(std::uint32_t)) / (1024.0 * 1024.0));
}

bool RigidCollapseRuntime::AcquireSlot(MeshHandle& meshOut) {
    if (m_active.size() >= m_pool.size()) {
        return false;
    }
    // 池很小（≤ 16），线性找出第一个"未被活跃集合占用"的槽位即可；结果与顺序无关（确定性）。
    for (MeshHandle candidate : m_pool) {
        const bool busy =
            std::any_of(m_active.begin(), m_active.end(),
                        [candidate](const ActiveCollapseUnit& unit) { return unit.mesh.id == candidate.id; });
        if (!busy) {
            meshOut = candidate;
            return true;
        }
    }
    return false;
}

RigidCollapseRuntime::UnitMeshBuild RigidCollapseRuntime::BuildUnitMesh(const CollapseUnit& unit) const {
    UnitMeshBuild build;
    build.mesh = BuildCollapseUnitMesh(unit);
    if (build.mesh.indices.empty() || build.mesh.vertices.empty()) {
        return build;  // 没有补丁 / 退化 ⇒ 由 `Spawn` 报错并回退
    }

    // T47：**截断前**先量一次 —— 这是归因的前提：容量截断**必然**制造边界边，
    // 只有先把"网格生成本身是否闭合"量出来，才能把玩家看到的洞归到候选①还是候选②/③。
    build.boundaryEdgesFull = CountBoundaryEdges(build.mesh);

    if (build.mesh.vertices.size() <= m_capacityVerts && build.mesh.indices.size() <= m_capacityIndices) {
        build.boundaryEdgesFinal = build.boundaryEdgesFull;
        WarnIfMeshOpen(unit, build.boundaryEdgesFull, build.boundaryEdgesFinal, false);
        return build;  // 常规路径：容量足够（整座塔的外表面通常只有几千个四边形）
    }

    // 超出容量：**按整个四边形**截断（索引每 6 个一个四边形），且只保留"引用顶点都在容量内"的四边形
    // ⇒ 顶点前缀上传即可覆盖全部被引用顶点。只影响外观，物理与回写不受影响（与 T33 的截断口径一致）。
    const std::size_t fullQuads = build.mesh.indices.size() / 6U;
    const std::size_t fullVerts = build.mesh.vertices.size();
    std::size_t quads = 0;
    while ((quads + 1U) * 6U <= build.mesh.indices.size() && (quads + 1U) * 6U <= m_capacityIndices) {
        bool fits = true;
        for (std::size_t index = quads * 6U; index < (quads + 1U) * 6U; ++index) {
            if (static_cast<std::size_t>(build.mesh.indices[index]) >= m_capacityVerts) {
                fits = false;
                break;
            }
        }
        if (!fits) {
            break;
        }
        ++quads;
    }
    build.mesh.indices.resize(quads * 6U);
    if (build.mesh.vertices.size() > m_capacityVerts) {
        build.mesh.vertices.resize(m_capacityVerts);
    }
    build.truncatedByCapacity = true;
    build.boundaryEdgesFinal  = CountBoundaryEdges(build.mesh);

    // T47：本条 WARN 补上**体素数**与 **patch 尺寸**（原先只报四边形 / 顶点数，且报的是**截断后**的值，
    // 无法据此判断"到底超了多少"）；原始规模现在如实报出。
    VX_LOG_WARN("倒塌整体的等值面超出网格池容量（T47）：体素 %zu 个、patch %d×%d×%d、原始 %zu 个四边形 / %zu 顶点"
                "（每槽上限 %zu 顶点 + %zu 索引）⇒ 本次只渲染前 %zu 个四边形"
                "（物理与回写不受影响）",
                unit.voxels.size(), unit.patchSizeX, unit.patchSizeY, unit.patchSizeZ, fullQuads, fullVerts,
                m_capacityVerts, m_capacityIndices, quads);
    WarnIfMeshOpen(unit, build.boundaryEdgesFull, build.boundaryEdgesFinal, true);
    return build;
}

bool RigidCollapseRuntime::Spawn(PhysicsWorld& physics, DigVolumeWorld& volumes, MeshRenderer& renderer,
                                 const CollapseSpec& spec, CollapseUnit unit) {
    MeshHandle mesh;
    if (!AcquireSlot(mesh)) {
        VX_LOG_WARN("倒塌跳过：活跃整体已达上限（%zu 个）⇒ 本次不抽出该整体（保持原状，见 ADR 0015）", m_pool.size());
        RestoreVoxels(volumes, unit);
        return false;
    }

    // 初始状态（T43 / ADR 0016）：
    //   ① 有**爆心冲量** ⇒ 用冲量算出的初线速度 + 初角速度（真实的不对称来自冲量分布，"被炸飞"）；
    //   ② 没有（`impulse_speed = 0`，或本次不是爆炸引起）⇒ 退回**人工不对称**：确定性哈希给出的
    //      一个小角速度（否则细长的塔只会"原地垂直落下"而不倒）。
    glm::vec3 initialLinear(0.0F);
    glm::vec3 initialAngular(0.0F);
    if (unit.hasImpulse) {
        initialLinear  = unit.impulseLinearVelocity;
        initialAngular = unit.impulseAngularVelocity;
    } else {
        const std::uint32_t hash    = Hash3(static_cast<std::uint32_t>(unit.bounds.minX),
                                            static_cast<std::uint32_t>(unit.bounds.minY),
                                            static_cast<std::uint32_t>(unit.bounds.minZ));
        const float         azimuth = static_cast<float>(hash & 0xFFFFU) / 65535.0F * 6.2831853F;
        const float         sign    = ((hash >> 16) & 1U) != 0U ? 1.0F : -1.0F;
        const glm::vec3     tiltAxis(std::cos(azimuth), 0.0F, std::sin(azimuth));
        initialAngular = tiltAxis * (sign * spec.initialTiltSpeed);
    }

    PhysicsWorld::ConvexHullDesc hull;
    hull.positions       = unit.hullPoints.data();
    hull.pointCount      = unit.hullPoints.size() / 3U;
    hull.originX         = unit.centroid.x;
    hull.originY         = unit.centroid.y;
    hull.originZ         = unit.centroid.z;
    // 质量 / 摩擦 / 弹性来自**材质表**（T43：石 ≠ 土 ≠ 草 ≠ 沙），由 world 层按体素算好。
    hull.mass            = unit.mass;
    hull.friction        = unit.friction;
    hull.restitution     = unit.restitution;
    hull.linearVelocity  = initialLinear;
    hull.angularVelocity = initialAngular;

    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(hull);
    if (body == 0) {
        VX_LOG_ERROR("倒塌失败：凸包刚体创建失败（体素数 %zu、凸包点数 %zu）⇒ 体素已写回，本次不倒塌", unit.voxels.size(),
                     hull.pointCount);
        RestoreVoxels(volumes, unit);
        return false;
    }

    // T42：几何 = **与地形同源的等值面**（`BuildCollapseUnitMesh`），只在上传时写一次；
    // 之后每帧只推 64 B 的模型变换（见 `SyncRender`）。局部顶点是"相对质心"的坐标，与凸包同源。
    const UnitMeshBuild build = BuildUnitMesh(unit);
    if (build.mesh.indices.empty() ||
        !renderer.UpdateMeshGeometry(mesh, build.mesh, unit.centroid)) {
        physics.RemoveBody(body);
        RestoreVoxels(volumes, unit);
        VX_LOG_ERROR("倒塌失败：渲染网格就地刷新失败（体素数 %zu、四边形 %zu）⇒ 体素已写回，本次不倒塌",
                     unit.voxels.size(), build.mesh.indices.size() / 6U);
        return false;
    }

    ActiveCollapseUnit active;
    active.body         = body;
    active.mesh         = mesh;
    active.unit         = std::move(unit);
    // T47：记下**实际上传**那份外观网格的闭合自检结果，供 `RetireRetained` 回报。
    active.meshBoundaryEdges = build.boundaryEdgesFinal;
    active.meshTruncated     = build.truncatedByCapacity;
    active.posePosition = active.unit.centroid;
    active.poseRotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    m_active.push_back(std::move(active));
    ++m_spawned;
    return true;
}

void RigidCollapseRuntime::Step(PhysicsWorld& physics, const CollapseSpec& spec,
                                std::vector<ActiveCollapseUnit>& settledOut) {
    for (std::size_t index = 0; index < m_active.size();) {
        ActiveCollapseUnit&                unit  = m_active[index];
        const PhysicsWorld::RigidBodyState state = physics.GetRigidBodyState(unit.body);
        unit.posePosition = state.position;
        unit.poseRotation = state.rotation;

        // T46（ADR 0017）：**保留中的刚性残骸**只刷位姿（渲染用），不再判落定 —— 它已落定且**不回写**。
        // 它可能在"支撑被挖掉"后被唤醒而重新运动（物理引擎负责），此时网格位姿自动跟随；
        // 若再次静止，也无需再处理（形状保持不变，本就不该回写）。
        if (unit.retained) {
            ++index;
            continue;
        }

        const bool still = glm::length(state.linearVelocity) <= spec.settleLinearSpeed &&
                           glm::length(state.angularVelocity) <= spec.settleAngularSpeed;
        unit.settleSteps = still ? (unit.settleSteps + 1) : 0;
        if (unit.settleSteps >= spec.settleSteps) {
            unit.settleSteps = 0;
            if (unit.unit.rigidDebris) {
                // 刚性（岩）：落定后**保留几何体** —— 不移出活跃集合、不回写、不使用网格槽位重传。
                // 这是"岩石落地后不允许任何形状变化"的落地方式（ADR 0017 决策二）。
                unit.retained = true;
                ++m_settled;
                VX_LOG_INFO("岩石残骸保留（T46）：体素 %zu 个、倾角 %.0f°、落点 (%.1f, %.1f, %.1f)"
                            "（**不回写** ⇒ 形状与掉落中一致；光球命中它时才体素化）",
                            unit.unit.voxels.size(),
                            static_cast<double>(CollapsePose { state.position, state.rotation }.TiltDegrees()),
                            state.position.x, state.position.y, state.position.z);
                ++index;
                continue;
            }
            settledOut.push_back(unit);
            m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(index));
            continue;  // 不递增下标：`erase` 已把后面的元素前移
        }
        ++index;
    }
}

void RigidCollapseRuntime::SyncRender(MeshRenderer& renderer) {
    for (const ActiveCollapseUnit& unit : m_active) {
        renderer.SetMeshTransform(unit.mesh, unit.posePosition, unit.poseRotation);
    }
}

CollapseWriteback RigidCollapseRuntime::Writeback(PhysicsWorld& physics, DigVolumeWorld& volumes,
                                                  MeshRenderer& renderer, const ActiveCollapseUnit& settled) {
    // 体素 ↔ 刚体转换的核心数学在 world 层（`WritebackCollapseUnit`，可单测）；这里只做接线与记账。
    const CollapsePose pose { settled.posePosition, settled.poseRotation };
    const CollapseWriteback result = WritebackCollapseUnit(volumes, settled.unit, pose);

    physics.RemoveBody(settled.body);
    HideSlot(renderer, settled.mesh);

    ++m_settled;
    m_writtenVoxels += result.writtenVoxels;
    m_droppedVoxels += result.droppedVoxels;
    return result;
}

std::size_t RigidCollapseRuntime::RetainedUnits() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_active.begin(), m_active.end(),
                      [](const ActiveCollapseUnit& unit) { return unit.retained; }));
}

bool RigidCollapseRuntime::RetireOldestRetained(PhysicsWorld& physics, DigVolumeWorld& volumes,
                                                MeshRenderer& renderer, CollapseWriteback& out) {
    for (std::size_t index = 0; index < m_active.size(); ++index) {
        if (m_active[index].retained) {  // `m_active` 即生成序 ⇒ 第一个保留中的就是最旧的
            return RetireRetained(index, physics, volumes, renderer, out);
        }
    }
    return false;
}

bool RigidCollapseRuntime::RetireRetained(std::size_t index, PhysicsWorld& physics, DigVolumeWorld& volumes,
                                          MeshRenderer& renderer, CollapseWriteback& out) {
    if (index >= m_active.size() || !m_active[index].retained) {
        return false;
    }
    // 惰性回写 = 与"落定即回写"同一段数学（world 层可单测），差别只在**触发时机**（玩家动作 / 腾位）。
    const ActiveCollapseUnit& unit = m_active[index];

    // T47：回报该残骸**保留期间**那份外观网格的闭合自检结果 —— 它正是玩家看到的"缺面"的来源。
    // 只在当时确实不闭合时打（闭合残骸退役是稳态事件，不该刷日志）。
    if (unit.meshBoundaryEdges != 0) {
        VX_LOG_WARN("保留残骸退役自检（T47）：该整体外观网格有 %zu 条边界边%s（体素 %zu 个）"
                    "⇒ 它在保留期间显示的缺面来自这里",
                    unit.meshBoundaryEdges, unit.meshTruncated ? "（且当时被网格池容量截断）" : "",
                    unit.unit.voxels.size());
    }

    const CollapsePose        pose { unit.posePosition, unit.poseRotation };
    out = WritebackCollapseUnit(volumes, unit.unit, pose);

    physics.RemoveBody(unit.body);
    HideSlot(renderer, unit.mesh);

    m_writtenVoxels += out.writtenVoxels;
    m_droppedVoxels += out.droppedVoxels;
    ++m_retired;
    m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void RigidCollapseRuntime::RetireCarved(std::size_t index, PhysicsWorld& physics, MeshRenderer& renderer) {
    if (index >= m_active.size()) {
        return;
    }
    const ActiveCollapseUnit unit = m_active[index];  // 拷贝：下面要 erase
    physics.RemoveBody(unit.body);
    HideSlot(renderer, unit.mesh);
    m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(index));
}

bool RigidCollapseRuntime::CarveBody(PhysicsWorld::BodyHandle body, const glm::dvec3& center, float radiusBlocks,
                                     const CollapseSpec& spec, const TerrainMaterialTable& materials,
                                     PhysicsWorld& physics, MeshRenderer& renderer, CollapseCarveResult& out) {
    if (body == 0) {
        return false;
    }
    for (std::size_t index = 0; index < m_active.size(); ++index) {
        ActiveCollapseUnit& active = m_active[index];
        if (active.body != body) {
            continue;
        }

        // ① 在**自身补丁**上挖一个球（与地形同口径的数学）。球没碰到它 ⇒ 无操作（不是错误）。
        if (!CarveCollapseUnitPatch(active.unit, center, radiusBlocks, materials)) {
            return false;
        }

        // ② 记住位姿与速度：雕刻**不该让碎块位移或顿一下**（同一帧内同源）。
        const PhysicsWorld::RigidBodyState state = physics.GetRigidBodyState(body);
        const std::size_t                 before = active.unit.voxels.size();

        // ③ 由补丁重算体素清单 / 凸包 / 质量与落地口径（**质心不变** ⇒ 局部坐标系不变）。
        const bool survives = RefreshCollapseUnitFromPatch(active.unit, materials);
        out.removedVoxels   = (before > active.unit.voxels.size()) ? (before - active.unit.voxels.size()) : 0U;
        out.leftVoxels      = active.unit.voxels.size();
        out.deleted         = false;

        // ④ 兜底：剩余太少 / 凸包点数不足 ⇒ **删除整个整体**（体素就此消失）。
        //    与"小碎片清除"同源（ADR 0016 决策三）：被打碎的岩石本来就该碎掉，而不是留一个凸包在空气里。
        const bool tooSmall = spec.debrisDeleteMaxVoxels > 0 &&
                             out.leftVoxels <= static_cast<std::size_t>(spec.debrisDeleteMaxVoxels);
        if (!survives || active.unit.hullPoints.size() / 3U < 4U || tooSmall) {
            RetireCarved(index, physics, renderer);
            out.deleted = true;
            ++m_carved;
            m_carvedVoxels += out.removedVoxels;
            return true;
        }

        // ⑤ 按剩余体素在**当前姿态**下原地重建刚体（先建后删：不留一帧的"真空"）。
        PhysicsWorld::ConvexHullDesc hull;
        hull.positions       = active.unit.hullPoints.data();
        hull.pointCount      = active.unit.hullPoints.size() / 3U;
        hull.originX         = active.unit.centroid.x;
        hull.originY         = active.unit.centroid.y;
        hull.originZ         = active.unit.centroid.z;
        hull.mass            = active.unit.mass;
        hull.friction        = active.unit.friction;
        hull.restitution     = active.unit.restitution;
        hull.linearVelocity  = state.linearVelocity;
        hull.angularVelocity = state.angularVelocity;
        hull.rotation        = state.rotation;  // 已倒下的岩石不该因为被雕刻而回正

        const PhysicsWorld::BodyHandle newBody  = physics.AddDynamicConvexHull(hull);
        const UnitMeshBuild            build    = BuildUnitMesh(active.unit);
        const bool meshOk = !build.mesh.indices.empty() &&
                            renderer.UpdateMeshGeometry(active.mesh, build.mesh, active.unit.centroid);
        if (newBody == 0 || !meshOk) {
            // 重建失败 ⇒ 宁可整体删除，也不留下"看着实心却打不到 / 形状对不上"的东西。
            VX_LOG_ERROR("雕刻后重建失败（体素 %zu 个、凸包点数 %zu）⇒ 整体删除（T50）", out.leftVoxels,
                         hull.pointCount);
            if (newBody != 0) {
                physics.RemoveBody(newBody);
            }
            RetireCarved(index, physics, renderer);
            out.deleted = true;
            ++m_carved;
            m_carvedVoxels += out.removedVoxels;
            return true;
        }

        physics.RemoveBody(body);
        active.body              = newBody;
        active.meshBoundaryEdges = build.boundaryEdgesFinal;
        active.meshTruncated     = build.truncatedByCapacity;
        active.posePosition      = state.position;
        active.poseRotation      = state.rotation;
        ++m_carved;
        m_carvedVoxels += out.removedVoxels;
        return true;
    }
    return false;
}

void RigidCollapseRuntime::AwakenIntersecting(const BlockCoord& block, PhysicsWorld& physics) {
    // 块的世界 AABB（闭区间 `[origin, origin + 32)`）。
    const glm::dvec3 blockMin(static_cast<double>(BlockOriginBlocks(block.x)),
                              static_cast<double>(BlockOriginBlocks(block.y)),
                              static_cast<double>(BlockOriginBlocks(block.z)));
    const glm::dvec3 blockMax = blockMin + glm::dvec3(static_cast<double>(kVolumeBlockSize));

    for (const ActiveCollapseUnit& unit : m_active) {
        if (!unit.retained) {
            continue;
        }
        // 该残骸的**世界 AABB**（world 层纯函数，= 局部 AABB 的 8 角旋转后取包围盒）——
        // 它**保守包含**真实 OBB ⇒ 不会漏唤醒；多唤醒的代价只是它重新静止一次。
        glm::dvec3 low(0.0);
        glm::dvec3 high(0.0);
        UnitWorldAabb(unit.unit, CollapsePose { unit.posePosition, unit.poseRotation }, low, high);
        if (low.x > blockMax.x || high.x < blockMin.x || low.y > blockMax.y || high.y < blockMin.y ||
            low.z > blockMax.z || high.z < blockMin.z) {
            continue;  // 不相交
        }
        physics.ActivateBody(unit.body);
        VX_LOG_INFO("岩石残骸被唤醒（T46）：块 (%d, %d, %d) 的碰撞体已重建 ⇒ 若支撑没了它会重新下落",
                    block.x, block.y, block.z);
    }
}

void RigidCollapseRuntime::Clear(PhysicsWorld& physics, MeshRenderer& renderer) {
    for (const ActiveCollapseUnit& unit : m_active) {
        physics.RemoveBody(unit.body);
        HideSlot(renderer, unit.mesh);
    }
    m_active.clear();
}

}  // namespace vx
