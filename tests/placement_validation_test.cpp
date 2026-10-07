// 放置合法性（2D 有向矩形重叠）的纯函数测试（V0.11 / I2）。
//
// 判据（可判定）：
//   1. 明显分离 ⇒ 不重叠；**边界接触（间隙 0）⇒ 不算重叠**；
//   2. 有重叠（含旋转矩形）⇒ 重叠；
//   3. 对称（a↔b 结果一致）、确定性。

#include "object/placement_validation.hpp"

#include <gtest/gtest.h>

namespace {

using vx::FootprintsOverlap2D;
using vx::PlacementFootprint;

PlacementFootprint MakeFootprint(double x, double z, double halfX, double halfZ, double yawDegrees) {
    PlacementFootprint footprint;
    footprint.x          = x;
    footprint.z          = z;
    footprint.halfX      = halfX;
    footprint.halfZ      = halfZ;
    footprint.yawDegrees = yawDegrees;
    return footprint;
}

}  // namespace

TEST(PlacementValidation, SeparatedRectanglesDoNotOverlap) {
    const PlacementFootprint a = MakeFootprint(0.0, 0.0, 1.0, 1.0, 0.0);
    const PlacementFootprint b = MakeFootprint(3.0, 0.0, 1.0, 1.0, 0.0);
    EXPECT_FALSE(FootprintsOverlap2D(a, b));
}

TEST(PlacementValidation, TouchingEdgesAreNotOverlap) {
    // 中心距 2.0 == 半宽之和 ⇒ 面贴面（间隙 0）⇒ **不算重叠**。
    const PlacementFootprint a = MakeFootprint(0.0, 0.0, 1.0, 1.0, 0.0);
    const PlacementFootprint b = MakeFootprint(2.0, 0.0, 1.0, 1.0, 0.0);
    EXPECT_FALSE(FootprintsOverlap2D(a, b));
}

TEST(PlacementValidation, OverlappingRectanglesOverlap) {
    const PlacementFootprint a = MakeFootprint(0.0, 0.0, 1.0, 1.0, 0.0);
    const PlacementFootprint b = MakeFootprint(1.9, 0.0, 1.0, 1.0, 0.0);
    EXPECT_TRUE(FootprintsOverlap2D(a, b));
}

TEST(PlacementValidation, RotatedRectanglesRespectExtent) {
    const PlacementFootprint a = MakeFootprint(0.0, 0.0, 1.0, 1.0, 0.0);
    // 45° 旋转后，沿 X 的半投影 = |1·cos45| + |1·sin45| ≈ 1.414 ⇒ 中心距 1.0 仍重叠。
    const PlacementFootprint near45 = MakeFootprint(1.0, 0.0, 1.0, 1.0, 45.0);
    EXPECT_TRUE(FootprintsOverlap2D(a, near45));
    // 中心距 3.0 > 1.0 + 1.414 ⇒ 分离。
    const PlacementFootprint far45 = MakeFootprint(3.0, 0.0, 1.0, 1.0, 45.0);
    EXPECT_FALSE(FootprintsOverlap2D(a, far45));
}

TEST(PlacementValidation, RotatedAndUnrotatedSquaresStillOverlapWhenConcentric) {
    const PlacementFootprint a = MakeFootprint(0.0, 0.0, 1.0, 1.0, 0.0);
    const PlacementFootprint b = MakeFootprint(0.0, 0.0, 1.0, 1.0, 90.0);
    EXPECT_TRUE(FootprintsOverlap2D(a, b));
}

TEST(PlacementValidation, IsSymmetricAndDeterministic) {
    const PlacementFootprint a = MakeFootprint(0.3, -0.2, 1.5, 0.5, 37.0);
    const PlacementFootprint b = MakeFootprint(1.1, 0.4, 0.5, 1.5, -12.0);
    EXPECT_EQ(FootprintsOverlap2D(a, b), FootprintsOverlap2D(b, a));
    EXPECT_EQ(FootprintsOverlap2D(a, b), FootprintsOverlap2D(a, b));
}
