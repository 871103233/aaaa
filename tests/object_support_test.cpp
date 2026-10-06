// 物件层（ADR 0004 层③）V0c 单测：底面**支撑探测点**的纯函数语义
// （中心 + 四角、绕 Y 旋转与渲染同向、任一实心即有支撑）。见 docs/plans/v0.5.md §1.5。

#include "object/object_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

namespace {

using vx::kObjectSupportProbeCount;
using vx::ObjectHasSupport;
using vx::ObjectSupportProbes;

constexpr float kEps = 1.0e-4F;

TEST(ObjectSupport, ProbesAreCentrePlusFourCorners) {
    const auto probes = ObjectSupportProbes(1.0F, 0.5F, 0.0F);
    ASSERT_EQ(probes.size(), kObjectSupportProbeCount);
    // 首个探测点 = 底面中心。
    EXPECT_NEAR(probes[0].x, 0.0F, kEps);
    EXPECT_NEAR(probes[0].z, 0.0F, kEps);

    // 四角 = (±halfX, ±halfZ)（yaw = 0 ⇒ 与局部坐标一致）。
    float maxX = 0.0F;
    float maxZ = 0.0F;
    for (std::size_t i = 1; i < probes.size(); ++i) {
        maxX = std::max(maxX, std::fabs(probes[i].x));
        maxZ = std::max(maxZ, std::fabs(probes[i].z));
    }
    EXPECT_NEAR(maxX, 1.0F, kEps);
    EXPECT_NEAR(maxZ, 0.5F, kEps);
}

TEST(ObjectSupport, RotationMatchesRenderYawConvention) {
    // 绕 +Y 右手旋转 90°：(x, z) → (z, -x)，与 `RotateMeshAboutY` 同一约定。
    const auto probes = ObjectSupportProbes(2.0F, 1.0F, 90.0F);
    bool found = false;
    for (std::size_t i = 1; i < probes.size(); ++i) {
        // 原角点 (2, -1) 旋转后应为 (-1, -2)。
        if (std::fabs(probes[i].x - (-1.0F)) < kEps && std::fabs(probes[i].z - (-2.0F)) < kEps) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(ObjectSupport, SquareFootprintProbeExtentIsRotationInvariant) {
    const float expected = 3.0F;  // halfX == halfZ == 3 ⇒ 任意朝向下角点距中心都相同
    for (const float yaw : { 0.0F, 17.0F, 45.0F, 90.0F, 200.0F }) {
        const auto probes = ObjectSupportProbes(expected, expected, yaw);
        for (std::size_t i = 1; i < probes.size(); ++i) {
            const float radius = std::sqrt(probes[i].x * probes[i].x + probes[i].z * probes[i].z);
            EXPECT_NEAR(radius, expected * std::sqrt(2.0F), kEps) << "yaw = " << yaw;
        }
    }
}

TEST(ObjectSupport, HasSupportIsTrueWhenAnyProbeIsSolid) {
    const bool onlyOneSolid[kObjectSupportProbeCount] = { false, false, true, false, false };
    EXPECT_TRUE(ObjectHasSupport(onlyOneSolid, kObjectSupportProbeCount));

    const bool noneSolid[kObjectSupportProbeCount] = { false, false, false, false, false };
    EXPECT_FALSE(ObjectHasSupport(noneSolid, kObjectSupportProbeCount));

    const bool allSolid[kObjectSupportProbeCount] = { true, true, true, true, true };
    EXPECT_TRUE(ObjectHasSupport(allSolid, kObjectSupportProbeCount));
}

}  // namespace
