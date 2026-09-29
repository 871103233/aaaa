// Jolt 薄封装实现。Jolt 头文件**只**出现在本 .cpp 内（见 physics_world.hpp 的分层说明）。

#include "physics/physics_world.hpp"

#include "core/log.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace vx {
namespace {

// ---------------------------------------------------------------
// 物理规模参数（V0.1 小场景：若干静态地表 tile + 1 个胶囊角色 + T33 的少量动态倒塌体）
// ---------------------------------------------------------------

constexpr JPH::uint kMaxBodies            = 4096;
constexpr JPH::uint kMaxBodyPairs         = 4096;
constexpr JPH::uint kMaxContactConstraints = 1024;
constexpr JPH::uint kTempAllocatorSize    = 8 * 1024 * 1024;  ///< 临时分配器 8 MB
constexpr int       kPhysicsJobThreads    = 2;                ///< 后台作业线程数（保留 1 核给主线程）

/// 高度场分块大小。Jolt 要求 `[2, 8]`，且采样数会被**向上取整**到分块大小的整数倍：
/// 地表 tile 是 65 个采样（64 列 + 1 层共享边界），只有 5 能整除它（65 = 13 × 5），
/// 否则多出的一列会被填成"无碰撞"空洞。
constexpr JPH::uint kHeightFieldBlockSize = 5;

/// 角色贴地探测距离（格）：略大于一个固定步的下落量，避免走小台阶时腾空。
constexpr float kStickToFloorStepDownBlocks = 0.5F;

/// 上台阶的**最小前进步长**系数：`最小前进步长 = 系数 × 胶囊半径`。
/// 必须**大于半径**（取 1.5 倍），否则低速逼近台阶时前进步长太小，胶囊中心越不过台阶棱，
/// 下探会落回原地，导致 1 格台阶上不去。
constexpr float kMinStepForwardRadiusFactor = 1.5F;

/// 上台阶的前进试探步长（格）：下探到过陡法线时，再往前探一段判断是否有可站立平面。
constexpr float kWalkStairsStepForwardTestBlocks = 0.15F;

/// 上台阶高度相对配置值的**余量**（格）：抵消碰撞容差 / 贴地余量，保证正好 1 格的台阶能上去。
constexpr float kStepUpMarginBlocks = 0.05F;

/// 引擎默认自动上台阶高度（格）：1 格（V0.1 验收口径）。
constexpr float kDefaultCharacterStepUpHeightBlocks = 1.0F;

// ---------------------------------------------------------------
// 对象层 / Broadphase 层：V0.1 只有一个默认层，过滤一律放行
// ---------------------------------------------------------------

constexpr JPH::ObjectLayer kObjectLayerStatic   = 0;
constexpr JPH::BroadPhaseLayer kBroadPhaseStatic(0);

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return 1; }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        (void)inLayer;
        return kBroadPhaseStatic;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        (void)inLayer;
        return "static";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        (void)inLayer1;
        (void)inLayer2;
        return true;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
        (void)inLayer1;
        (void)inLayer2;
        return true;
    }
};

// ---------------------------------------------------------------
// Jolt 进程级全局注册（分配器 / 类型工厂 / 日志钩子）
// ---------------------------------------------------------------

int g_joltUsers = 0;  ///< 仅主线程访问（物理只在逻辑线程推进）

void jolt_trace(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    VX_LOG_DEBUG("Jolt: %s", buffer);
}

void acquire_jolt() {
    if (g_joltUsers++ != 0) {
        return;
    }
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    JPH::Trace = &jolt_trace;
    VX_LOG_INFO("Jolt 物理已初始化（全局注册完成）");
}

void release_jolt() {
    if (--g_joltUsers != 0) {
        return;
    }
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

/// 从采样描述构造一个 `HeightFieldShape`；失败返回空 `RefConst`。
[[nodiscard]] JPH::ShapeRefC build_height_field_shape(const PhysicsWorld::HeightFieldDesc& desc) {
    if (desc.samples == nullptr || desc.sampleCount < 2) {
        VX_LOG_ERROR("高度场参数非法：sampleCount=%u，samples=%s", desc.sampleCount,
                     (desc.samples != nullptr) ? "非空" : "空");
        return {};
    }

    // 采样是绝对高度（世界单位），因此偏移为零、缩放为一；世界定位由静态刚体的位置承担。
    JPH::HeightFieldShapeSettings settings(desc.samples, JPH::Vec3::sZero(), JPH::Vec3::sOne(), desc.sampleCount);
    settings.mBlockSize = kHeightFieldBlockSize;

    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        VX_LOG_ERROR("构造 HeightFieldShape 失败：%s", result.GetError().c_str());
        return {};
    }
    return result.Get();
}

/// 从三角网描述构造一个 `MeshShape`；失败返回空 `RefConst`。
///
/// 用于可挖体积的等值面网格（ADR 0012）：顶点是**块内局部坐标**，世界定位由静态刚体的位置承担。
[[nodiscard]] JPH::ShapeRefC build_mesh_shape(const PhysicsWorld::MeshDesc& desc) {
    if (desc.positions == nullptr || desc.indices == nullptr || desc.vertexCount == 0 || desc.triangleCount == 0) {
        VX_LOG_ERROR("三角网参数非法：顶点 %zu、三角形 %zu，positions=%s，indices=%s", desc.vertexCount,
                     desc.triangleCount, (desc.positions != nullptr) ? "非空" : "空",
                     (desc.indices != nullptr) ? "非空" : "空");
        return {};
    }

    JPH::VertexList vertices;
    vertices.reserve(desc.vertexCount);
    for (std::size_t i = 0; i < desc.vertexCount; ++i) {
        vertices.push_back(JPH::Float3(desc.positions[i * 3 + 0], desc.positions[i * 3 + 1],
                                       desc.positions[i * 3 + 2]));
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(desc.triangleCount);
    for (std::size_t t = 0; t < desc.triangleCount; ++t) {
        const std::uint32_t i0 = desc.indices[t * 3 + 0];
        const std::uint32_t i1 = desc.indices[t * 3 + 1];
        const std::uint32_t i2 = desc.indices[t * 3 + 2];
        if (i0 >= desc.vertexCount || i1 >= desc.vertexCount || i2 >= desc.vertexCount) {
            VX_LOG_ERROR("三角网索引越界：三角形 %zu 的索引 (%u, %u, %u)，顶点数 %zu", t, i0, i1, i2,
                         desc.vertexCount);
            return {};
        }
        triangles.push_back(JPH::IndexedTriangle(i0, i1, i2, /*inMaterialIndex=*/0));
    }

    // 构造时 Jolt 会自动 `Sanitize`（去掉退化 / 重复三角形）；返回空列表时视为失败。
    //
    // T30：可挖体积的块**每次挖除 / 塌落都要整块重建形状**（Jolt 的 `MeshShape` 不可变），
    // 故用 `FavorBuildSpeed` 换构建速度 —— 代价是该形状的运行期查询稍慢，而这里是静态地形，
    // 查询余量很大（实测本工程单块重建 16 ms → 见 `tests/volume_collision_test.cpp` 的 T30 基准）。
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        VX_LOG_ERROR("构造 MeshShape 失败：%s", result.GetError().c_str());
        return {};
    }
    return result.Get();
}

/// 从点云构造一个 `ConvexHullShape`；失败返回空 `RefConst`。
///
/// 用于 T33 的"倒塌整体"（[ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md)）：
/// 顶点是**局部坐标**（分量质心为原点），世界定位由刚体的位置承担。
/// Jolt 会求这些点的凸包 ⇒ **凹形被填平**（已知限制，ADR 0015 后果 1）。
[[nodiscard]] JPH::ShapeRefC build_convex_hull_shape(const PhysicsWorld::ConvexHullDesc& desc) {
    if (desc.positions == nullptr || desc.pointCount < 4) {
        VX_LOG_ERROR("凸包参数非法：点数 %zu（须 ≥ 4），positions=%s", desc.pointCount,
                     (desc.positions != nullptr) ? "非空" : "空");
        return {};
    }

    JPH::Array<JPH::Vec3> points;
    points.reserve(desc.pointCount);
    for (std::size_t i = 0; i < desc.pointCount; ++i) {
        points.push_back(JPH::Vec3(desc.positions[i * 3 + 0], desc.positions[i * 3 + 1],
                                   desc.positions[i * 3 + 2]));
    }

    JPH::ConvexHullShapeSettings settings(points);
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        VX_LOG_ERROR("构造 ConvexHullShape 失败：%s", result.GetError().c_str());
        return {};
    }
    return result.Get();
}

}  // namespace

struct PhysicsWorld::Impl {
    /// 一个角色的运行时条目（含用于推导上台阶参数的胶囊几何）。
    struct CharacterEntry {
        std::unique_ptr<JPH::CharacterVirtual> character;
        float                                  radius       = 0.3F;
        float                                  stepUpHeight = kDefaultCharacterStepUpHeightBlocks;
    };

    BroadPhaseLayerInterfaceImpl     broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseFilter;
    ObjectLayerPairFilterImpl        objectLayerPairFilter;

    std::unique_ptr<JPH::TempAllocatorImpl>   tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    std::unique_ptr<JPH::PhysicsSystem>       system;

    std::vector<JPH::BodyID>    bodies;  ///< 槽位 → BodyID；已释放槽位保存无效 ID
    /// 与 `bodies` 同槽位：该刚体**形状局部原点相对质心**的偏移。
    ///
    /// Jolt 的刚体位置存的是**质心**（`Body::mPosition`），而本类的对外约定是**局部原点**
    /// （见 `ConvexHullDesc` / `RigidBodyState`）⇒ 创建时把偏移补进位置，读回时再减掉。
    /// 静态体恒为零。
    std::vector<JPH::Vec3>      bodyComLocals;
    std::vector<std::uint32_t>  freeBodySlots;
    std::vector<CharacterEntry> characters;

    /// 把一个成功创建的刚体登记进槽位表，返回句柄（`slot + 1`）。
    [[nodiscard]] PhysicsWorld::BodyHandle RegisterBody(const JPH::BodyID& bodyID,
                                                       const JPH::Vec3& comLocal = JPH::Vec3::sZero()) {
        std::uint32_t slot = 0;
        if (!freeBodySlots.empty()) {
            slot = freeBodySlots.back();
            freeBodySlots.pop_back();
            bodies[slot] = bodyID;
        } else {
            slot = static_cast<std::uint32_t>(bodies.size());
            bodies.push_back(bodyID);
            bodyComLocals.push_back(comLocal);
        }
        bodyComLocals[slot] = comLocal;
        return slot + 1;
    }
};

/// T48：只让**动态刚体**参与查询（静态地形 / 体积 / 角色由玩法层按"谁画谁挡同源"的现行口径自行判定）。
class DynamicOnlyBodyFilter final : public JPH::BodyFilter {
public:
    [[nodiscard]] bool ShouldCollide(const JPH::BodyID&) const override { return true; }

    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        return body.GetMotionType() == JPH::EMotionType::Dynamic;
    }
};

PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {
    acquire_jolt();

    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(kTempAllocatorSize);
    m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                                   kPhysicsJobThreads);

    m_impl->system = std::make_unique<JPH::PhysicsSystem>();
    m_impl->system->Init(kMaxBodies, /*inNumBodyMutexes=*/0, kMaxBodyPairs, kMaxContactConstraints,
                         m_impl->broadPhaseLayerInterface, m_impl->objectVsBroadPhaseFilter,
                         m_impl->objectLayerPairFilter);
}

PhysicsWorld::~PhysicsWorld() {
    // 角色的生命周期必须在系统之前结束。
    m_impl->characters.clear();
    m_impl->bodies.clear();
    m_impl.reset();
    release_jolt();
}

void PhysicsWorld::Update(float dt) {
    const JPH::EPhysicsUpdateError error =
        m_impl->system->Update(dt, /*inCollisionSteps=*/1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        VX_LOG_WARN("Jolt PhysicsSystem::Update 返回错误码 %d", static_cast<int>(error));
    }
}

void PhysicsWorld::SetGravity(const glm::vec3& gravity) noexcept {
    m_impl->system->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
}

void PhysicsWorld::OptimizeBroadPhase() {
    // T79③：把"加载期一次性加了几百个静态体"的建造树工作提前做完（详见头文件注释）。
    // 这是**加载期收口**的一次性调用，不是每帧工作 ⇒ 不违反"每帧工作不得与总量成正比"。
    m_impl->system->OptimizeBroadPhase();
}

PhysicsWorld::BodyHandle PhysicsWorld::AddHeightField(const HeightFieldDesc& desc) {
    const JPH::ShapeRefC shape = build_height_field_shape(desc);
    if (shape == nullptr) {
        return 0;
    }

    JPH::BodyCreationSettings bodySettings(shape,
                                           JPH::RVec3(static_cast<JPH::Real>(desc.originX), JPH::Real(0.0),
                                                      static_cast<JPH::Real>(desc.originZ)),
                                           JPH::Quat::sIdentity(), JPH::EMotionType::Static, kObjectLayerStatic);
    const JPH::BodyID bodyID =
        m_impl->system->GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    if (bodyID.IsInvalid()) {
        VX_LOG_ERROR("创建高度场刚体失败");
        return 0;
    }
    return m_impl->RegisterBody(bodyID);
}

PhysicsWorld::BodyHandle PhysicsWorld::AddStaticBox(const BoxDesc& desc) {
    if (desc.halfExtents.x <= 0.0 || desc.halfExtents.y <= 0.0 || desc.halfExtents.z <= 0.0) {
        VX_LOG_ERROR("静态盒体参数非法：半长必须为正（%g, %g, %g）", desc.halfExtents.x, desc.halfExtents.y,
                     desc.halfExtents.z);
        return 0;
    }

    const JPH::ShapeRefC shape =
        new JPH::BoxShape(JPH::Vec3(static_cast<float>(desc.halfExtents.x), static_cast<float>(desc.halfExtents.y),
                                    static_cast<float>(desc.halfExtents.z)));
    JPH::BodyCreationSettings bodySettings(shape,
                                           JPH::RVec3(static_cast<JPH::Real>(desc.center.x),
                                                      static_cast<JPH::Real>(desc.center.y),
                                                      static_cast<JPH::Real>(desc.center.z)),
                                           JPH::Quat::sIdentity(), JPH::EMotionType::Static, kObjectLayerStatic);
    const JPH::BodyID bodyID =
        m_impl->system->GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    if (bodyID.IsInvalid()) {
        VX_LOG_ERROR("创建静态盒体刚体失败");
        return 0;
    }
    return m_impl->RegisterBody(bodyID);
}

bool PhysicsWorld::UpdateHeightField(BodyHandle handle, const HeightFieldDesc& desc) {
    if (handle == 0 || handle > m_impl->bodies.size()) {
        return false;
    }
    const JPH::BodyID bodyID = m_impl->bodies[handle - 1];
    if (bodyID.IsInvalid()) {
        return false;
    }

    const JPH::ShapeRefC shape = build_height_field_shape(desc);
    if (shape == nullptr) {
        return false;  // 保留旧形状
    }

    m_impl->system->GetBodyInterface().SetShape(bodyID, shape, /*inUpdateMassProperties=*/false,
                                                JPH::EActivation::DontActivate);
    return true;
}

PhysicsWorld::BodyHandle PhysicsWorld::AddMesh(const MeshDesc& desc) {
    const JPH::ShapeRefC shape = build_mesh_shape(desc);
    if (shape == nullptr) {
        return 0;
    }

    JPH::BodyCreationSettings bodySettings(
        shape, JPH::RVec3(static_cast<JPH::Real>(desc.originX), static_cast<JPH::Real>(desc.originY),
                          static_cast<JPH::Real>(desc.originZ)),
        JPH::Quat::sIdentity(), JPH::EMotionType::Static, kObjectLayerStatic);
    const JPH::BodyID bodyID =
        m_impl->system->GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    if (bodyID.IsInvalid()) {
        VX_LOG_ERROR("创建三角网刚体失败");
        return 0;
    }
    return m_impl->RegisterBody(bodyID);
}

bool PhysicsWorld::UpdateMesh(BodyHandle handle, const MeshDesc& desc) {
    if (handle == 0 || handle > m_impl->bodies.size()) {
        return false;
    }
    const JPH::BodyID bodyID = m_impl->bodies[handle - 1];
    if (bodyID.IsInvalid()) {
        return false;
    }

    const JPH::ShapeRefC shape = build_mesh_shape(desc);
    if (shape == nullptr) {
        return false;  // 保留旧形状
    }

    m_impl->system->GetBodyInterface().SetShape(bodyID, shape, /*inUpdateMassProperties=*/false,
                                                JPH::EActivation::DontActivate);
    return true;
}

PhysicsWorld::BodyHandle PhysicsWorld::AddDynamicConvexHull(const ConvexHullDesc& desc) {
    if (!(desc.mass > 0.0F)) {
        VX_LOG_ERROR("动态凸包参数非法：质量必须为正（%g）", static_cast<double>(desc.mass));
        return 0;
    }
    const JPH::ShapeRefC shape = build_convex_hull_shape(desc);
    if (shape == nullptr) {
        return 0;
    }

    // Jolt 的刚体位置 = **质心**；本类对外以**局部原点**为准（`ConvexHullDesc::origin*`）⇒
    // 这里把质心偏移补上。T50 起可带**初始姿态**（`desc.rotation`）⇒ 质心偏移必须先旋转到世界方向
    // （T50 的"雕刻后原地重建刚体"用它：已经倒下的岩石不该因为重建而回正）。
    const JPH::Quat  rotation = JPH::Quat(desc.rotation.x, desc.rotation.y, desc.rotation.z, desc.rotation.w);
    const JPH::Vec3  comLocal = shape->GetCenterOfMass();
    const JPH::Vec3  comWorld = rotation * comLocal;

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(static_cast<JPH::Real>(desc.originX) + comWorld.GetX(),
                   static_cast<JPH::Real>(desc.originY) + comWorld.GetY(),
                   static_cast<JPH::Real>(desc.originZ) + comWorld.GetZ()),
        rotation, JPH::EMotionType::Dynamic, kObjectLayerStatic);
    bodySettings.mAllowSleeping              = true;  // 落定后由 Jolt 休眠（省 CPU）
    bodySettings.mFriction                   = desc.friction;
    bodySettings.mRestitution                = desc.restitution;  // 来自材质表（T43 / ADR 0016）
    bodySettings.mOverrideMassProperties     = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = desc.mass;
    // 初速：线速度来自"爆心冲量"（被炸飞），角速度来自冲量折算或人工不对称（T43）。
    bodySettings.mLinearVelocity = JPH::Vec3(desc.linearVelocity.x, desc.linearVelocity.y, desc.linearVelocity.z);
    bodySettings.mAngularVelocity =
        JPH::Vec3(desc.angularVelocity.x, desc.angularVelocity.y, desc.angularVelocity.z);

    const JPH::BodyID bodyID =
        m_impl->system->GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::Activate);
    if (bodyID.IsInvalid()) {
        VX_LOG_ERROR("创建动态凸包刚体失败（点数 %zu）", desc.pointCount);
        return 0;
    }
    return m_impl->RegisterBody(bodyID, comLocal);
}

PhysicsWorld::RigidBodyState PhysicsWorld::GetRigidBodyState(BodyHandle handle) const noexcept {
    RigidBodyState state;
    if (handle == 0 || handle > m_impl->bodies.size()) {
        return state;
    }
    const JPH::BodyID bodyID = m_impl->bodies[handle - 1];
    if (bodyID.IsInvalid()) {
        return state;
    }
    const JPH::BodyInterface& bodyInterface = m_impl->system->GetBodyInterface();

    JPH::RVec3 centerOfMass;
    JPH::Quat  rotation;
    bodyInterface.GetPositionAndRotation(bodyID, centerOfMass, rotation);
    const JPH::Vec3 linear  = bodyInterface.GetLinearVelocity(bodyID);
    const JPH::Vec3 angular = bodyInterface.GetAngularVelocity(bodyID);

    // 质心 → 局部原点（与 `Body::GetWorldTransform()` 同式：减去"旋转后的质心局部偏移"）。
    const JPH::Vec3 localOffset = rotation * m_impl->bodyComLocals[handle - 1];
    state.position = glm::dvec3(centerOfMass.GetX() - localOffset.GetX(),
                               centerOfMass.GetY() - localOffset.GetY(),
                               centerOfMass.GetZ() - localOffset.GetZ());
    state.rotation = glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    state.linearVelocity  = glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ());
    state.angularVelocity = glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ());
    return state;
}

PhysicsWorld::RayCastHit PhysicsWorld::RayCastDynamic(const glm::dvec3& from, const glm::dvec3& to) const {
    RayCastHit result;
    if (m_impl == nullptr || m_impl->system == nullptr) {
        return result;
    }
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double dz = to.z - from.z;
    if (dx * dx + dy * dy + dz * dz <= 0.0) {
        return result;  // 退化线段
    }

    // Jolt 的 `RRayCast` 约定：`inDirection` 是"起点 → 终点"的**非单位**向量 ⇒ `mFraction ∈ [0, 1]`。
    const JPH::RRayCast ray { JPH::RVec3(static_cast<JPH::Real>(from.x), static_cast<JPH::Real>(from.y),
                                         static_cast<JPH::Real>(from.z)),
                              JPH::Vec3(static_cast<JPH::Real>(dx), static_cast<JPH::Real>(dy),
                                        static_cast<JPH::Real>(dz)) };
    JPH::RayCastResult       hit;
    const DynamicOnlyBodyFilter filter;
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) {
        return result;
    }
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    result.hit             = true;
    result.point = glm::dvec3(static_cast<double>(point.GetX()), static_cast<double>(point.GetY()),
                              static_cast<double>(point.GetZ()));
    // BodyID → 句柄（槽位表很小，线性扫描即可；与 `RegisterBody` 的 `slot + 1` 约定一致）。
    for (std::size_t slot = 0; slot < m_impl->bodies.size(); ++slot) {
        if (m_impl->bodies[slot] == hit.mBodyID) {
            result.body = static_cast<BodyHandle>(slot + 1U);
            break;
        }
    }
    return result;
}

void PhysicsWorld::ActivateBody(BodyHandle handle) noexcept {
    if (handle == 0 || handle > m_impl->bodies.size()) {
        return;
    }
    const JPH::BodyID bodyID = m_impl->bodies[handle - 1];
    if (bodyID.IsInvalid()) {
        return;
    }
    m_impl->system->GetBodyInterface().ActivateBody(bodyID);
}

void PhysicsWorld::RemoveBody(BodyHandle handle) noexcept {
    if (handle == 0 || handle > m_impl->bodies.size()) {
        return;
    }
    const std::uint32_t slot = handle - 1;
    const JPH::BodyID   bodyID = m_impl->bodies[slot];
    if (bodyID.IsInvalid()) {
        return;
    }
    JPH::BodyInterface& bodyInterface = m_impl->system->GetBodyInterface();
    bodyInterface.RemoveBody(bodyID);
    bodyInterface.DestroyBody(bodyID);
    m_impl->bodies[slot] = JPH::BodyID();
    m_impl->bodyComLocals[slot] = JPH::Vec3::sZero();  // 槽位复用前清掉质心偏移（静态体为零）
    m_impl->freeBodySlots.push_back(slot);
}

std::size_t PhysicsWorld::BodyCount() const noexcept {
    return m_impl->bodies.size() - m_impl->freeBodySlots.size();
}

PhysicsWorld::CharacterHandle PhysicsWorld::CreateCharacter(const CapsuleDesc& desc) {
    JPH::CharacterVirtualSettings settings;
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(desc.maxSlopeAngleDeg);

    // 用 RotatedTranslatedShape 把胶囊中心抬高到"脚底在原点"（Jolt 的 `CharacterVirtual` 约定）。
    const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(desc.cylinderHalfHeight, desc.radius);
    JPH::ShapeSettings::ShapeResult shapeResult =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0F, desc.cylinderHalfHeight + desc.radius, 0.0F),
                                            JPH::Quat::sIdentity(), capsule)
            .Create();
    if (shapeResult.HasError()) {
        VX_LOG_ERROR("构造角色胶囊失败：%s", shapeResult.GetError().c_str());
        return 0;
    }
    settings.mShape = shapeResult.Get();

    auto character = std::make_unique<JPH::CharacterVirtual>(
        &settings,
        JPH::RVec3(static_cast<JPH::Real>(desc.position.x), static_cast<JPH::Real>(desc.position.y),
                   static_cast<JPH::Real>(desc.position.z)),
        JPH::Quat::sIdentity(), m_impl->system.get());

    Impl::CharacterEntry entry;
    entry.character    = std::move(character);
    entry.radius       = desc.radius;
    entry.stepUpHeight = (desc.stepUpHeight > 0.0F) ? desc.stepUpHeight : kDefaultCharacterStepUpHeightBlocks;

    const std::uint32_t handle = static_cast<std::uint32_t>(m_impl->characters.size()) + 1;
    m_impl->characters.push_back(std::move(entry));
    return handle;
}

void PhysicsWorld::RemoveCharacter(CharacterHandle handle) noexcept {
    if (handle == 0 || handle > m_impl->characters.size()) {
        return;
    }
    m_impl->characters[handle - 1].character.reset();
}

void PhysicsWorld::SetCharacterVelocity(CharacterHandle handle, const glm::vec3& velocity) noexcept {
    if (handle == 0 || handle > m_impl->characters.size()) {
        return;
    }
    if (JPH::CharacterVirtual* character = m_impl->characters[handle - 1].character.get(); character != nullptr) {
        character->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
    }
}

void PhysicsWorld::SetCharacterPosition(CharacterHandle handle, const glm::dvec3& position) noexcept {
    if (handle == 0 || handle > m_impl->characters.size()) {
        return;
    }
    JPH::CharacterVirtual* character = m_impl->characters[handle - 1].character.get();
    if (character == nullptr) {
        return;
    }
    character->SetPosition(JPH::RVec3(static_cast<JPH::Real>(position.x), static_cast<JPH::Real>(position.y),
                                      static_cast<JPH::Real>(position.z)));
    character->SetLinearVelocity(JPH::Vec3::sZero());
}

void PhysicsWorld::MoveCharacter(CharacterHandle handle, float dt, const glm::vec3& gravity) {
    if (dt <= 0.0F || handle == 0 || handle > m_impl->characters.size()) {
        return;
    }
    Impl::CharacterEntry&  entry     = m_impl->characters[handle - 1];
    JPH::CharacterVirtual* character = entry.character.get();
    if (character == nullptr) {
        return;
    }

    const JPH::Vec3 gravityVector(gravity.x, gravity.y, gravity.z);

    // 重力由物理层统一施加；玩法层只负责水平速度与跳跃冲量。
    JPH::Vec3 velocity = character->GetLinearVelocity();
    if (character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) {
        if (velocity.GetY() < 0.0F) {
            velocity.SetY(0.0F);  // 着地时不允许向下累积速度，避免"粘"在斜坡上
        }
    } else {
        velocity += gravityVector * dt;
    }
    character->SetLinearVelocity(velocity);

    // 贴地 + 上台阶的设置按**该角色的胶囊几何**推导（多角色共存时互不干扰）。
    JPH::CharacterVirtual::ExtendedUpdateSettings extendedSettings;
    extendedSettings.mStickToFloorStepDown = JPH::Vec3(0.0F, -kStickToFloorStepDownBlocks, 0.0F);
    extendedSettings.mWalkStairsStepUp =
        JPH::Vec3(0.0F, entry.stepUpHeight + kStepUpMarginBlocks, 0.0F);
    extendedSettings.mWalkStairsMinStepForward = entry.radius * kMinStepForwardRadiusFactor;
    extendedSettings.mWalkStairsStepForwardTest = kWalkStairsStepForwardTestBlocks;
    extendedSettings.mWalkStairsStepDownExtra   = JPH::Vec3::sZero();

    // 默认过滤器（全放行）即可满足 V0.1 的单层世界。
    const JPH::BroadPhaseLayerFilter broadPhaseFilter;
    const JPH::ObjectLayerFilter    objectLayerFilter;
    const JPH::BodyFilter           bodyFilter;
    const JPH::ShapeFilter          shapeFilter;

    character->ExtendedUpdate(dt, gravityVector, extendedSettings, broadPhaseFilter, objectLayerFilter, bodyFilter,
                              shapeFilter, *m_impl->tempAllocator);
}

PhysicsWorld::CharacterState PhysicsWorld::GetCharacterState(CharacterHandle handle) const noexcept {
    CharacterState state;
    if (handle == 0 || handle > m_impl->characters.size()) {
        return state;
    }
    const JPH::CharacterVirtual* character = m_impl->characters[handle - 1].character.get();
    if (character == nullptr) {
        return state;
    }

    const JPH::RVec3 position = character->GetPosition();
    const JPH::Vec3  velocity = character->GetLinearVelocity();
    state.position = glm::dvec3(position.GetX(), position.GetY(), position.GetZ());
    state.velocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());

    // T54：把 Jolt 的**三态**如实映射出来 —— 不能把 `OnSteepGround` 与 `OnGround` 合成一个布尔。
    //   `onGround`       = 被支撑（含"贴着过陡坡 / 垂直壁"）—— 供"落地自检 / 是否与地面接触"使用；
    //   `walkableGround` = 站在可行走地面（**只有** `OnGround`）—— 玩法层的**起跳门槛**只能用这个，
    //                      否则贴着垂直岩壁下滑时每一步都满足起跳条件 ⇒ 无限爬墙（缺陷 T54）。
    //   地面法线一并给出：它是"可行走"的几何依据（`maxSlopeAngleDeg` 由 Jolt 在 `ExtendedUpdate` 内判定），
    //   调用方（相机 / 手感）需要时可直接读，不必再自己投射射线。
    const JPH::CharacterBase::EGroundState groundState = character->GetGroundState();
    state.onGround       = (groundState == JPH::CharacterBase::EGroundState::OnGround ||
                      groundState == JPH::CharacterBase::EGroundState::OnSteepGround);
    state.walkableGround = (groundState == JPH::CharacterBase::EGroundState::OnGround);
    if (groundState != JPH::CharacterBase::EGroundState::InAir) {
        const JPH::Vec3 normal = character->GetGroundNormal();
        state.groundNormal     = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
    }
    return state;
}

}  // namespace vx
