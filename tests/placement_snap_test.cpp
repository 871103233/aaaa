// 放置吸附与对齐的纯函数测试（V0.11 / I1；ADR 0038 决策四"吸附"）。
//
// 判据（可判定）：
//   1. 平移吸附 ⇒ 结果 = 步长整数倍（就近取整），同输入恒定；
//   2. 旋转吸附 ⇒ 结果 = 步长整数倍且落在 [0, 360)；
//   3. 步长 <= 0 ⇒ **关闭**：平移逐位返回输入、朝向仅归一化（"引入前逐位一致"的对照口径）；
//   4. 非法输入（NaN）⇒ 不产生 NaN（不崩、不静默传播）。

#include "object/placement_snap.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using vx::PlacementFootprint;
using vx::PlacementSnapSettings;
using vx::SnapPlacementToNeighbor;
using vx::SnapToStep;
using vx::SnapYawDegrees;

PlacementFootprint Neighbor(double x, double z, double halfX, double halfZ, double yawDegrees) {
    PlacementFootprint n;
    n.x          = x;
    n.z          = z;
    n.halfX      = halfX;
    n.halfZ      = halfZ;
    n.yawDegrees = yawDegrees;
    return n;
}

}  // namespace

TEST(PlacementSnap, TranslateExactMultiplesAreUnchanged) {
    EXPECT_DOUBLE_EQ(SnapToStep(0.0, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapToStep(3.0, 1.0), 3.0);
    EXPECT_DOUBLE_EQ(SnapToStep(-7.0, 1.0), -7.0);
    EXPECT_DOUBLE_EQ(SnapToStep(2.5, 0.5), 2.5);
}

TEST(PlacementSnap, TranslateRoundsToNearestMultiple) {
    EXPECT_DOUBLE_EQ(SnapToStep(1.4, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(SnapToStep(1.6, 1.0), 2.0);
    EXPECT_DOUBLE_EQ(SnapToStep(-1.4, 1.0), -1.0);
    EXPECT_DOUBLE_EQ(SnapToStep(-1.6, 1.0), -2.0);
    // 结果必为步长的整数倍（可判定）。
    const double step = 0.25;
    for (int i = 0; i < 16; ++i) {
        const double snapped = SnapToStep(0.11 * static_cast<double>(i), step);
        EXPECT_NEAR(snapped / step, std::round(snapped / step), 1e-9);
    }
}

TEST(PlacementSnap, TranslateStepNonPositiveDisablesSnapping) {
    // 关闭吸附 ⇒ 逐位返回输入（不吸附）。
    EXPECT_DOUBLE_EQ(SnapToStep(1.2345, 0.0), 1.2345);
    EXPECT_DOUBLE_EQ(SnapToStep(-0.9876, -3.0), -0.9876);
}

TEST(PlacementSnap, TranslateNaNDoesNotProduceGarbage) {
    // NaN 输入 ⇒ 原样返回（仍是 NaN），绝不产生"看似有效"的坐标。
    EXPECT_TRUE(std::isnan(SnapToStep(std::nan(""), 1.0)));
    // 有限输入 + 非法步长 ⇒ 原样有限值。
    EXPECT_DOUBLE_EQ(SnapToStep(2.75, std::nan("")), 2.75);
}

TEST(PlacementSnap, YawSnapsToStep) {
    EXPECT_DOUBLE_EQ(SnapYawDegrees(0.0, 15.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(14.0, 15.0), 15.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(16.0, 15.0), 15.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(22.0, 15.0), 15.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(23.0, 15.0), 30.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(90.0, 15.0), 90.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(11.0, 90.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(46.0, 90.0), 90.0);
}

TEST(PlacementSnap, YawNormalizesIntoZeroTo360) {
    EXPECT_DOUBLE_EQ(SnapYawDegrees(-15.0, 15.0), 345.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(360.0, 15.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(359.0, 15.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(720.0, 15.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(-370.0, 15.0), 345.0);  // −370 就近吸附到 −375 ⇒ 归一化 345
    // 值域恒在 [0, 360)。
    const double step = 15.0;
    for (int i = -40; i <= 40; ++i) {
        const double yaw = SnapYawDegrees(static_cast<double>(i) * 13.0, step);
        EXPECT_GE(yaw, 0.0);
        EXPECT_LT(yaw, 360.0);
    }
}

TEST(PlacementSnap, YawStepNonPositiveNormalizesOnly) {
    // 关闭旋转吸附 ⇒ 仅归一化（与既有 `fmod(v + 360, 360)` 口径一致）。
    EXPECT_DOUBLE_EQ(SnapYawDegrees(200.0, 0.0), 200.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(-90.0, -1.0), 270.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(450.0, 0.0), 90.0);
}

TEST(PlacementSnap, YawNaNTreatedAsZero) {
    EXPECT_DOUBLE_EQ(SnapYawDegrees(std::nan(""), 15.0), 0.0);
    EXPECT_DOUBLE_EQ(SnapYawDegrees(std::nan(""), 0.0), 0.0);
}

TEST(PlacementSnap, DefaultsAreEnabledAndDeterministic) {
    const PlacementSnapSettings settings;  // 缺省 = 开启（首期交互完善即此项）
    EXPECT_GT(settings.translateBlocks, 0.0);
    EXPECT_GT(settings.yawDegrees, 0.0);
    EXPECT_DOUBLE_EQ(settings.translateBlocks, 0.25);  // V0.11 I1c：缺省步长 1.0 → 0.25
    EXPECT_DOUBLE_EQ(settings.yawDegrees, 1.0);        // V0.11（2026-10-08）：缺省旋转吸附 15 → 1（精细调整）
    EXPECT_DOUBLE_EQ(settings.neighborRadiusBlocks, 2.0);

    // 确定性：同输入 ⇒ 逐位相同（红线 7）。
    for (int i = 0; i < 8; ++i) {
        const double value = 1.3 + 0.17 * static_cast<double>(i);
        EXPECT_EQ(SnapToStep(value, settings.translateBlocks), SnapToStep(value, settings.translateBlocks));
        EXPECT_EQ(SnapYawDegrees(value * 10.0, settings.yawDegrees),
                  SnapYawDegrees(value * 10.0, settings.yawDegrees));
    }
}

// ---- 邻居优先吸附（V0.11 / I1c）----

TEST(PlacementSnap, NeighborSnapIsSkippedWhenDisabledOrEmpty) {
    double x = 0.0;
    double z = 0.0;
    const std::vector<PlacementFootprint> none;
    EXPECT_FALSE(SnapPlacementToNeighbor(3.0, 0.1, 0.5, 0.5, none, 2.0, 0.25, x, z));  // 列表空
    const std::vector<PlacementFootprint> one{ Neighbor(0.0, 0.0, 1.0, 1.0, 0.0) };
    EXPECT_FALSE(SnapPlacementToNeighbor(3.0, 0.1, 0.5, 0.5, one, 0.0, 0.25, x, z));  // 半径 0 = 关闭
}

TEST(PlacementSnap, NeighborSnapFlushesToTheNearestFace) {
    const std::vector<PlacementFootprint> neighbors{ Neighbor(0.0, 0.0, 1.0, 1.0, 0.0) };
    double x = 0.0;
    double z = 0.0;
    // 从 +X 靠近 ⇒ 贴 +X 面：x = 1 + 0.5 = 1.5；另一轴按 0.25 量化（0.1 → 0.0）。
    ASSERT_TRUE(SnapPlacementToNeighbor(3.0, 0.1, 0.5, 0.5, neighbors, 5.0, 0.25, x, z));
    EXPECT_NEAR(x, 1.5, 1e-9);
    EXPECT_NEAR(z, 0.0, 1e-9);
}

TEST(PlacementSnap, NeighborSnapRespectsNeighborYaw) {
    // 邻居绕 Y 转 90°：其 −X 面在世界里朝向 +Z。
    const std::vector<PlacementFootprint> neighbors{ Neighbor(0.0, 0.0, 1.0, 1.0, 90.0) };
    double x = 0.0;
    double z = 0.0;
    ASSERT_TRUE(SnapPlacementToNeighbor(0.0, 3.0, 0.5, 0.5, neighbors, 5.0, 0.25, x, z));
    EXPECT_NEAR(x, 0.0, 1e-9);
    EXPECT_NEAR(z, 1.5, 1e-9);
}

TEST(PlacementSnap, NeighborSnapFallsBackWhenOutOfRadius) {
    const std::vector<PlacementFootprint> neighbors{ Neighbor(0.0, 0.0, 1.0, 1.0, 0.0) };
    double x = 0.0;
    double z = 0.0;
    // 点距足迹 9 格 > 半径 2 ⇒ 找不到邻居（调用方退回世界网格）。
    EXPECT_FALSE(SnapPlacementToNeighbor(10.0, 0.0, 0.5, 0.5, neighbors, 2.0, 0.25, x, z));
}

TEST(PlacementSnap, NeighborSnapPicksTheClosestNeighbor) {
    const std::vector<PlacementFootprint> neighbors{
        Neighbor(0.0, 0.0, 1.0, 1.0, 0.0),
        Neighbor(10.0, 0.0, 1.0, 1.0, 0.0),
    };
    double x = 0.0;
    double z = 0.0;
    ASSERT_TRUE(SnapPlacementToNeighbor(12.0, 0.05, 0.5, 0.5, neighbors, 5.0, 0.25, x, z));
    EXPECT_NEAR(x, 11.5, 1e-9);  // 贴到 10.0 那个邻居的 +X 面
    EXPECT_NEAR(z, 0.0, 1e-9);
}
