#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vx {

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
        bool       onGround = false;   ///< 是否被支撑（着地或站在过陡坡上）
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

    // ---- 通用碰撞体 ----

    /// 创建一个静态高度场碰撞体。返回无效句柄表示创建失败（形状参数非法等），失败原因写入日志。
    [[nodiscard]] BodyHandle AddHeightField(const HeightFieldDesc& desc);

    /// 用新的采样**重建同一碰撞体**的形状（例如地形被笔刷改动后）。
    /// 返回 false 表示句柄无效或重建失败；失败时保留旧形状。
    bool UpdateHeightField(BodyHandle handle, const HeightFieldDesc& desc);

    /// 创建一个静态盒体碰撞体。返回无效句柄表示创建失败（半长非正等），失败原因写入日志。
    [[nodiscard]] BodyHandle AddStaticBox(const BoxDesc& desc);

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
