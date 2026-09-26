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

#include <glm/vec3.hpp>

#include <algorithm>
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
