// T7 物理与角色的**无窗口**测试：
//   - 高度场构建器（`BuildHeightFieldSamples`）的尺寸与采样值；
//   - `CharacterVirtual` 胶囊在已知高度场上**落地静止**且不下穿；
//   - 1 格台阶被**自动**上步（行走与冲刺两种速度）。
//
// 全部为固定 `dt = 1/60` 的确定性模拟，不依赖窗口 / GPU / 墙钟时间。

#include "physics/physics_world.hpp"
#include "terrain/terrain_collision.hpp"
#include "terrain/terrain_tile.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using vx::PhysicsWorld;

/// 与地表 tile 的采样数一致（65 = 64 列 + 1 层共享边界）。
constexpr std::uint32_t kSampleCount = static_cast<std::uint32_t>(vx::kTerrainTileVertexCount);

constexpr float kGravity = 24.0F;      ///< 与游戏侧一致的重力（格/秒²）
constexpr float kFixedDt = 1.0F / 60.0F;

constexpr float kRestTolerance       = 0.1F;  ///< 落地静止的高度容差（格）
constexpr float kNoFallTolerance     = 0.2F;  ///< 允许的最小穿透量（格）

/// 全平面高度场。
[[nodiscard]] std::vector<float> MakeFlatSamples(float height) {
    return std::vector<float>(static_cast<std::size_t>(kSampleCount) * kSampleCount, height);
}

/// 台阶高度场：列 `x < stepColumn` 为 `low`，否则为 `high`（相邻采样的竖直台阶）。
[[nodiscard]] std::vector<float> MakeStepSamples(float low, float high, std::uint32_t stepColumn) {
    std::vector<float> samples(static_cast<std::size_t>(kSampleCount) * kSampleCount, low);
    for (std::uint32_t y = 0; y < kSampleCount; ++y) {
        for (std::uint32_t x = stepColumn; x < kSampleCount; ++x) {
            samples[static_cast<std::size_t>(y) * kSampleCount + x] = high;
        }
    }
    return samples;
}

[[nodiscard]] PhysicsWorld::HeightFieldDesc MakeDesc(const std::vector<float>& samples) {
    PhysicsWorld::HeightFieldDesc desc;
    desc.sampleCount = kSampleCount;
    desc.samples     = samples.data();
    desc.originX     = 0.0;
    desc.originZ     = 0.0;
    return desc;
}

/// 用固定水平速度驱动角色 `steps` 个固定步，返回期间到达过的最低脚底高度。
[[nodiscard]] float DriveCharacter(PhysicsWorld& physics, PhysicsWorld::CharacterHandle character, float speedX,
                                   int steps) {
    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);

    float lowest = static_cast<float>(physics.GetCharacterState(character).position.y);
    for (int i = 0; i < steps; ++i) {
        glm::vec3 velocity = physics.GetCharacterState(character).velocity;
        velocity.x         = speedX;
        velocity.z         = 0.0F;
        physics.SetCharacterVelocity(character, velocity);
        physics.MoveCharacter(character, kFixedDt, gravity);
        lowest = std::min(lowest, static_cast<float>(physics.GetCharacterState(character).position.y));
    }
    return lowest;
}

/// 轴对齐盒体（半长 `hx/hy/hz`）的 8 个角点，**局部原点在盒心**。
/// 凸包形状由点集求出，故这 8 点即可代表一个盒体。
[[nodiscard]] std::vector<float> MakeBoxHullPoints(float hx, float hy, float hz) {
    std::vector<float> points;
    for (const float sx : { -1.0F, 1.0F }) {
        for (const float sy : { -1.0F, 1.0F }) {
            for (const float sz : { -1.0F, 1.0F }) {
                points.push_back(sx * hx);
                points.push_back(sy * hy);
                points.push_back(sz * hz);
            }
        }
    }
    return points;
}

/// 位姿的**倾角**（度）：局部 +Y 轴与世界上方的夹角（`0` = 完全直立）。
[[nodiscard]] float TiltDegrees(const glm::quat& rotation) {
    const glm::vec3 up = rotation * glm::vec3(0.0F, 1.0F, 0.0F);
    return glm::degrees(std::acos(std::clamp(up.y, -1.0F, 1.0F)));
}

}  // namespace

// 高度场构建器：报告的尺寸与逐采样值必须与 tile 数据一致。
TEST(TerrainCollision, BuildHeightFieldSamplesReportsDimensionsAndValues) {
    vx::TerrainTile tile;
    tile.coord = vx::TileCoord { 2, -3 };

    // 写入可逐点区分的定点高度（1/16 格），覆盖 0 ~ 上限。
    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            tile.SetAt(i, j, static_cast<vx::Height>((i * 16 + j * 3) % 512));
        }
    }

    std::vector<float> samples;
    vx::BuildHeightFieldSamples(tile, samples);

    const std::size_t expectedSize =
        static_cast<std::size_t>(vx::kTerrainTileVertexCount) * static_cast<std::size_t>(vx::kTerrainTileVertexCount);
    ASSERT_EQ(samples.size(), expectedSize);

    // 行主序：`y * N + x`。
    for (int j = 0; j < vx::kTerrainTileVertexCount; ++j) {
        for (int i = 0; i < vx::kTerrainTileVertexCount; ++i) {
            const std::size_t index =
                static_cast<std::size_t>(j) * static_cast<std::size_t>(vx::kTerrainTileVertexCount) +
                static_cast<std::size_t>(i);
            EXPECT_FLOAT_EQ(samples[index], vx::HeightToBlocks(tile.At(i, j)))
                << "采样 (" << i << ", " << j << ") 不一致";
        }
    }
}

// 胶囊从空中落到平面高度场：必须停在表面上，且全程不下穿。
TEST(PhysicsCharacter, FallsAndRestsOnFlatHeightField) {
    PhysicsWorld physics;

    const std::vector<float> samples = MakeFlatSamples(5.0F);
    const PhysicsWorld::BodyHandle body = physics.AddHeightField(MakeDesc(samples));
    ASSERT_NE(body, 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(32.0, 20.0, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);
    float           lowest = 20.0F;
    for (int i = 0; i < 240; ++i) {  // 4 秒
        physics.MoveCharacter(character, kFixedDt, gravity);
        lowest = std::min(lowest, static_cast<float>(physics.GetCharacterState(character).position.y));
    }

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.y, 5.0, kRestTolerance);      // 停在地表
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance);               // 从未穿到地表以下
    EXPECT_NEAR(state.position.x, 32.0, 1e-3);               // 无水平输入 → 不漂移
}

// 1 格台阶：行走速度下自动上步。
TEST(PhysicsCharacter, ClimbsOneGridUnitStepWhileWalking) {
    PhysicsWorld physics;

    const std::vector<float> samples = MakeStepSamples(5.0F, 6.0F, 32);
    const PhysicsWorld::BodyHandle body = physics.AddHeightField(MakeDesc(samples));
    ASSERT_NE(body, 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.stepUpHeight = 1.0F;
    capsule.position     = glm::dvec3(28.0, 5.0, 32.0);  // 站在低处，面向 +X
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    // 200 步 × 6 格/秒 ≈ 20 格：从 x=28 越过 x=32 的台阶后约在 x=48，远离高度场右边界。
    const float lowest = DriveCharacter(physics, character, 6.0F, 200);

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_GT(state.position.x, 32.0);                                   // 已越过台阶棱
    EXPECT_NEAR(state.position.y, 6.0, kRestTolerance);                  // 站到台面高度
    EXPECT_TRUE(state.onGround);
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance);
}

// 20 格/秒冲刺同样不穿地形，并能自动上 1 格台阶（T7 验收口径）。
TEST(PhysicsCharacter, SprintDoesNotPassThroughStep) {
    PhysicsWorld physics;

    const std::vector<float> samples = MakeStepSamples(5.0F, 6.0F, 32);
    const PhysicsWorld::BodyHandle body = physics.AddHeightField(MakeDesc(samples));
    ASSERT_NE(body, 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.stepUpHeight = 1.0F;
    capsule.position     = glm::dvec3(24.0, 5.0, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    // 1 秒 × 20 格/秒 = 20 格，从 x=24 出发 → 越过 x=32 的台阶后约在 x=44，仍在高度场范围内。
    const float lowest = DriveCharacter(physics, character, 20.0F, 60);

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_GT(state.position.x, 32.0);
    EXPECT_GT(state.position.y, 5.9);                 // 位于台面之上（未卡在台阶根部）
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance);        // 全程不穿地形
}

// 20 格/秒冲刺撞向**高墙**（远高于自动上台阶高度）：不得穿过墙体。
TEST(PhysicsCharacter, SprintDoesNotPassThroughTallWall) {
    PhysicsWorld physics;

    const std::vector<float> samples = MakeStepSamples(5.0F, 12.0F, 32);  // 7 格高墙
    const PhysicsWorld::BodyHandle body = physics.AddHeightField(MakeDesc(samples));
    ASSERT_NE(body, 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.stepUpHeight = 1.0F;
    capsule.position     = glm::dvec3(24.0, 5.0, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    const float lowest = DriveCharacter(physics, character, 20.0F, 60);  // 1 秒，位移上限 20 格

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_LT(state.position.x, 32.0);           // 被墙挡住，未穿过
    EXPECT_NEAR(state.position.y, 5.0, kRestTolerance);  // 仍站在低处
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance);   // 全程不穿地形
}

// T18 新增的**通用静态盒体**（`AddStaticBox`）：20 格/秒冲刺撞上盒体不得穿过，
// 与高度场墙同一验收口径；同时确认盒体计入碰撞体总数。
TEST(PhysicsBody, SprintDoesNotPassThroughStaticBox) {
    PhysicsWorld physics;

    const std::vector<float> samples = MakeFlatSamples(5.0F);
    ASSERT_NE(physics.AddHeightField(MakeDesc(samples)), 0u);

    PhysicsWorld::BoxDesc box;
    box.center      = glm::dvec3(32.0, 10.0, 32.0);  // 近端面在 x = 31
    box.halfExtents = glm::dvec3(1.0, 15.0, 10.0);   // 覆盖 y ∈ [-5, 25]，高过角色
    ASSERT_NE(physics.AddStaticBox(box), 0u);
    EXPECT_EQ(physics.BodyCount(), 2u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.stepUpHeight = 1.0F;
    capsule.position     = glm::dvec3(24.0, 5.0, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    const float lowest = DriveCharacter(physics, character, 20.0F, 60);  // 1 秒，位移上限 20 格

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_LT(state.position.x, 31.0);                       // 被盒体挡住，未穿过
    EXPECT_NEAR(state.position.y, 5.0, kRestTolerance);      // 仍站在地面
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance);               // 全程不穿地形
}

// 非法盒体（半长非正）必须拒绝并返回无效句柄，不得静默创建退化碰撞体。
TEST(PhysicsBody, RejectsStaticBoxWithNonPositiveHalfExtent) {
    PhysicsWorld physics;

    PhysicsWorld::BoxDesc box;
    box.halfExtents = glm::dvec3(1.0, 0.0, 1.0);
    EXPECT_EQ(physics.AddStaticBox(box), 0u);
    EXPECT_EQ(physics.BodyCount(), 0u);
}

// 通用三角网静态碰撞体（T28 / ADR 0012）：非法几何必须拒绝，合法几何可建、可重建、可移除。
TEST(PhysicsBody, MeshBodyValidatesGeometryAndSupportsRebuild) {
    PhysicsWorld physics;

    const float         positions[] = { 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F };
    const std::uint32_t indices[]   = { 0, 1, 2 };

    PhysicsWorld::MeshDesc mesh;
    mesh.positions     = positions;
    mesh.vertexCount   = 3;
    mesh.indices       = indices;
    mesh.triangleCount = 1;

    // 非法：空指针 / 三角形数为 0。
    PhysicsWorld::MeshDesc empty;
    EXPECT_EQ(physics.AddMesh(empty), 0u);
    EXPECT_EQ(physics.BodyCount(), 0u);

    PhysicsWorld::MeshDesc noTriangles = mesh;
    noTriangles.triangleCount          = 0;
    EXPECT_EQ(physics.AddMesh(noTriangles), 0u);
    EXPECT_EQ(physics.BodyCount(), 0u);

    // 非法：索引越界。
    const std::uint32_t badIndices[] = { 0, 1, 3 };
    PhysicsWorld::MeshDesc outOfRange = mesh;
    outOfRange.indices                = badIndices;
    EXPECT_EQ(physics.AddMesh(outOfRange), 0u);
    EXPECT_EQ(physics.BodyCount(), 0u);

    // 合法：建得出，且可原地重建形状。
    const PhysicsWorld::BodyHandle body = physics.AddMesh(mesh);
    ASSERT_NE(body, 0u);
    EXPECT_EQ(physics.BodyCount(), 1u);
    EXPECT_TRUE(physics.UpdateMesh(body, mesh));
    EXPECT_FALSE(physics.UpdateMesh(0u, mesh));
    EXPECT_FALSE(physics.UpdateMesh(body, noTriangles)) << "空网格必须拒绝，且保留旧形状";

    physics.RemoveBody(body);
    EXPECT_EQ(physics.BodyCount(), 0u);
}

// 角色站在**三角网碰撞体**上：这就是"洞能走进去、能站在腔底"的物理前提
// （体积网格由 Surface Nets 产出，走的正是这条路径）。
TEST(PhysicsBody, CharacterRestsOnMeshBodySurface) {
    PhysicsWorld physics;

    // 32×32 格的平面（两个三角形），局部 y = 0、原点抬到世界高度 100。
    const float positions[] = { -16.0F, 0.0F, -16.0F, 16.0F, 0.0F, -16.0F, 16.0F, 0.0F, 16.0F, -16.0F, 0.0F, 16.0F };
    const std::uint32_t indices[] = { 0, 1, 2, 0, 2, 3 };

    PhysicsWorld::MeshDesc mesh;
    mesh.positions     = positions;
    mesh.vertexCount   = 4;
    mesh.indices       = indices;
    mesh.triangleCount = 2;
    mesh.originY       = 100.0;
    ASSERT_NE(physics.AddMesh(mesh), 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(0.0, 104.0, 0.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);
    float           lowest = 104.0F;
    for (int i = 0; i < 240; ++i) {  // 4 秒
        physics.MoveCharacter(character, kFixedDt, gravity);
        lowest = std::min(lowest, static_cast<float>(physics.GetCharacterState(character).position.y));
    }

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround) << "三角网面必须能支撑角色";
    EXPECT_NEAR(state.position.y, 100.0, kRestTolerance);
    EXPECT_GE(lowest, 100.0 - kNoFallTolerance) << "不得穿过三角网面";
}

// ---- T33：动态**凸包**刚体（"倒塌整体"）----
//
// 这是"挖断支撑后整体倾倒 / 翻滚"的物理基础（ADR 0015：**统一连通分量刚体化**）：
//   - `AddDynamicConvexHull` 用点云求凸包并交给 Jolt 动态求解；
//   - 位姿按**局部原点**读写（Jolt 内部以质心为位置，本类把它折算回局部原点）。
// 下面三项分别覆盖：受重力下落并停在地面上；有初始角速度时**会转动**（"逐列下落"做不到的效果）；
// 以及非法参数的拒绝。

// 立方体凸包从空中落下：必须停在平坦高度场上，且不漂移、不倾斜。
TEST(PhysicsBody, DynamicConvexHullFallsAndRestsOnFlatGround) {
    PhysicsWorld physics;
    physics.SetGravity(glm::vec3(0.0F, -kGravity, 0.0F));

    const std::vector<float> samples = MakeFlatSamples(5.0F);
    ASSERT_NE(physics.AddHeightField(MakeDesc(samples)), 0u);

    const std::vector<float> points = MakeBoxHullPoints(1.0F, 1.0F, 1.0F);

    PhysicsWorld::ConvexHullDesc desc;
    desc.positions  = points.data();
    desc.pointCount = points.size() / 3;
    desc.originX    = 32.0;
    desc.originY    = 20.0;  // 局部原点 = 盒心，从空中 20 格处落下
    desc.originZ    = 32.0;
    desc.mass       = 100.0F;

    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(desc);
    ASSERT_NE(body, 0u);

    float lowest = 20.0F;
    for (int i = 0; i < 300; ++i) {  // 5 秒：足够落下并落定
        physics.Update(kFixedDt);
        lowest = std::min(lowest, static_cast<float>(physics.GetRigidBodyState(body).position.y));
    }

    const PhysicsWorld::RigidBodyState state = physics.GetRigidBodyState(body);
    EXPECT_NEAR(state.position.y, 6.0, 0.15);   // 半长 1 ⇒ 盒心停在地表 5 + 1
    EXPECT_NEAR(state.position.x, 32.0, 0.05);  // 无水平外力 ⇒ 不漂移
    EXPECT_NEAR(state.position.z, 32.0, 0.05);
    EXPECT_GE(lowest, 5.0 - kNoFallTolerance) << "不得穿过地面";
    EXPECT_LT(TiltDegrees(state.rotation), 5.0F) << "平放在平地上不应倾斜";
}

// 细长柱在空中自由下落 + 初始角速度：**必须转动**（这是刚体化相对于逐列下落的核心差异），
// 且下落加速度必须是 `SetGravity` 设的值（而不是 Jolt 默认的 9.81）。
TEST(PhysicsBody, DynamicConvexHullRotatesFromInitialAngularVelocity) {
    PhysicsWorld physics;
    physics.SetGravity(glm::vec3(0.0F, -kGravity, 0.0F));

    const std::vector<float> points = MakeBoxHullPoints(0.5F, 4.0F, 0.5F);  // 高 8 格、截面 1×1 的柱

    PhysicsWorld::ConvexHullDesc desc;
    desc.positions       = points.data();
    desc.pointCount      = points.size() / 3;
    desc.originX         = 0.0;
    desc.originY         = 100.0;  // 远离地面 ⇒ 1 秒内不会碰到任何东西
    desc.originZ         = 0.0;
    desc.mass            = 60.0F;
    desc.angularVelocity = glm::vec3(0.0F, 0.0F, 1.0F);  // 绕 Z 轴 1 rad/s

    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(desc);
    ASSERT_NE(body, 0u);

    const int steps = 60;  // 1 秒
    for (int i = 0; i < steps; ++i) {
        physics.Update(kFixedDt);
    }

    const PhysicsWorld::RigidBodyState state = physics.GetRigidBodyState(body);
    // 绕 Z 匀速转 1 秒 ⇒ 倾角 ≈ 57°，远超 30° 判据（T33 判据 ①）。
    EXPECT_GT(TiltDegrees(state.rotation), 30.0F) << "给定初始角速度后必须转动（整体刚体化）";
    // Jolt 的默认阻尼（线 / 角各 0.05）保留不关 ⇒ 1 秒后角速度略降（≈ 0.95 rad/s），仍远大于 0。
    EXPECT_NEAR(state.angularVelocity.z, 1.0F, 0.1F);
    // 半隐式欧拉：n 步后下落 ≈ g·dt²·n(n+1)/2 ≈ 12 格（含阻尼）；Jolt 默认重力 9.81 只会落 ≈ 5 格。
    EXPECT_NEAR(state.position.y, 100.0 - 12.2, 0.6) << "重力必须是 SetGravity 设置的值，而不是 Jolt 默认的 9.81";
    EXPECT_NEAR(state.linearVelocity.y, -kGravity, 1.0F);  // 同上：含阻尼（实测 ≈ -23.4）
}

// T48 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三：
// **动态刚体**必须能被**线段查询**命中 —— 这是 BUG4（"掉落中的整体打不中"）的判据：
// 倒塌中的整体**体素已被抽出**（体积里是空的），只有物理查询能看见它；
// 命中点必须落在**真实凸包表面**上（不再是手工 OBB 边界），并回报**是哪个刚体**。
TEST(PhysicsBody, RayCastDynamicHitsMovingHull) {
    PhysicsWorld physics;
    physics.SetGravity(glm::vec3(0.0F, -kGravity, 0.0F));

    // 不放地形：刚体自由下落 ⇒ 查询期间它**仍在运动**（T48 要覆盖的正是"飞行中"这一态）。
    const std::vector<float> points = MakeBoxHullPoints(1.0F, 1.0F, 1.0F);

    PhysicsWorld::ConvexHullDesc desc;
    desc.positions  = points.data();
    desc.pointCount = points.size() / 3;
    desc.originX    = 0.0;
    desc.originY    = 100.0;
    desc.originZ    = 0.0;
    desc.mass       = 100.0F;

    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(desc);
    ASSERT_NE(body, 0u);

    for (int i = 0; i < 30; ++i) {  // 0.5 秒：仍在空中
        physics.Update(kFixedDt);
    }
    const PhysicsWorld::RigidBodyState falling = physics.GetRigidBodyState(body);
    ASSERT_LT(falling.linearVelocity.y, -1.0F) << "前置：整体仍在**运动中**（不是落定态）";

    const glm::dvec3 center = falling.position;
    const PhysicsWorld::RayCastHit hit =
        physics.RayCastDynamic(center - glm::dvec3(0.0, 5.0, 0.0), center + glm::dvec3(0.0, 5.0, 0.0));
    ASSERT_TRUE(hit.hit) << "运动中的动态刚体必须可被查询命中（BUG4 的判据）";
    EXPECT_EQ(hit.body, body) << "必须回报命中的是哪个刚体（玩法层据此定位「是哪个整体」）";
    // 盒半长 1 ⇒ 命中点应在盒心下方 1 格附近的**凸包表面**上（而不是盒心 / OBB 外的早命中点）。
    EXPECT_NEAR(hit.point.y, center.y - 1.0, 0.2) << "命中点应落在真实凸包表面";
    EXPECT_NEAR(hit.point.x, center.x, 0.2);
    EXPECT_NEAR(hit.point.z, center.z, 0.2);

    // 反例：擦不到刚体的射线必须 miss（否则"命中"没有区分度）。
    const PhysicsWorld::RayCastHit miss = physics.RayCastDynamic(center + glm::dvec3(10.0, -5.0, 0.0),
                                                                  center + glm::dvec3(10.0, 5.0, 0.0));
    EXPECT_FALSE(miss.hit) << "远处射线不得命中";
    EXPECT_FALSE(physics.RayCastDynamic(center, center).hit) << "退化线段不得命中";
}

// 非法凸包参数（点数不足 / 质量非正 / 空指针）必须拒绝并返回无效句柄，不得静默创建退化刚体。
TEST(PhysicsBody, DynamicConvexHullValidatesInput) {
    PhysicsWorld physics;

    const std::vector<float> points = MakeBoxHullPoints(1.0F, 1.0F, 1.0F);

    PhysicsWorld::ConvexHullDesc desc;
    desc.positions  = points.data();
    desc.pointCount = points.size() / 3;
    desc.mass       = 10.0F;

    PhysicsWorld::ConvexHullDesc tooFew = desc;
    tooFew.pointCount                   = 3;
    EXPECT_EQ(physics.AddDynamicConvexHull(tooFew), 0u);

    PhysicsWorld::ConvexHullDesc zeroMass = desc;
    zeroMass.mass                         = 0.0F;
    EXPECT_EQ(physics.AddDynamicConvexHull(zeroMass), 0u);

    PhysicsWorld::ConvexHullDesc nullPoints = desc;
    nullPoints.positions                    = nullptr;
    EXPECT_EQ(physics.AddDynamicConvexHull(nullPoints), 0u);

    EXPECT_EQ(physics.BodyCount(), 0u);

    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(desc);
    ASSERT_NE(body, 0u);
    EXPECT_EQ(physics.BodyCount(), 1u);

    physics.RemoveBody(body);
    EXPECT_EQ(physics.BodyCount(), 0u);
    EXPECT_EQ(physics.GetRigidBodyState(body).position, glm::dvec3(0.0)) << "无效句柄读回零位姿，不得崩溃";
}

// T46：`ActivateBody` —— "支撑被挖掉后唤醒保留残骸"的入口（ADR 0017 决策二）。两条硬要求：
//   ① 无效句柄 / 静态体是**无操作**（不崩溃）；② 唤醒一个**已静止**的刚体后，它不会因此弹跳 / 漂移。
TEST(PhysicsBody, ActivateBodyToleratesInvalidHandlesAndKeepsRestingHullStable) {
    PhysicsWorld physics;
    physics.SetGravity(glm::vec3(0.0F, -24.0F, 0.0F));

    physics.ActivateBody(0);     // 无效句柄
    physics.ActivateBody(9999);  // 越界句柄

    const PhysicsWorld::BodyHandle ground =
        physics.AddStaticBox(PhysicsWorld::BoxDesc { glm::dvec3(0.0, 0.0, 0.0), glm::dvec3(4.0, 0.5, 4.0) });
    ASSERT_NE(ground, 0u);
    physics.ActivateBody(ground);  // 静态体：无操作
    EXPECT_EQ(physics.BodyCount(), 1u);

    const std::vector<float>     points = MakeBoxHullPoints(1.0F, 1.0F, 1.0F);
    PhysicsWorld::ConvexHullDesc desc;
    desc.positions  = points.data();
    desc.pointCount = points.size() / 3;
    desc.originX    = 0.0;
    desc.originY    = 3.0;  // 从地面盒顶（y = 0.5）上方落下
    desc.originZ    = 0.0;
    desc.mass       = 10.0F;
    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(desc);
    ASSERT_NE(body, 0u);

    for (int i = 0; i < 300; ++i) {  // 5 秒：落到盒顶并静止（Jolt 会把静止体休眠）
        physics.Update(kFixedDt);
    }
    const PhysicsWorld::RigidBodyState resting = physics.GetRigidBodyState(body);
    EXPECT_NEAR(resting.position.y, 1.5, 0.15) << "盒心停在地面盒顶 0.5 + 半长 1";

    physics.ActivateBody(body);  // T46：唤醒（游戏层在"块碰撞体重建"时对相交的保留残骸调用）
    for (int i = 0; i < 30; ++i) {
        physics.Update(kFixedDt);
    }
    const PhysicsWorld::RigidBodyState after = physics.GetRigidBodyState(body);
    EXPECT_NEAR(after.position.y, resting.position.y, 0.1) << "唤醒静止体不应使它弹跳 / 漂移";
    EXPECT_LT(std::fabs(after.linearVelocity.y), 1.0F);
}

// ---- T(W1) / [ADR 0025]：大世界坐标精度 —— 物理世界原点与重定基 ----
//
// 判据（ADR 0025）：① 默认原点 `(0,0,0)` ⇒ 与引入前逐位等价；② 挡位量化是纯函数且向下取整；
// ③ `SetWorldOrigin` 只平移、**世界位置不变**（刚体与角色都算），且之后模拟仍稳定。

// 水平原点量化：`x` / `z` 向下取整到 `quantum` 的整数倍、`y` 恒为 0；`quantum ≤ 0` ⇒ 原点 `(0,0,0)`。
TEST(PhysicsBody, QuantizeHorizontalWorldOriginSnapsDownToGrid) {
    const glm::dvec3 q = vx::QuantizeHorizontalWorldOrigin(glm::dvec3(10000.0, 123.0, 10000.0), 512.0);
    EXPECT_DOUBLE_EQ(q.x, 9728.0);  // floor(10000 / 512) = 19 ⇒ 19 × 512
    EXPECT_DOUBLE_EQ(q.y, 0.0);
    EXPECT_DOUBLE_EQ(q.z, 9728.0);

    const glm::dvec3 negative = vx::QuantizeHorizontalWorldOrigin(glm::dvec3(-100.0, 0.0, 50.0), 512.0);
    EXPECT_DOUBLE_EQ(negative.x, -512.0);  // 负坐标也向下取整（-0.195 ⇒ -1）
    EXPECT_DOUBLE_EQ(negative.z, 0.0);

    EXPECT_EQ(vx::QuantizeHorizontalWorldOrigin(glm::dvec3(1.0, 2.0, 3.0), 0.0), glm::dvec3(0.0));
}

// 重定基：把全部刚体与角色按"旧原点 − 新原点"平移后，**世界位置逐值不变**；之后角色仍能正常落地。
TEST(PhysicsBody, SetWorldOriginKeepsWorldPositionsAndSimulationStable) {
    PhysicsWorld physics;
    physics.SetGravity(glm::vec3(0.0F, -kGravity, 0.0F));
    EXPECT_EQ(physics.WorldOrigin(), glm::dvec3(0.0)) << "默认原点必须是 (0,0,0)，保证与引入前等价";

    // 放在距原点 10 km 处：平坦地形（世界原点 10000）+ 角色 + 一个空中动态刚体。
    const std::vector<float>      samples = MakeFlatSamples(5.0F);
    PhysicsWorld::HeightFieldDesc heightField = MakeDesc(samples);
    heightField.originX = 10000.0;
    heightField.originZ = 10000.0;
    ASSERT_NE(physics.AddHeightField(heightField), 0u);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(10032.0, 20.0, 10032.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    const std::vector<float>     points = MakeBoxHullPoints(1.0F, 1.0F, 1.0F);
    PhysicsWorld::ConvexHullDesc hull;
    hull.positions  = points.data();
    hull.pointCount = points.size() / 3;
    hull.originX    = 10000.0;
    hull.originY    = 60.0;
    hull.originZ    = 10000.0;
    hull.mass       = 30.0F;
    const PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(hull);
    ASSERT_NE(body, 0u);

    const glm::dvec3 characterBefore = physics.GetCharacterState(character).position;
    const glm::dvec3 hullBefore      = physics.GetRigidBodyState(body).position;
    ASSERT_NEAR(characterBefore.x, 10032.0, 1e-6);
    ASSERT_NEAR(hullBefore.x, 10000.0, 1e-6);

    // 把原点跳到角色所在的 512 格挡位。
    const glm::dvec3 newOrigin = vx::QuantizeHorizontalWorldOrigin(characterBefore, 512.0);
    physics.SetWorldOrigin(newOrigin);
    EXPECT_EQ(physics.WorldOrigin(), newOrigin);

    const glm::dvec3 characterAfter = physics.GetCharacterState(character).position;
    const glm::dvec3 hullAfter      = physics.GetRigidBodyState(body).position;
    EXPECT_NEAR(characterAfter.x, characterBefore.x, 1e-3) << "重定基不得改变角色的世界位置";
    EXPECT_NEAR(characterAfter.y, characterBefore.y, 1e-3);
    EXPECT_NEAR(characterAfter.z, characterBefore.z, 1e-3);
    EXPECT_NEAR(hullAfter.x, hullBefore.x, 1e-3) << "重定基不得改变刚体的世界位置";
    EXPECT_NEAR(hullAfter.y, hullBefore.y, 1e-3);
    EXPECT_NEAR(hullAfter.z, hullBefore.z, 1e-3);

    // 幂等：设成同一个原点 ⇒ 无操作、位置仍不变。
    physics.SetWorldOrigin(newOrigin);
    EXPECT_NEAR(physics.GetCharacterState(character).position.x, characterBefore.x, 1e-3);

    // 重定基后继续模拟：角色照常落到地表（世界 y ≈ 5），水平不漂移。
    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);
    for (int i = 0; i < 240; ++i) {
        physics.MoveCharacter(character, kFixedDt, gravity);
    }
    const PhysicsWorld::CharacterState landed = physics.GetCharacterState(character);
    EXPECT_TRUE(landed.onGround);
    EXPECT_NEAR(landed.position.y, 5.0, kRestTolerance) << "重定基后仍必须站在地表";
    EXPECT_NEAR(landed.position.x, characterBefore.x, 1.0);
    EXPECT_NEAR(landed.position.z, characterBefore.z, 1.0);
}
