// 地形采样（V0.6 C4 + C7）单测：`SlopeDegreesFromGradient` / `SlopeDegreesFromNeighbors` / `SamplePlacement`
//（含温度 / 湿度两路气候回调）。
// 见 docs/adr/0033-world-content-placement-and-streaming.md 决策一与 docs/plans/v0.6.md C4 / C7。

#include "object/terrain_sampling.hpp"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace {

using vx::LandformKind;
using vx::PlacementRule;
using vx::PlacementSample;

/// 采样回调：固定高度 / 地貌 / 气候（大多数用例只关心坡度与区间判据）。
[[nodiscard]] auto FlatHeight(float value) {
    return [value](float, float) { return value; };
}
[[nodiscard]] auto FixedLandform(LandformKind kind) {
    return [kind](float, float) { return kind; };
}
[[nodiscard]] auto FlatClimate(float value) {
    return [value](float, float) { return value; };
}

TEST(SlopeDegrees, FlatGradientIsZero) {
    EXPECT_FLOAT_EQ(vx::SlopeDegreesFromGradient(0.0F, 0.0F), 0.0F);
}

TEST(SlopeDegrees, UnitGradientIsFortyFiveDegrees) {
    EXPECT_NEAR(vx::SlopeDegreesFromGradient(1.0F, 0.0F), 45.0F, 1.0e-3);
    EXPECT_NEAR(vx::SlopeDegreesFromGradient(0.0F, 1.0F), 45.0F, 1.0e-3);
    EXPECT_NEAR(vx::SlopeDegreesFromGradient(-1.0F, 0.0F), 45.0F, 1.0e-3);  // 方向不影响坡度
}

TEST(SlopeDegrees, DiagonalGradientMatchesAtanSqrtTwo) {
    EXPECT_NEAR(vx::SlopeDegreesFromGradient(1.0F, 1.0F), 54.73561F, 1.0e-3);
}

TEST(SlopeDegrees, NeighborsOfFlatPlaneIsZero) {
    const float halfStep = 1.0F;
    EXPECT_NEAR(vx::SlopeDegreesFromNeighbors(3.0F, 3.0F, 3.0F, 3.0F, halfStep), 0.0F, 1.0e-3);
}

TEST(SlopeDegrees, NeighborsOfRampUsesCentralDifference) {
    // 平面 h = 2x ⇒ 中心差分 dh/dx = 2 ⇒ atan(2) ≈ 63.435°（与 halfStep 无关）。
    const float halfStep = 1.0F;
    const float x        = 10.0F;
    EXPECT_NEAR(vx::SlopeDegreesFromNeighbors(2.0F * (x - halfStep), 2.0F * (x + halfStep), 2.0F * x, 2.0F * x,
                                              halfStep),
                std::atan(2.0) * 180.0 / 3.14159265358979323846, 1.0e-3);
}

TEST(SlopeDegrees, NonPositiveHalfStepIsTreatedAsFlat) {
    EXPECT_FLOAT_EQ(vx::SlopeDegreesFromNeighbors(1.0F, 5.0F, 0.0F, 9.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(vx::SlopeDegreesFromNeighbors(1.0F, 5.0F, 0.0F, 9.0F, -2.0F), 0.0F);
}

TEST(SamplePlacement, FlatTerrainYieldsZeroSlopeAndGivenLandformAndClimate) {
    const PlacementSample sample = vx::SamplePlacement(7.0F, -3.0F, 1.0F, FlatHeight(120.0F),
                                                       FixedLandform(LandformKind::Mountains), FlatClimate(0.8F),
                                                       FlatClimate(0.2F));
    EXPECT_FLOAT_EQ(sample.heightBlocks, 120.0F);
    EXPECT_NEAR(sample.slopeDegrees, 0.0F, 1.0e-3);
    EXPECT_EQ(sample.landform, LandformKind::Mountains);
    EXPECT_FLOAT_EQ(sample.temperature, 0.8F);
    EXPECT_FLOAT_EQ(sample.humidity, 0.2F);
}

TEST(SamplePlacement, RampTerrainYieldsExpectedSlope) {
    const auto heightAt = [](float x, float) { return x; };  // h = x ⇒ 45°

    const PlacementSample sample = vx::SamplePlacement(50.0F, 0.0F, 1.0F, heightAt, FixedLandform(LandformKind::Hills),
                                                       FlatClimate(0.5F), FlatClimate(0.5F));
    EXPECT_NEAR(sample.heightBlocks, 50.0F, 1.0e-3);
    EXPECT_NEAR(sample.slopeDegrees, 45.0F, 1.0e-3);
    EXPECT_EQ(sample.landform, LandformKind::Hills);
}

TEST(SamplePlacement, FeedsPlacementRuleGate) {
    // 陡坡（h = 4x ⇒ atan(4) ≈ 75.96°）⇒ 默认规则（max 45°）拒绝；平缓坡（h = 0.1x）⇒ 通过。
    PlacementRule rule;
    rule.maxSlopeDegrees = 45.0F;
    rule.allowMountains  = false;

    const auto steepHeight = [](float x, float) { return 4.0F * x; };
    const auto mildHeight  = [](float x, float) { return 0.1F * x; };

    EXPECT_FALSE(vx::IsPlacementAllowed(
        rule, vx::SamplePlacement(10.0F, 0.0F, 1.0F, steepHeight, FixedLandform(LandformKind::Hills), FlatClimate(0.5F),
                                  FlatClimate(0.5F))));
    EXPECT_TRUE(vx::IsPlacementAllowed(
        rule, vx::SamplePlacement(10.0F, 0.0F, 1.0F, mildHeight, FixedLandform(LandformKind::Hills), FlatClimate(0.5F),
                                  FlatClimate(0.5F))));
}

TEST(SamplePlacement, ClimateFeedsPlacementRuleGate) {
    // 只在**湿润**地区放置：湿度 0.2 拒、0.8 通过（V0.6 C7 的气候判据）。
    PlacementRule rule;
    rule.minHumidity = 0.5F;

    EXPECT_FALSE(vx::IsPlacementAllowed(
        rule, vx::SamplePlacement(0.0F, 0.0F, 1.0F, FlatHeight(100.0F), FixedLandform(LandformKind::Hills),
                                  FlatClimate(0.5F), FlatClimate(0.2F))));
    EXPECT_TRUE(vx::IsPlacementAllowed(
        rule, vx::SamplePlacement(0.0F, 0.0F, 1.0F, FlatHeight(100.0F), FixedLandform(LandformKind::Hills),
                                  FlatClimate(0.5F), FlatClimate(0.8F))));
}

}  // namespace
