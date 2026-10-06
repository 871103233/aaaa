// 传送门交互（V3）单测：**最近门**纯函数的确定性语义 —— 半径内取最近、半径外无门、
// 边界取闭区间、等距取先出现者。见 docs/plans/v0.5.md §1.8。

#include "portal_interaction.hpp"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::FindNearestPortal;
using vx::PortalEntry;

[[nodiscard]] PortalEntry MakePortal(double x, double y, double z, std::string target) {
    PortalEntry portal;
    portal.position      = glm::dvec3(x, y, z);
    portal.targetWorldId = std::move(target);
    return portal;
}

TEST(PortalInteraction, EmptyListReturnsNull) {
    const std::vector<PortalEntry> portals;
    EXPECT_EQ(FindNearestPortal(portals, glm::dvec3(0.0), 10.0), nullptr);
}

TEST(PortalInteraction, PicksNearestWithinRadius) {
    const std::vector<PortalEntry> portals = {
        MakePortal(10.0, 0.0, 0.0, "world_b"),
        MakePortal(2.0, 0.0, 0.0, "world_c"),
        MakePortal(-3.0, 0.0, 0.0, "world_a"),
    };
    const PortalEntry* nearest = FindNearestPortal(portals, glm::dvec3(0.0), 5.0);
    ASSERT_NE(nearest, nullptr);
    EXPECT_EQ(nearest->targetWorldId, "world_c");  // 距原点 2 格，最近
}

TEST(PortalInteraction, OutOfRadiusReturnsNull) {
    const std::vector<PortalEntry> portals = { MakePortal(20.0, 0.0, 0.0, "world_b") };
    EXPECT_EQ(FindNearestPortal(portals, glm::dvec3(0.0), 4.0), nullptr);
}

TEST(PortalInteraction, BoundaryIsInclusive) {
    const std::vector<PortalEntry> portals = { MakePortal(3.0, 0.0, 0.0, "world_b") };
    // 距离恰好 = 半径 ⇒ 命中（闭区间），与 `distanceSq > radiusSq ⇒ 跳过` 一致。
    ASSERT_NE(FindNearestPortal(portals, glm::dvec3(0.0), 3.0), nullptr);
    // 略小于半径 ⇒ 仍命中。
    ASSERT_NE(FindNearestPortal(portals, glm::dvec3(0.0), 3.0000001), nullptr);
}

TEST(PortalInteraction, EqualDistancePicksFirstOccurrence) {
    const std::vector<PortalEntry> portals = {
        MakePortal(4.0, 0.0, 0.0, "first"),
        MakePortal(-4.0, 0.0, 0.0, "second"),
    };
    const PortalEntry* nearest = FindNearestPortal(portals, glm::dvec3(0.0), 8.0);
    ASSERT_NE(nearest, nullptr);
    EXPECT_EQ(nearest->targetWorldId, "first");  // 等距 ⇒ 先出现者（确定性，红线 7）
}

TEST(PortalInteraction, UsesFullThreeDimensionalDistance) {
    const std::vector<PortalEntry> portals = { MakePortal(0.0, 0.0, 0.0, "below") };
    // 竖直偏移 5 格 > 半径 4 ⇒ 视为不在附近（避免"隔着楼层"误触发）。
    EXPECT_EQ(FindNearestPortal(portals, glm::dvec3(0.0, 5.0, 0.0), 4.0), nullptr);
}

}  // namespace
