// 缺陷 B3 回归测试与跳跃设计规格（跳跃高度 = 当前身高的 60%）：
//   1) 派生公式：`v0 = sqrt(2 g * 0.6 H)`，由返回速度与重力反算的最高点须落在 0.6 H 的 5% 内；
//   2) 物理可跳：在平地高度场上施加派生初速，角色确实离地并达到约 0.6 H 的高度；
//   3) 帧循环：复刻 game/main.cpp 的"帧级锁存 + 固定步应用"，在 ~1500 FPS（多数帧没有逻辑步）
//      下，一次空格按下必须跳起来 —— 直接锁定 B3 的根因（帧边界消费边沿会在零步帧被丢弃）。
//
// 全部为固定 dt 的确定性模拟，不依赖窗口 / GPU / 墙钟时间。

#include "character_movement.hpp"
#include "core/fixed_step.hpp"
#include "physics/physics_world.hpp"
#include "terrain/terrain_types.hpp"

#include <glm/vec3.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using vx::FixedStepAccumulator;
using vx::JumpVelocityForHeight;
using vx::kFixedDt;
using vx::kJumpApexHeightRatio;
using vx::kTerrainTileVertexCount;
using vx::PhysicsWorld;

constexpr float kGravity         = 24.0F;   ///< 与 game/main.cpp 一致的重力（格/秒²）
constexpr float kCharacterHeight = 1.80F;   ///< 与 game/main.cpp 一致的角色总高（格）
constexpr float kFlatHeight      = 5.0F;    ///< 测试用平地高度（格）

/// 建立一个"全平面"高度场 + 一个停在其上的角色，返回角色句柄。
[[nodiscard]] PhysicsWorld::CharacterHandle MakeSettledCharacter(PhysicsWorld& physics) {
    const std::uint32_t sampleCount = static_cast<std::uint32_t>(kTerrainTileVertexCount);
    std::vector<float>  samples(static_cast<std::size_t>(sampleCount) * sampleCount, kFlatHeight);

    PhysicsWorld::HeightFieldDesc desc;
    desc.sampleCount = sampleCount;
    desc.samples     = samples.data();
    desc.originX     = 0.0;
    desc.originZ     = 0.0;
    // 注意：samples 的所有权在此函数内，必须在 AddHeightField 之前保持有效——它在返回前已复制。
    (void)physics.AddHeightField(desc);

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(32.0, static_cast<double>(kFlatHeight) + 1.0, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);

    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);
    for (int i = 0; i < 120; ++i) {
        physics.MoveCharacter(character, static_cast<float>(kFixedDt), gravity);
    }
    return character;
}

/// 复刻 game/main.cpp 固定逻辑步的**跳跃相关**部分（水平输入恒为 0）。
///
/// T54：门槛改用 `walkableGround`（`OnGround`），并**逐字镜像** `vx::AdvanceJumpAssist`
/// （土狼时间 + 跳跃缓冲）—— 测试与玩法层共用同一份实现，避免"测的是另一套逻辑"。
/// 返回本步是否真的起跳。
bool StepWithJump(PhysicsWorld& physics, PhysicsWorld::CharacterHandle character, vx::JumpAssist& assist,
                  bool jumpPressed, float pushX = 0.0F) {
    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);

    glm::vec3 velocity = state.velocity;
    velocity.x         = pushX;
    velocity.z         = 0.0F;
    const bool jumped  = vx::AdvanceJumpAssist(assist, state.walkableGround, jumpPressed,
                                               static_cast<float>(kFixedDt));
    if (jumped) {
        velocity.y = JumpVelocityForHeight(kGravity, kCharacterHeight);
    }
    physics.SetCharacterVelocity(character, velocity);
    physics.MoveCharacter(character, static_cast<float>(kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));
    return jumped;
}

/// 建立"平地 + 一段**陡坡** + 平地"的高度场，并把角色放在**陡坡中段**上（T54 的现场）。
///
/// 坡度取 `kSlopeRise = 3` 格 / 列 ⇒ `arctan(3) ≈ 71.6°`，远陡于 `CapsuleDesc::maxSlopeAngleDeg = 50°`
/// ⇒ Jolt 报 `OnSteepGround`：角色**被坡面支撑**（`onGround == true`）但**不可站立**（`walkableGround == false`）。
/// 这正是缺陷 T54 的成因 —— 旧实现把 `OnSteepGround` 也算作"着地"，贴坡反复按跳就能爬上去。
[[nodiscard]] PhysicsWorld::CharacterHandle MakeCharacterSlidingOnSteepSlope(PhysicsWorld& physics) {
    const std::uint32_t sampleCount = static_cast<std::uint32_t>(kTerrainTileVertexCount);
    std::vector<float>  samples(static_cast<std::size_t>(sampleCount) * sampleCount, kFlatHeight);

    constexpr int   kSlopeStart = 40;
    constexpr int   kSlopeEnd   = 52;
    constexpr float kSlopeRise  = 3.0F;  // 格 / 列 ⇒ ≈ 71.6°
    for (std::uint32_t z = 0; z < sampleCount; ++z) {
        for (int x = kSlopeStart; x <= kSlopeEnd; ++x) {
            samples[static_cast<std::size_t>(x) + static_cast<std::size_t>(sampleCount) * z] =
                kFlatHeight + kSlopeRise * static_cast<float>(x - kSlopeStart);
        }
    }

    PhysicsWorld::HeightFieldDesc desc;
    desc.sampleCount = sampleCount;
    desc.samples     = samples.data();
    desc.originX     = 0.0;
    desc.originZ     = 0.0;
    (void)physics.AddHeightField(desc);  // 采样在调用内被复制

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(46.0, static_cast<double>(kFlatHeight + kSlopeRise * 6.0F) + 0.5, 32.0);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    for (int i = 0; i < 30; ++i) {  // 落到坡面上（贴住、并被重力带着沿坡下滑）
        physics.SetCharacterVelocity(character, glm::vec3(0.0F));
        physics.MoveCharacter(character, static_cast<float>(kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));
    }
    return character;
}

}  // namespace

// 派生公式：由返回初速与重力反算的最高点，必须落在 `kJumpApexHeightRatio * H` 的 5% 内。
TEST(CharacterJump, VelocityApexMatchesRatioOfHeight) {
    const float gravities[] = { 9.8F, 24.0F, 40.0F };
    const float heights[]   = { 0.9F, 1.80F, 3.20F };

    for (const float gravity : gravities) {
        for (const float height : heights) {
            const float velocity = JumpVelocityForHeight(gravity, height);
            const float apex     = velocity * velocity / (2.0F * gravity);  // h = v0² / (2 g)
            const float target   = kJumpApexHeightRatio * height;
            EXPECT_NEAR(apex, target, 0.05F * target)
                << "g=" << gravity << " H=" << height;
        }
    }

    // 数值锚点：g = 24、H = 1.80 → v0 = 7.2 格/秒、最高点 1.08 格（= 身高的 60%）。
    const float velocity = JumpVelocityForHeight(24.0F, 1.80F);
    EXPECT_NEAR(velocity, 7.2F, 1e-4F);
    EXPECT_NEAR(velocity * velocity / (2.0F * 24.0F), 1.08F, 1e-4F);

    // 非法入参按"不起跳"处理。
    EXPECT_FLOAT_EQ(JumpVelocityForHeight(0.0F, 1.80F), 0.0F);
    EXPECT_FLOAT_EQ(JumpVelocityForHeight(24.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(JumpVelocityForHeight(-1.0F, -1.0F), 0.0F);
}

// 物理可跳：着地状态下施加派生初速，角色离地并跳到约 0.6 H 的高度。
TEST(CharacterJump, PhysicsReachesDerivedApex) {
    PhysicsWorld physics;
    const PhysicsWorld::CharacterHandle character = MakeSettledCharacter(physics);

    const PhysicsWorld::CharacterState settled = physics.GetCharacterState(character);
    ASSERT_TRUE(settled.onGround);
    ASSERT_TRUE(settled.walkableGround) << "平地必须同时满足 onGround 与 walkableGround";

    vx::JumpAssist assist;
    (void)StepWithJump(physics, character, assist, /*jumpPressed=*/true);  // 施加跳跃冲量

    float apex = static_cast<float>(physics.GetCharacterState(character).position.y);
    for (int i = 0; i < 120; ++i) {  // 继续重力积分的空中段
        (void)StepWithJump(physics, character, assist, /*jumpPressed=*/false);
        apex = std::max(apex, static_cast<float>(physics.GetCharacterState(character).position.y));
    }

    // 离散固定步会让实测最高点略高于理想抛体值，故这里用比公式测试更宽的容差。
    const float rise = apex - kFlatHeight;
    EXPECT_NEAR(rise, kJumpApexHeightRatio * kCharacterHeight, 0.25F) << "实际上升 " << rise << " 格";
    EXPECT_GT(rise, 0.5F);
}

// 帧循环回归（B3 根因）：~1500 FPS 下绝大多数帧没有逻辑步，一次空格按下仍必须跳起来。
TEST(CharacterJump, LatchedSpacePressJumpsDespiteZeroStepFrames) {
    PhysicsWorld physics;
    const PhysicsWorld::CharacterHandle character = MakeSettledCharacter(physics);
    ASSERT_TRUE(physics.GetCharacterState(character).onGround);

    const double              frameDt = 1.0 / 1500.0;  // Mailbox 下实测帧率量级
    FixedStepAccumulator      accumulator(kFixedDt);
    vx::JumpAssist            assist;
    bool                      jumpRequested   = false;
    constexpr int             kPressFrame     = 7;
    bool                      pressFrameStepped = false;
    int                       framesWithSteps = 0;

    float apex = static_cast<float>(physics.GetCharacterState(character).position.y);
    for (int frame = 0; frame < 1500; ++frame) {  // 1 秒
        // 复刻 game/main.cpp：空格边沿在帧边界**锁存**，只有真正跑逻辑步后才消费。
        jumpRequested = jumpRequested || (frame == kPressFrame);

        const vx::StepPlan plan = accumulator.Advance(frameDt);
        if (plan.steps > 0) {
            ++framesWithSteps;
        }
        if (frame == kPressFrame && plan.steps > 0) {
            pressFrameStepped = true;
        }

        for (int step = 0; step < plan.steps; ++step) {
            (void)StepWithJump(physics, character, assist, jumpRequested);
        }
        if (plan.steps > 0) {
            jumpRequested = false;
        }

        apex = std::max(apex, static_cast<float>(physics.GetCharacterState(character).position.y));
    }

    // 该按下帧确实没有逻辑步：证明本测试覆盖的是"锁存"这条路径，而不是碰巧踩中有步的帧。
    EXPECT_FALSE(pressFrameStepped) << "按下帧意外地跑了逻辑步，本用例未覆盖锁存路径";
    EXPECT_LT(framesWithSteps, 750) << "高帧率下应有明显过半的帧没有逻辑步";

    const float rise = apex - kFlatHeight;
    EXPECT_GT(rise, 0.5F) << "锁存的空格按下必须真正跳起来";
    EXPECT_NEAR(rise, kJumpApexHeightRatio * kCharacterHeight, 0.25F);
}

// ============================================================================================
// T54（所有者 2026-09-28 实测："贴着垂直岩壁能够一直跳"）—— **判定：确认是缺陷**
//
// 契约 = 起跳必须站在**可站立**地面上（业内口径：Unity `CharacterController.isGrounded` + `slopeLimit`、
//        Unreal `Walking` / `WalkableFloorZ`、Jolt 官方示例 `GetGroundState() == OnGround`）；
// 机制 = `CharacterState::onGround` 把 Jolt 的 **`OnSteepGround` 也算作"着地"**，
//        而玩法层只用 `command.jump && state.onGround` 放行起跳 ⇒ 贴着陡壁下滑时**每一步**都能起跳。
// 修法 = 起跳门槛改用 `walkableGround`（只有 `OnGround`），并按业内规范补土狼时间 + 跳跃缓冲。
// ============================================================================================

// 判据 ①：贴着陡坡 / 陡壁时 `onGround == true`（仍被支撑）但 `walkableGround == false`（不可站立）。
TEST(CharacterJump, SteepSlopeIsSupportedButNotWalkable) {
    PhysicsWorld physics;
    const PhysicsWorld::CharacterHandle character = MakeCharacterSlidingOnSteepSlope(physics);

    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround) << "陡坡上角色确实被坡面**支撑**（onGround 必须仍为真）";
    EXPECT_FALSE(state.walkableGround) << "但坡度过陡、不可站立 ⇒ walkableGround 必须为假（T54 的根因）";
}

// 判据 ③（**缺陷判据**）：贴着陡坡反复请求起跳 ⇒ 高度**不得上升**（旧实现在这里会一路爬上去）。
TEST(CharacterJump, SteepSlopeCannotBeClimbedByRepeatedJumps) {
    PhysicsWorld physics;
    const PhysicsWorld::CharacterHandle character = MakeCharacterSlidingOnSteepSlope(physics);

    vx::JumpAssist assist;
    const float    startY = static_cast<float>(physics.GetCharacterState(character).position.y);
    float          apex   = startY;
    int            jumpsOnUnwalkable = 0;
    for (int step = 0; step < 240; ++step) {  // 4 秒：足够"一路爬上去"
        // 模拟"玩家反复按跳"：每一步都给一次按下边沿，并持续把角色压向坡面。
        const bool walkableBefore = physics.GetCharacterState(character).walkableGround;
        const bool jumped = StepWithJump(physics, character, assist, /*jumpPressed=*/true, /*pushX=*/1.0F);
        if (jumped && !walkableBefore) {
            ++jumpsOnUnwalkable;  // 契约违背：不在**可站立**地面上却起跳了
        }
        apex = std::max(apex, static_cast<float>(physics.GetCharacterState(character).position.y));
    }

    EXPECT_EQ(jumpsOnUnwalkable, 0)
        << "**不可站立**（陡坡 / 陡壁）时一次都不许起跳 —— 旧实现（门槛 = onGround）在这里会每步都跳";
    EXPECT_LT(apex - startY, 0.5F)
        << "贴陡坡反复按跳不得爬升（实测上升 " << (apex - startY) << " 格；跳一次就应该是 "
        << (kJumpApexHeightRatio * kCharacterHeight) << " 格量级）";
}

// 判据 ②：平地起跳仍然成功；**腾空后（土狼窗口已过）**再按跳不得二段跳。
TEST(CharacterJump, AirborneJumpRequestAfterCoyoteExpiresIsIgnored) {
    PhysicsWorld physics;
    const PhysicsWorld::CharacterHandle character = MakeSettledCharacter(physics);
    ASSERT_TRUE(physics.GetCharacterState(character).walkableGround);

    vx::JumpAssist assist;
    EXPECT_TRUE(StepWithJump(physics, character, assist, /*jumpPressed=*/true)) << "平地起跳必须成功";

    // 腾空 0.5 s（≫ `kJumpCoyoteSeconds = 0.10 s`）后再按跳 —— 不得重新获得起跳速度。
    for (int step = 0; step < 30; ++step) {
        EXPECT_FALSE(StepWithJump(physics, character, assist, /*jumpPressed=*/false));
    }
    EXPECT_FALSE(StepWithJump(physics, character, assist, /*jumpPressed=*/true))
        << "土狼窗口已过 ⇒ 空中按跳不得起跳（否则就是二段跳）";
    EXPECT_LT(physics.GetCharacterState(character).velocity.y,
              JumpVelocityForHeight(kGravity, kCharacterHeight) * 0.5F)
        << "竖直速度不得被重新抬到起跳初速量级";
}

// 判据 ④：土狼时间 / 跳跃缓冲两个窗口的**边界**（纯函数，逐字用玩法层同一实现）。
TEST(CharacterJump, CoyoteTimeAndJumpBufferWindows) {
    const float dt = static_cast<float>(kFixedDt);

    // ① 土狼时间：站在可行走地面上 ⇒ 窗口满格；刚离开**窗口内**仍可跳。
    vx::JumpAssist within;
    for (int i = 0; i < 3; ++i) {
        (void)vx::AdvanceJumpAssist(within, /*walkableGround=*/true, /*jumpPressed=*/false, dt);
    }
    EXPECT_TRUE(vx::AdvanceJumpAssist(within, /*walkableGround=*/false, /*jumpPressed=*/true, dt))
        << "刚离开地面的窗口内按下 ⇒ 仍可起跳";

    // ② 土狼时间：离开地面**超过窗口**后不得起跳。
    vx::JumpAssist expired;
    (void)vx::AdvanceJumpAssist(expired, /*walkableGround=*/true, /*jumpPressed=*/false, dt);
    for (int i = 0; i < 20; ++i) {  // 0.33 s > 0.10 s
        (void)vx::AdvanceJumpAssist(expired, /*walkableGround=*/false, /*jumpPressed=*/false, dt);
    }
    EXPECT_FALSE(vx::AdvanceJumpAssist(expired, /*walkableGround=*/false, /*jumpPressed=*/true, dt))
        << "土狼窗口已过 ⇒ 不得起跳";

    // ③ 跳跃缓冲：空中按下（当时不可跳）⇒ 请求被记住；**落地那一步**兑现。
    vx::JumpAssist buffered;
    EXPECT_FALSE(vx::AdvanceJumpAssist(buffered, /*walkableGround=*/false, /*jumpPressed=*/true, dt))
        << "空中按下当时不得起跳（没有地面）";
    EXPECT_TRUE(vx::AdvanceJumpAssist(buffered, /*walkableGround=*/true, /*jumpPressed=*/false, dt))
        << "落地的那一步必须兑现缓冲里的跳跃请求";

    // ④ 跳跃缓冲：超出窗口即失效（不得"很久以后落地才补跳"）。
    vx::JumpAssist stale;
    (void)vx::AdvanceJumpAssist(stale, /*walkableGround=*/false, /*jumpPressed=*/true, dt);
    for (int i = 0; i < 20; ++i) {  // 0.33 s > 0.15 s
        (void)vx::AdvanceJumpAssist(stale, /*walkableGround=*/false, /*jumpPressed=*/false, dt);
    }
    EXPECT_FALSE(vx::AdvanceJumpAssist(stale, /*walkableGround=*/true, /*jumpPressed=*/false, dt))
        << "缓冲已超时 ⇒ 落地不得补跳";

    // ⑤ 起跳即清零 ⇒ 同一次按键不会被消费两次。
    vx::JumpAssist once;
    (void)vx::AdvanceJumpAssist(once, /*walkableGround=*/true, /*jumpPressed=*/false, dt);
    EXPECT_TRUE(vx::AdvanceJumpAssist(once, /*walkableGround=*/true, /*jumpPressed=*/true, dt));
    EXPECT_FALSE(vx::AdvanceJumpAssist(once, /*walkableGround=*/true, /*jumpPressed=*/false, dt))
        << "同一次按下只能起跳一次";
}
