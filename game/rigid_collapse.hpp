#pragma once

#include "dig/dig_volume.hpp"
#include "dig/volume_collapse.hpp"
#include "physics/physics_world.hpp"
#include "render/mesh_renderer.hpp"
#include "terrain/terrain_types.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// 一个**活跃的倒塌整体**（T33）：Jolt 动态刚体 + 它的渲染网格（来自池）+ 体素清单。
struct ActiveCollapseUnit {
    PhysicsWorld::BodyHandle body {};          ///< Jolt 动态刚体（0 = 无效）
    MeshHandle               mesh {};          ///< 渲染网格（池槽位；局部顶点不变，每帧只改位姿）
    CollapseUnit             unit;             ///< 体素清单 / 质心 / 局部凸包点（落定回写用）
    int                      settleSteps = 0;  ///< 连续"低于落定阈值"的固定步数
    glm::dvec3               posePosition { 0.0 };                     ///< 最近一次读取的位姿（渲染 / 回写用）
    glm::quat                poseRotation { 1.0F, 0.0F, 0.0F, 0.0F };
    /// ---- T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)：保留中的刚性残骸 ----
    ///
    /// `true` = 该整体是**刚性**（`unit.rigidDebris`）且**已落定** ⇒ **不回写**、形状与外观保持不变，
    /// 直到"光球命中"或"池满腾位"时才**惰性体素化**（见 `RetireRetainedAt` / `RetireOldestRetained`）。
    bool       retained = false;
};

/// **体素化回写**的结果见 `vx::CollapseWriteback`（world 层 `volume_collapse.hpp`）——
/// "体素 ↔ 刚体转换"的核心数学放在 world 层（可脱离 GPU / Jolt 单测），本类只负责与物理 / 渲染的接线。

/// 倒塌整体（T33）的运行时：活跃刚体 + **预分配**的渲染网格池。
///
/// 池化的理由（SKILL 第四节硬规则 4：资源与管线创建不得发生在渲染热路径）：池在**加载期**一次性建好，
/// 倒塌发生时只做 `UpdateMeshGeometry`（就地写**用到的顶点 / 索引前缀**，不建资源、不等 GPU）；
/// 倒塌期间每帧只 `SetMeshTransform`（一次 64 字节 uniform 推送）—— CPU **不重烘焙顶点**。
///
/// T42（外观口径统一）：每个槽位的几何来自 `BuildCollapseUnitMesh`（**与地形同源的 Surface Nets**），
/// 不再是"逐体素方块面 + 面法线" ⇒ 掉落中不再棱角明显、落地后不再换一套外观。
/// 变长 ⇒ 用 `UpdateMeshGeometry`（顶点与索引都可变、**只上传用到的前缀**）；
/// "清空一个槽位"因此是零上传、零绘制（`usedIndexCount = 0`）。
///
/// 线程约定：只在逻辑线程（主线程）使用。
class RigidCollapseRuntime {
public:
    /// 建立网格池（**加载期**调用）。`capacityVerts` = 每个槽位的顶点数上限；
    /// 单个整体的外表面顶点数超出它时**只截断外观**（物理不受影响）并记日志 —— 见 `Spawn`。
    void Init(MeshRenderer& renderer, std::size_t slotCount, std::size_t capacityVerts);

    [[nodiscard]] std::size_t FreeSlots() const noexcept { return m_pool.size() - m_active.size(); }
    [[nodiscard]] std::size_t ActiveUnits() const noexcept { return m_active.size(); }
    [[nodiscard]] std::size_t TotalSpawned() const noexcept { return m_spawned; }
    [[nodiscard]] std::size_t TotalSettled() const noexcept { return m_settled; }
    [[nodiscard]] std::size_t TotalWrittenVoxels() const noexcept { return m_writtenVoxels; }
    [[nodiscard]] std::size_t TotalDroppedVoxels() const noexcept { return m_droppedVoxels; }
    /// T46：已被**惰性回写**（光球命中 / 池满腾位）的保留残骸数。
    [[nodiscard]] std::size_t TotalRetired() const noexcept { return m_retired; }
    /// T46：当前**保留中**的刚性残骸数（= 占用中的槽位里"不回写"的那部分）。
    [[nodiscard]] std::size_t RetainedUnits() const noexcept;
    [[nodiscard]] const std::vector<ActiveCollapseUnit>& Active() const noexcept { return m_active; }

    /// 把抽出的整体转成刚体并填好渲染网格。
    ///
    /// 失败（无空闲槽位 / 凸包构建失败 / 网格刷新失败）时**把体素按原样写回体积**并返回 false ——
    /// 绝不让体素"被抽出后又没有刚体可承载"（那会凭空消失，破坏世界自洽）。
    bool Spawn(PhysicsWorld& physics, DigVolumeWorld& volumes, MeshRenderer& renderer, const CollapseSpec& spec,
               CollapseUnit unit);

    /// 每固定步：读取位姿、累计"低于落定阈值"的步数；把**本步落定**的整体移出活跃集合并追加到 `settledOut`。
    ///
    /// T46：**刚性**整体（`unit.rigidDebris`）落定后**不移出、不回写** —— 只标记 `retained` 并继续刷位姿，
    /// 形状与外观因此保持不变；**散体**整体落定后照旧移出并交 `Writeback` 体素化。
    void Step(PhysicsWorld& physics, const CollapseSpec& spec, std::vector<ActiveCollapseUnit>& settledOut);

    /// 每帧：把活跃整体的位姿推给渲染器（不触碰顶点缓冲）。
    void SyncRender(MeshRenderer& renderer);

    /// 把落定的整体按**最终姿态**体素化回写到体积（委托 `WritebackCollapseUnit`），
    /// 并释放刚体与该网格槽位。返回回写结果（含 dirty 块）。
    [[nodiscard]] CollapseWriteback Writeback(PhysicsWorld& physics, DigVolumeWorld& volumes, MeshRenderer& renderer,
                                              const ActiveCollapseUnit& settled);

    /// ---- T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)：保留中的刚性残骸 ----

    /// 该点是否落在**某个保留中的刚性残骸**内（光球命中判定用）。
    ///
    /// 判据 = 该整体**局部 AABB**（随姿态旋转 ⇒ OBB）包含该点；凸包的真实边界比它小 ⇒ 可能"早一点"命中
    /// （[ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md) 后果 5）。
    /// 用途：让"看着是实心"的岩石残骸**挡住光球** —— 否则光球会穿过去（世界不自洽）。
    [[nodiscard]] bool ContainsRetainedPoint(const glm::dvec3& point) const noexcept;

    /// **惰性回写**：把 `point` 所在的保留残骸体素化回写为地形，并释放它的刚体与网格槽位。
    /// 返回 false 表示该点不在任何保留残骸内（无操作）。
    /// 用途（项目所有者选中）：**光球撞上岩石残骸 ⇒ 它才被体素化**，随后按常规被这次爆炸挖除 ⇒ 仍可挖。
    [[nodiscard]] bool RetireRetainedAt(PhysicsWorld& physics, DigVolumeWorld& volumes, MeshRenderer& renderer,
                                        const glm::dvec3& point, CollapseWriteback& out);

    /// **腾位**：把**最旧**的保留残骸惰性回写（网格池满时的兜底 —— 否则"新结构不再倒塌"）。
    /// 返回 false 表示当前没有保留中的残骸（无操作）。顺序确定：`m_active` 即生成序。
    [[nodiscard]] bool RetireOldestRetained(PhysicsWorld& physics, DigVolumeWorld& volumes, MeshRenderer& renderer,
                                            CollapseWriteback& out);

    /// 某体积块的**碰撞体被重建**（被挖 / 塌落）后，唤醒与它相交的保留刚性残骸。
    ///
    /// 为什么必须显式唤醒：Jolt 不会因"脚下的静态形状被改写"自动唤醒休眠体 —— 不唤醒就会出现
    /// "支撑被挖掉了、岩石却悬空不动"（违反 ADR 0015 决策一）。判据用**世界 AABB 相交**（保守：
    /// 宁可多唤醒一次，也不漏唤醒；多唤醒的代价只是它重新静止）。
    void AwakenIntersecting(const BlockCoord& block, PhysicsWorld& physics);

    /// 释放全部活跃刚体与网格槽位（退出前善后；无效句柄为无操作）。
    void Clear(PhysicsWorld& physics, MeshRenderer& renderer);

private:
    /// 槽位 → 网格句柄（加载期创建，容量固定；顶点容量 = `m_capacityVerts`，索引容量 = `m_capacityVerts / 4 × 6`）。
    std::vector<MeshHandle>         m_pool;
    std::vector<ActiveCollapseUnit> m_active;
    std::size_t                     m_capacityVerts  = 0;
    std::size_t                     m_capacityIndices = 0;

    std::size_t m_spawned        = 0;
    std::size_t m_settled        = 0;
    std::size_t m_writtenVoxels  = 0;
    std::size_t m_droppedVoxels  = 0;
    std::size_t m_retired        = 0;   ///< T46：已被惰性回写的保留残骸数

    /// 把一个整体网格化成**与地形同源的等值面**（`BuildCollapseUnitMesh`），并在超出槽位容量时
    /// **按整个四边形**截断（只影响外观；物理与回写不受影响）。
    [[nodiscard]] MeshData BuildUnitMesh(const CollapseUnit& unit) const;

    /// 找一个空闲槽位；没有则返回 false。
    [[nodiscard]] bool AcquireSlot(MeshHandle& meshOut);

    /// T46：把第 `index` 个（必须 `retained`）保留残骸惰性体素化回写，并释放刚体与槽位。
    [[nodiscard]] bool RetireRetained(std::size_t index, PhysicsWorld& physics, DigVolumeWorld& volumes,
                                      MeshRenderer& renderer, CollapseWriteback& out);
};

}  // namespace vx
