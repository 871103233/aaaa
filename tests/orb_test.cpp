// 光球弹道与命中检测（game/orb.hpp）+ 弹丸规格表（world/dig/projectile_table.*）单元测试。
//
// 判据（阶段计划 T27）：
//   ① 无命中时弹道满足**离散解析解**（半隐式欧拉：v 先受重力、p 再位移），且与连续抛物线一致；
//   ② 命中检测在给定实心域上触发，命中点落在**表面附近**（取最后一个安全点与首个实心点的中点）；
//   ③ 池满时不发射、零方向不发射；超时弹丸自动失效；
//   ④ 程序化球网格尺寸 / 位置 / 法线正确；
//   ⑤ 弹丸表：内置默认与仓库内文件一致；文件缺失 / 非法即抛异常。

#include "orb.hpp"

#include "dig/projectile_table.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>

#include <gtest/gtest.h>

namespace {

using vx::IOrbWorldQuery;
using vx::Orb;
using vx::OrbHit;
using vx::OrbPool;
using vx::ProjectileSpec;
using vx::ProjectileTable;

constexpr float kStepSeconds = 1.0F / 60.0F;

/// 假世界：水平地面（`y <= groundHeight` 为实心）+ 可选的一个实心球。
class FakeWorld final : public IOrbWorldQuery {
public:
    explicit FakeWorld(double groundHeight) noexcept : m_ground(groundHeight) {}

    [[nodiscard]] bool IsSolid(double x, double y, double z) const override {
        if (y <= m_ground) {
            return true;
        }
        if (m_sphereRadius > 0.0) {
            const double dx = x - m_sphereCenter.x;
            const double dy = y - m_sphereCenter.y;
            const double dz = z - m_sphereCenter.z;
            return (dx * dx + dy * dy + dz * dz) <= m_sphereRadius * m_sphereRadius;
        }
        return false;
    }

    void SetSphere(const glm::dvec3& center, double radius) noexcept {
        m_sphereCenter = center;
        m_sphereRadius = radius;
    }

private:
    double     m_ground = 0.0;
    glm::dvec3 m_sphereCenter { 0.0 };
    double     m_sphereRadius = 0.0;
};

/// 发射一枚弹丸（池容量 4）并返回其下标 0 的槽位。
[[nodiscard]] Orb& FireOne(OrbPool& pool, const ProjectileSpec& spec, const glm::dvec3& origin,
                           const glm::vec3& direction) {
    EXPECT_TRUE(pool.Fire(origin, direction, spec));
    return pool.Orbs()[0];
}

}  // namespace

// 命中检测：垂直向下的弹道在地面上命中，命中点落在表面附近。
TEST(Orb, MarchRayHitsGroundNearSurface) {
    const FakeWorld world(0.0);

    const OrbHit hit = vx::MarchRay(world, glm::dvec3(0.0, 10.0, 0.0), glm::dvec3(0.0, -10.0, 0.0));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y, 0.0, 0.25) << "命中点应落在表面附近（半个采样步长内）";
    EXPECT_NEAR(hit.point.x, 0.0, 1.0e-9);
}

// 命中检测：未触到实心域时不报告命中。
TEST(Orb, MarchRayReportsNoHitAboveGround) {
    const FakeWorld world(0.0);
    const OrbHit   hit = vx::MarchRay(world, glm::dvec3(0.0, 10.0, 0.0), glm::dvec3(0.0, 5.0, 0.0));
    EXPECT_FALSE(hit.hit);
}

// 命中检测：可挖体积那种"山头"用实心球模拟——侧向射入应命中山体表面（这正是 T27 的目标场景）。
TEST(Orb, MarchRayHitsSphereFromSide) {
    FakeWorld world(-1000.0);  // 地面放得很低，只测球体
    world.SetSphere(glm::dvec3(50.0, 100.0, 0.0), 8.0);

    const OrbHit hit = vx::MarchRay(world, glm::dvec3(0.0, 100.0, 0.0), glm::dvec3(80.0, 100.0, 0.0));
    ASSERT_TRUE(hit.hit) << "从侧面射向实心球必须命中";
    const double distance = glm::length(hit.point - glm::dvec3(50.0, 100.0, 0.0));
    EXPECT_NEAR(distance, 8.0, 0.3) << "命中点应贴在球面上";
}

// 弹道：无命中时满足离散解析解（半隐式欧拉），并与连续抛物线一致。
TEST(Orb, BallisticMatchesDiscreteAnalyticSolution) {
    const FakeWorld world(-10000.0);  // 永远命中不到

    ProjectileSpec spec;
    spec.id             = "test";
    spec.speed          = 40.0F;
    spec.gravityScale   = 1.0F;
    spec.lifetimeSeconds = 10.0F;

    OrbPool pool(4);
    Orb&    orb = FireOne(pool, spec, glm::dvec3(0.0, 100.0, 0.0), glm::vec3(1.0F, 0.0F, 0.0F));

    constexpr float  kGravity     = 24.0F;
    constexpr float  kSpeed       = 40.0F;
    constexpr int    kSteps       = 60;  // 1 秒
    constexpr double kDt          = static_cast<double>(kStepSeconds);

    OrbHit hit;
    for (int step = 0; step < kSteps; ++step) {
        ASSERT_FALSE(vx::StepOrb(orb, world, kGravity, spec.gravityScale, kStepSeconds, hit));
        ASSERT_TRUE(orb.active);
    }

    // 离散解：每次先 v.y -= g·dt 再 p += v·dt ⇒ p.y = y0 − g·dt²·n(n+1)/2
    // （容差 1e-4 覆盖 `float` 累加 60 步的舍入误差）
    const double expectedDrop = static_cast<double>(kGravity) * kDt * kDt *
                                static_cast<double>(kSteps) * static_cast<double>(kSteps + 1) * 0.5;
    EXPECT_NEAR(orb.position.y, 100.0 - expectedDrop, 1.0e-4);
    EXPECT_NEAR(orb.position.x, static_cast<double>(kSpeed) * kDt * static_cast<double>(kSteps), 1.0e-4);

    // 与连续抛物线（y = y0 − ½gt²）的偏差不超过一个步长量级。
    const double continuousDrop = 0.5 * static_cast<double>(kGravity) * 1.0 * 1.0;
    EXPECT_NEAR(orb.position.y, 100.0 - continuousDrop, 0.3) << "固定步长的离散弹道须与连续解一致（不随帧率漂移）";
}

// 弹丸命中后失效并给出爆炸点（供上层触发破坏）；超时弹丸静默失效。
TEST(Orb, OrbDeactivatesOnHitAndOnLifetimeEnd) {
    const FakeWorld world(0.0);

    ProjectileSpec spec;
    spec.id              = "test";
    spec.speed           = 40.0F;
    spec.gravityScale    = 0.0F;  // 无重力：水平直飞
    spec.lifetimeSeconds = 10.0F;

    OrbPool pool(4);
    Orb&    orb = FireOne(pool, spec, glm::dvec3(0.0, 10.0, 0.0), glm::vec3(0.0F, -1.0F, 0.0F));

    OrbHit    hit;
    bool      reported = false;
    const int maxSteps = 600;
    for (int step = 0; step < maxSteps && !reported; ++step) {
        reported = vx::StepOrb(orb, world, 24.0F, spec.gravityScale, kStepSeconds, hit);
    }
    EXPECT_TRUE(reported) << "向地面发射必须在有限步内命中";
    EXPECT_FALSE(orb.active);
    EXPECT_NEAR(hit.point.y, 0.0 + 0.0, 0.25);

    // 超时：把存活时间设成 1 步，第二次推进即失效且不报命中。
    spec.lifetimeSeconds = kStepSeconds;
    spec.gravityScale    = 0.0F;
    Orb& timed           = FireOne(pool, spec, glm::dvec3(0.0, 100.0, 0.0), glm::vec3(1.0F, 0.0F, 0.0F));
    OrbHit ignored;
    EXPECT_FALSE(vx::StepOrb(timed, world, 0.0F, 0.0F, kStepSeconds, ignored));
    EXPECT_FALSE(timed.active) << "存活时间到期后弹丸必须失效（且不产生命中）";
}

// 池：容量上限生效；零方向被拒绝（不产生 NaN 速度）。
TEST(Orb, PoolRespectsCapacityAndRejectsZeroDirection) {
    const FakeWorld world(-1000.0);

    ProjectileSpec spec;
    spec.speed           = 10.0F;
    spec.lifetimeSeconds = 10.0F;

    OrbPool pool(2);
    EXPECT_FALSE(pool.Fire(glm::dvec3(0.0), glm::vec3(0.0F), spec)) << "零方向必须被拒绝";
    EXPECT_TRUE(pool.Fire(glm::dvec3(0.0), glm::vec3(1.0F, 0.0F, 0.0F), spec));
    EXPECT_TRUE(pool.Fire(glm::dvec3(0.0), glm::vec3(1.0F, 0.0F, 0.0F), spec));
    EXPECT_FALSE(pool.Fire(glm::dvec3(0.0), glm::vec3(1.0F, 0.0F, 0.0F), spec)) << "池满必须拒绝（不淘汰已有弹丸）";
    EXPECT_EQ(pool.ActiveCount(), 2U);

    // 方向会被归一化：速度大小恒为 speed。
    EXPECT_NEAR(glm::length(pool.Orbs()[0].velocity), spec.speed, 1.0e-4F);
}

// 程序化球网格：顶点 / 索引数量、半径、单位法线、索引范围。
TEST(OrbMesh, BuildsUnitSphereAtRequestedRadius) {
    constexpr float       kRadius = 0.6F;
    constexpr int         kRadial = 12;
    constexpr int         kRings  = 8;
    const vx::MeshData    mesh    = vx::BuildOrbMesh(kRadius, kRadial, kRings);

    EXPECT_EQ(mesh.vertices.size(), static_cast<std::size_t>(kRings + 1) * static_cast<std::size_t>(kRadial + 1));
    EXPECT_EQ(mesh.indices.size(), static_cast<std::size_t>(kRings) * static_cast<std::size_t>(kRadial) * 6U);

    for (const vx::MeshVertex& vertex : mesh.vertices) {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        EXPECT_NEAR(glm::length(position), kRadius, 1.0e-4F) << "顶点必须落在球面上";
        EXPECT_NEAR(glm::length(normal), 1.0F, 1.0e-4F);
        EXPECT_NEAR(glm::dot(glm::normalize(position), normal), 1.0F, 1.0e-3F) << "法线必须朝外";
    }
    for (const std::uint32_t index : mesh.indices) {
        EXPECT_LT(index, mesh.vertices.size());
    }
}

// 半径非正 ⇒ 空网格（防御性：调用方不会这样用）。
TEST(OrbMesh, RejectsNonPositiveRadius) {
    EXPECT_TRUE(vx::BuildOrbMesh(0.0F).vertices.empty());
    EXPECT_TRUE(vx::BuildOrbMesh(-1.0F).indices.empty());
}

// 弹丸表：仓库内文件可加载、含 `light_orb`，且与内置默认一致；文件缺失即抛异常。
TEST(ProjectileTable, LoadsShippedTableAndRejectsMissingFile) {
    const ProjectileTable fromFile =
        ProjectileTable::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/projectiles.toml");
    ASSERT_EQ(fromFile.Projectiles().size(), 1U);
    EXPECT_EQ(fromFile.Projectiles()[0].id, "light_orb");
    EXPECT_GT(fromFile.MaxActive(), 0);
    EXPECT_GT(fromFile.DefaultProjectile().speed, 0.0F);
    EXPECT_GT(fromFile.DefaultProjectile().explosionRadiusBlocks, 0.0F);

    const ProjectileTable defaults = ProjectileTable::Default();
    ASSERT_EQ(defaults.Projectiles().size(), 1U);
    EXPECT_EQ(defaults.Projectiles()[0].id, fromFile.Projectiles()[0].id);
    EXPECT_FLOAT_EQ(defaults.Projectiles()[0].speed, fromFile.Projectiles()[0].speed);
    EXPECT_FLOAT_EQ(defaults.Projectiles()[0].explosionRadiusBlocks,
                    fromFile.Projectiles()[0].explosionRadiusBlocks);

    EXPECT_THROW(static_cast<void>(ProjectileTable::LoadFromFile(std::filesystem::path("no_such_projectiles.toml"))),
                 std::runtime_error)
        << "文件缺失必须抛异常（禁止静默回退）";
}
