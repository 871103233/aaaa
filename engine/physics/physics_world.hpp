#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vx {

/// 把世界位置量化成**水平原点挡位**（纯函数）：`x` / `z` 各自向下取整到 `quantum` 的整数倍，`y` 恒为 `0`。
///
/// 用途（[ADR 0025](../../docs/adr/0025-large-world-coordinate-precision.md)）：由玩家位置决定
/// `PhysicsWorld` 的世界原点，避免原点每次微动都触发一次全体平移（挡位化 ⇒ 只有跨越挡位才 `SetWorldOrigin`）。
/// 前置条件：`quantum > 0`；否则返回 `(0, 0, 0)`。
[[nodiscard]] glm::dvec3 QuantizeHorizontalWorldOrigin(const glm::dvec3& position, double quantum) noexcept;

/// 物理世界：Jolt 生命周期的薄封装（作业系统 / 临时分配器 / 固定步 `Update`）。
///
/// 分层（SKILL §2 / 方案 §9.1）：本类位于 **engine 层**，只提供**通用**碰撞体与角色控制接口，
/// **不含任何地形 / 玩法专有类型**；Jolt 的**全部**类型隐藏在 pimpl 内，公共头不出现 Jolt 类型，
/// 依赖方向仍为 `world → engine`。
///
/// 线程约定：只在逻辑线程（主线程）使用。Jolt 的全局注册（分配器 / 类型工厂）是**进程级一次性**的，
/// 由本类内部以引用计数管理，多个实例可安全共存。
class PhysicsWorld final {
public:
    /// 不透明句柄；`0` 表示无效（与 `MeshHandle` 的约定一致）。
    using BodyHandle      = std::uint32_t;
    using CharacterHandle = std::uint32_t;

    /// 通用高度场碰撞体描述（与地形无关，任何规则高度网格都可使用）。
    ///
    /// 语义与 Jolt `HeightFieldShape` 一致：采样按**行主序** `y * sampleCount + x` 排列，
    /// 世界位置为 `(originX + x, samples[y * sampleCount + x], originZ + y)`。
    /// 采样数据只需在调用期间有效——构造时会被复制，调用返回后即可释放。
    ///
    /// 精度说明：本仓库的 vcpkg `joltphysics` 端口**未**开启 `JPH_DOUBLE_PRECISION`，
    /// 其 `RVec3` 实际是 `float`。因此 `originX` / `originZ` 虽以 `double` 传入（红线 6），
    /// 进入 Jolt 时会**显式**转换为 `JPH::Real`（当前为 `float`）；大坐标下物理精度受此限制。
    struct HeightFieldDesc {
        std::uint32_t sampleCount = 0;        ///< 每边采样数（须 `>= 2`）
        const float*  samples     = nullptr;  ///< `sampleCount²` 个高度（世界单位，行主序）
        double        originX     = 0.0;      ///< 采样 `(0, 0)` 的世界 X（`double`，红线 6）
        double        originZ     = 0.0;      ///< 采样 `(0, 0)` 的世界 Z（`double`，红线 6）
    };

    /// 通用静态盒体描述（与地形无关；任何轴对齐的静态阻挡都能使用）。
    ///
    /// `center` 为盒中心、`halfExtents` 为各轴半长（均须 `> 0`），均为世界空间、单位格。
    /// 精度说明同 `HeightFieldDesc`：进入 Jolt 时显式转换为 `JPH::Real`（当前为 `float`）。
    struct BoxDesc {
        glm::dvec3 center { 0.0 };       ///< 盒中心（世界，格）
        glm::dvec3 halfExtents { 0.5 };  ///< 各轴半长（格，须 `> 0`）
    };

    /// 通用**三角网**静态碰撞体描述（与地形无关；等值面网格、任何静态三角几何都能使用）。
    ///
    /// 顶点按**局部坐标**给出（网格自身的坐标原点），世界定位由 `origin*` 承担 ——
    /// 与渲染网格的约定一致（红线 6：世界定位留给 `double`，顶点保持小数值以保证 `float` 精度）。
    /// 顶点 / 索引数据只需在调用期间有效（构造时被复制）。
    ///
    /// 前置条件：`triangleCount >= 1`，且每个索引 `< vertexCount`（否则构造失败并返回 0）。
    struct MeshDesc {
        const float*      positions     = nullptr;  ///< `3 * vertexCount` 个局部坐标（x, y, z 依次）
        std::size_t       vertexCount   = 0;
        const std::uint32_t* indices    = nullptr;  ///< `3 * triangleCount` 个顶点索引
        std::size_t       triangleCount = 0;
        double            originX       = 0.0;      ///< 局部原点的世界 X（`double`，红线 6）
        double            originY       = 0.0;      ///< 局部原点的世界 Y
        double            originZ       = 0.0;      ///< 局部原点的世界 Z
    };

    /// 角色胶囊描述（**脚底**为原点，与 Jolt `CharacterVirtual` 约定一致）。
    struct CapsuleDesc {
        float      radius             = 0.3F;   ///< 胶囊半径（格）
        float      cylinderHalfHeight = 0.6F;   ///< 圆柱段半高（格）；总高 = `2 * (半高 + 半径)`
        float      maxSlopeAngleDeg   = 50.0F;  ///< 可正常行走的最大坡度（度）
        float      stepUpHeight       = 1.0F;   ///< **自动**上台阶高度（格）
        glm::dvec3 position { 0.0 };            ///< 初始脚底位置（世界，格）
    };

    /// 角色在某个固定步结束后的状态。
    struct CharacterState {
        glm::dvec3 position { 0.0 };   ///< 脚底位置（世界，`double`）
        glm::vec3  velocity { 0.0F };  ///< 线速度（格 / 秒）
        /// 是否**被支撑**（Jolt `OnGround` **或** `OnSteepGround`）。语义比 `walkableGround` 宽：
        /// 贴着垂直岩壁下滑时它为 `true`（角色确实被墙面撑着），但那**不是**能起跳的地面。
        bool       onGround = false;
        /// 是否站在**可行走**地面上（Jolt `EGroundState::OnGround`，**不含** `OnSteepGround`）。
        ///
        /// T54：**起跳门槛只能用这个** —— 业内口径一致（Unity `CharacterController.isGrounded` +
        /// `slopeLimit`、Unreal `Walking` / `WalkableFloorZ`、Jolt 官方示例 `GetGroundState() == OnGround`）。
        /// 用 `onGround` 放行起跳会让"贴着垂直岩壁反复按跳"变成无限爬墙（缺陷 T54）。
        bool       walkableGround = false;
        /// 地面法线（世界空间，由 Jolt 给出；**未着地**时为 `+Y` 占位）。
        glm::vec3  groundNormal { 0.0F, 1.0F, 0.0F };
    };

    /// 动态**凸包**刚体描述（T33「倒塌整体」）。
    ///
    /// `positions` 是**局部坐标**（刚体局部原点 = 这些点的坐标系原点，通常取分量质心），
    /// 世界定位由 `origin*` 承担（红线 6：世界定位用 `double`，局部顶点保持小数值）。
    /// 形状由这些点求**凸包** —— 凹形会被填平，见 [ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md) 后果 1。
    /// 前置条件：`positions` 非空、`pointCount >= 4`、`mass > 0`。
    struct ConvexHullDesc {
        const float* positions  = nullptr;  ///< `3 * pointCount` 个局部坐标（x, y, z 依次）
        std::size_t  pointCount = 0;
        double       originX    = 0.0;      ///< 局部原点的世界 X（`double`，红线 6）
        double       originY    = 0.0;      ///< 局部原点的世界 Y
        double       originZ    = 0.0;      ///< 局部原点的世界 Z
        float        mass       = 1.0F;     ///< 质量（须 `> 0`）
        float        friction    = 0.5F;    ///< 摩擦系数（0~1，来自玩法配置；越大越不容易滑）
        /// 弹性（0~1；`0` = 完全不回弹）。来自**材质表**（T43 / ADR 0016：岩略回弹、土 / 草几乎不回弹）。
        float        restitution = 0.0F;
        glm::vec3    linearVelocity { 0.0F };   ///< 初始线速度（格/秒）；用于"被炸飞"（T43 的爆心冲量）
        glm::vec3    angularVelocity { 0.0F };  ///< 初始角速度（rad/s）；用于模拟倒塌的**初始不对称**
        /// **初始姿态**（T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策二）。
        ///
        /// 默认单位四元数（= 与 T33 引入时逐位一致）。存在的唯一理由：T50 的"在碎块**自身补丁**上雕刻后
        /// 重建刚体"必须**在当前姿态下原地重建** —— 否则一块已经倒下的岩石被打中时会突然回正。
        glm::quat    rotation { 1.0F, 0.0F, 0.0F, 0.0F };
    };

    /// 动态刚体的位姿与速度。
    ///
    /// `position` 是**局部原点**的世界位置（`double`）—— 与 `ConvexHullDesc::origin*` 同一约定，
    /// 因此 `world = position + rotation * 局部坐标` 可直接用于渲染与体素化回写。
    /// （Jolt 内部以**质心**为位置，本类在这里把质心偏移折回局部原点，调用方无需关心。）
    struct RigidBodyState {
        glm::dvec3 position { 0.0 };
        glm::quat  rotation { 1.0F, 0.0F, 0.0F, 0.0F };
        glm::vec3  linearVelocity { 0.0F };
        glm::vec3  angularVelocity { 0.0F };
    };

    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    /// 固定步推进动态刚体（角色不属于刚体系统，需另行调用 `MoveCharacter`）。
    /// 前置条件：`dt > 0`。
    void Update(float dt);

    /// 设置物理世界的**重力**（对动态刚体生效；角色重力由 `MoveCharacter` 的入参决定，不读这里）。
    ///
    /// 为什么必须显式设置：Jolt 的默认重力是 `-9.81`，而本项目玩法层的重力是配置值（当前 24 格/秒²）——
    /// 不设置就会出现"角色与倒塌体各按一套重力下落"的不自洽（红线：世界内一致性）。
    /// 前置条件：各分量为有限值。下一次 `Update` 生效。
    void SetGravity(const glm::vec3& gravity) noexcept;

    /// **优化宽相位**（T79③；Jolt 官方口径：`PhysicsSystem::OptimizeBroadPhase`）。
    ///
    /// 何时必须调用（Jolt 文档原文）："needed only if you've added many bodies prior to calling `Update()`
    /// for the first time"。本项目**正是这个情形** —— 加载期一次性建好数百个静态体（地表高度场 +
    /// 可挖体积三角网），首个 `Update` 会把同一份建树工作**摊到随后若干帧**上（各帧多花一点 CPU）。
    /// 在**加载期收口处调用一次**即把这份工作提前做完，`Update` 的稳态帧时间因此更平（帧尖峰打点可见）。
    ///
    /// **不得每帧调用**（文档原文："Don't call this every frame"）—— 那是把本已摊平的工作重新集中。
    /// 也**不需要**在批量增删静态体之后反复调用：Jolt 的批量接口本身就会建出高效的包围体层次。
    void OptimizeBroadPhase();

    // ---- 大世界坐标精度（[ADR 0025](../../docs/adr/0025-large-world-coordinate-precision.md)）----

    /// 当前**水平世界原点**（`x` / `z` 有效；`y` 恒为 `0`）。
    ///
    /// 语义：**进出 Jolt 的水平位置 = 世界坐标 − 原点**；读取时再加回。对调用方**完全透明**
    /// （所有接口仍以世界坐标 `double` 收发）。默认 `(0, 0, 0)` ⇒ 与引入本项之前**逐位等价**。
    ///
    /// **为什么只重定基水平方向**：世界垂直范围仅 `0 ~ 512` 格，`float32` 精度绰绰有余；
    /// 且高度场采样本身就是**绝对高度**，对其做 Y 偏移会改变地形形状（故 Y 一律保持世界值）。
    [[nodiscard]] glm::dvec3 WorldOrigin() const noexcept;

    /// 设置水平世界原点，并把**全部刚体与角色**按 `旧原点 − 新原点` 平移（世界位置不变）。
    ///
    /// 用途：玩家远离当前原点时，把原点跳一档（建议用 `QuantizeHorizontalWorldOrigin`，512 格对齐），
    /// 使送进 Jolt 的坐标始终落在原点附近的小数值区间，避免单精度在大坐标下退化。
    ///
    /// 语义要点：① `y` 分量被忽略；② 新旧一致 ⇒ 无操作；③ **只改位置、不改速度与姿态**
    /// （动态刚体速度保持、不做额外激活）；④ 默认原点 ⇒ 与引入前逐位等价。
    void SetWorldOrigin(const glm::dvec3& origin) noexcept;

    // ---- 通用碰撞体 ----

    /// 创建一个静态高度场碰撞体。返回无效句柄表示创建失败（形状参数非法等），失败原因写入日志。
    [[nodiscard]] BodyHandle AddHeightField(const HeightFieldDesc& desc);

    /// 用新的采样**重建同一碰撞体**的形状（例如地形被笔刷改动后）。
    /// 返回 false 表示句柄无效或重建失败；失败时保留旧形状。
    bool UpdateHeightField(BodyHandle handle, const HeightFieldDesc& desc);

    /// 创建一个静态三角网碰撞体（例如可挖体积的等值面网格，ADR 0012）。
    /// 返回无效句柄表示创建失败（参数非法等），失败原因写入日志。
    [[nodiscard]] BodyHandle AddMesh(const MeshDesc& desc);

    /// 用新的三角网**重建同一碰撞体**的形状（挖除 / 塌落之后）。
    /// 网格为空（`triangleCount == 0`）返回 false —— 调用方应先 `RemoveBody`。
    /// 返回 false 表示句柄无效、网格为空或重建失败；失败时保留旧形状。
    bool UpdateMesh(BodyHandle handle, const MeshDesc& desc);

    /// 创建一个静态盒体碰撞体。返回无效句柄表示创建失败（半长非正等），失败原因写入日志。
    [[nodiscard]] BodyHandle AddStaticBox(const BoxDesc& desc);

    /// 创建一个**动态凸包**刚体（T33：倒塌整体）。返回无效句柄表示创建失败（参数非法 / 凸包构建失败）。
    ///
    /// 与上面的静态体共用同一套句柄表与对象层 ⇒ `RemoveBody` / `BodyCount` 通用；
    /// 由 `Update(dt)` 按重力与碰撞求解，位姿用 `GetRigidBodyState` 读回。
    /// 保留 Jolt 的默认**阻尼**（线 / 角各 0.05）与**休眠**：倒塌体落地后更快静止，利于落定判定。
    [[nodiscard]] BodyHandle AddDynamicConvexHull(const ConvexHullDesc& desc);

    /// 读取动态刚体的位姿（**局部原点**，见 `ConvexHullDesc`）与线 / 角速度；无效句柄返回默认（零）状态。
    [[nodiscard]] RigidBodyState GetRigidBodyState(BodyHandle handle) const noexcept;

    /// **动态刚体**的线段查询结果（T48 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）。
    struct RayCastHit {
        bool       hit = false;
        glm::dvec3 point { 0.0 };  ///< 命中点（世界坐标；落在**真实凸包表面**上，不再是手工 OBB 边界）
        BodyHandle body {};        ///< 命中刚体的句柄（`0` = 未命中）
    };

    /// 沿线段 `from → to` 查询**动态刚体**（倒塌中的整体）。
    ///
    /// 为什么需要（T48 / BUG4）：倒塌中的整体**体素已被抽出**（体积里是空的），只按密度判定会把
    /// "看着是实心"的整体误判为空气 ⇒ 光球穿过去 ⇒ 掉落中打不中。改由物理引擎回答，
    /// 命中点是**真实凸包表面**（不是手工 OBB 近似），且**覆盖"飞行中"这一态**（无需任何状态记账）。
    ///
    /// **静态地形 / 体积 / 角色不参与**（它们由玩法层按"谁画谁挡同源"的现行口径自行判定）。
    /// 成本与活跃刚体数无关（Jolt 宽相位 + `NarrowPhaseQuery`，有界）。
    /// 无命中 / 线段退化 / 无物理系统 ⇒ `hit = false`。
    [[nodiscard]] RayCastHit RayCastDynamic(const glm::dvec3& from, const glm::dvec3& to) const;

    /// **唤醒**一个动态刚体（T46 / [ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)）。
    ///
    /// 为什么需要：Jolt 的休眠体**不会**因为"它脚下的静态形状被改写"自动醒来；而本项目会挖掉地形 / 残骸下方的
    /// 体积碰撞体 —— 若不显式唤醒，保留中的刚性残骸就会**悬空不动**（违反"承重被破坏 ⇒ 上部不得悬空"）。
    /// 静态体 / 无效句柄为无操作。
    void ActivateBody(BodyHandle handle) noexcept;

    /// 移除一个碰撞体；无效句柄为无操作。
    void RemoveBody(BodyHandle handle) noexcept;

    [[nodiscard]] std::size_t BodyCount() const noexcept;

    // ---- 角色（胶囊，`CharacterVirtual`）----

    /// 创建一个胶囊角色。返回无效句柄表示创建失败。
    [[nodiscard]] CharacterHandle CreateCharacter(const CapsuleDesc& desc);

    /// 移除一个角色；无效句柄为无操作。
    void RemoveCharacter(CharacterHandle handle) noexcept;

    /// 设置角色速度（格 / 秒）。水平分量由玩法层给出，竖直分量含跳跃冲量。
    void SetCharacterVelocity(CharacterHandle handle, const glm::vec3& velocity) noexcept;

    /// 直接把角色**脚底**放到 `position` 并清零速度；无效句柄为无操作。
    ///
    /// 用途：地形（静态高度场）在角色脚下被抬高后，Jolt 的 `CharacterVirtual` **不会**被静态形状
    /// 变化顶出——一旦角色被新地表埋住，支撑判定失效，它会在重力下穿过高度场。此时由玩法层把角色
    /// 放回新地表（"地形升起时骑上去"，与放置方块把角色顶起的惯例一致）。
    /// 该操作会瞬移角色，调用方需自行把相机吸附到新位置以避免插值拖影。
    void SetCharacterPosition(CharacterHandle handle, const glm::dvec3& position) noexcept;

    /// 在固定步内推进角色：着地时清除下沉速度、空中时累加重力，再做滑动 / 贴地 / **自动上台阶**。
    /// 前置条件：`dt > 0`。
    void MoveCharacter(CharacterHandle handle, float dt, const glm::vec3& gravity);

    /// 查询角色状态；无效句柄返回默认（零）状态。
    [[nodiscard]] CharacterState GetCharacterState(CharacterHandle handle) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
