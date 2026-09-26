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
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
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
// 物理规模参数（V0.1 小场景：若干静态地表 tile + 1 个胶囊角色，无动态刚体）
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
    std::vector<std::uint32_t>  freeBodySlots;
    std::vector<CharacterEntry> characters;
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

    std::uint32_t slot = 0;
    if (!m_impl->freeBodySlots.empty()) {
        slot = m_impl->freeBodySlots.back();
        m_impl->freeBodySlots.pop_back();
        m_impl->bodies[slot] = bodyID;
    } else {
        slot = static_cast<std::uint32_t>(m_impl->bodies.size());
        m_impl->bodies.push_back(bodyID);
    }
    return slot + 1;
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

    const JPH::CharacterBase::EGroundState groundState = character->GetGroundState();
    state.onGround = (groundState == JPH::CharacterBase::EGroundState::OnGround ||
                      groundState == JPH::CharacterBase::EGroundState::OnSteepGround);
    return state;
}

}  // namespace vx
