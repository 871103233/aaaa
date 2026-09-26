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
void StepWithJump(PhysicsWorld& physics, PhysicsWorld::CharacterHandle character, bool jump) {
    const PhysicsWorld::CharacterState state = physics.GetCharacterState(character);

    glm::vec3 velocity = state.velocity;
    velocity.x         = 0.0F;
    velocity.z         = 0.0F;
    if (jump && state.onGround) {
        velocity.y = JumpVelocityForHeight(kGravity, kCharacterHeight);
    }
    physics.SetCharacterVelocity(character, velocity);
    physics.MoveCharacter(character, static_cast<float>(kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));
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

    StepWithJump(physics, character, /*jump=*/true);  // 施加跳跃冲量

    float apex = static_cast<float>(physics.GetCharacterState(character).position.y);
    for (int i = 0; i < 120; ++i) {  // 继续重力积分的空中段
        StepWithJump(physics, character, /*jump=*/false);
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
            StepWithJump(physics, character, jumpRequested);
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
