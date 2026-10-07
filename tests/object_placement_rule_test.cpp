// 地形感知放置规则（V0.6 C1/C2）单测：`IsPlacementAllowed` 的四项判据 + `PlanTileCandidates` 的
// 分块确定性 / tile 局部 / 不重叠。见 docs/adr/0033-world-content-placement-and-streaming.md 与 docs/plans/v0.6.md。

#include "object/object_placement_rule.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::LandformKind;
using vx::PlacementRule;
using vx::PlacementSample;
using vx::ScatterPoint;

[[nodiscard]] PlacementSample MakeSample(float height, float slope, LandformKind landform) {
    PlacementSample sample;
    sample.heightBlocks = height;
    sample.slopeDegrees = slope;
    sample.landform     = landform;
    return sample;
}

[[nodiscard]] double Distance(const ScatterPoint& left, const ScatterPoint& right) {
    const double dx = static_cast<double>(left.x) - static_cast<double>(right.x);
    const double dz = static_cast<double>(left.z) - static_cast<double>(right.z);
    return std::sqrt(dx * dx + dz * dz);
}

// ---------------------------------------------------------------- C1：规则判据

TEST(PlacementRule, SlopeGateUsesClosedInterval) {
    PlacementRule rule;
    rule.minSlopeDegrees = 10.0F;
    rule.maxSlopeDegrees = 40.0F;

    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 9.9F, LandformKind::Hills)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 10.0F, LandformKind::Hills)));  // 含端点
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 25.0F, LandformKind::Hills)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 40.0F, LandformKind::Hills)));  // 含端点
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 40.1F, LandformKind::Hills)));
}

TEST(PlacementRule, HeightGateUsesClosedInterval) {
    PlacementRule rule;
    rule.minHeightBlocks = 100.0F;
    rule.maxHeightBlocks = 200.0F;

    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(99.0F, 0.0F, LandformKind::Plains)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 0.0F, LandformKind::Plains)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(200.0F, 0.0F, LandformKind::Plains)));
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(201.0F, 0.0F, LandformKind::Plains)));
}

TEST(PlacementRule, LandformGateRespectsAllowedSet) {
    PlacementRule rule;
    rule.allowPlains    = false;
    rule.allowHills     = true;
    rule.allowMountains = true;

    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 0.0F, LandformKind::Plains)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 0.0F, LandformKind::Hills)));
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, 0.0F, LandformKind::Mountains)));
}

TEST(PlacementRule, ClimateGateUsesClosedInterval) {
    // V0.6 C7：温度 / 湿度按闭区间判定；缺省全区间 ⇒ 不约束。
    PlacementRule rule;
    rule.minTemperature = 0.3F;
    rule.maxTemperature = 0.7F;
    rule.minHumidity    = 0.4F;

    PlacementSample sample = MakeSample(100.0F, 0.0F, LandformKind::Hills);
    sample.temperature     = 0.5F;
    sample.humidity        = 0.6F;
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, sample));

    sample.temperature = 0.3F;  // 含下界
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, sample));
    sample.temperature = 0.29F;
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, sample));

    sample.temperature = 0.5F;
    sample.humidity    = 0.39F;
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, sample));
    sample.humidity = 0.40F;  // 含下界
    EXPECT_TRUE(vx::IsPlacementAllowed(rule, sample));

    // 缺省规则（全区间）⇒ 任意气候都通过。
    const PlacementRule defaults;
    PlacementSample     anyClimate = MakeSample(100.0F, 0.0F, LandformKind::Hills);
    anyClimate.temperature          = 0.0F;
    anyClimate.humidity             = 1.0F;
    EXPECT_TRUE(vx::IsPlacementAllowed(defaults, anyClimate));
}

TEST(PlacementRule, NaNSampleIsRejected) {
    const PlacementRule rule;
    const float         nan = std::nanf("");
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(nan, 0.0F, LandformKind::Plains)));
    EXPECT_FALSE(vx::IsPlacementAllowed(rule, MakeSample(100.0F, nan, LandformKind::Plains)));
}

// ---------------------------------------------------------------- C2：分块确定性候选点

TEST(TileCandidates, SameInputIsBitForBitReproducible) {
    const PlacementRule              rule;
    const std::vector<ScatterPoint>  first  = vx::PlanTileCandidates(rule, 2001U, vx::TileCoord { 3, -2 });
    const std::vector<ScatterPoint>  second = vx::PlanTileCandidates(rule, 2001U, vx::TileCoord { 3, -2 });

    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i].x, second[i].x);  // 逐位（红线 7）
        EXPECT_FLOAT_EQ(first[i].z, second[i].z);
        EXPECT_FLOAT_EQ(first[i].yawDegrees, second[i].yawDegrees);
    }
}

TEST(TileCandidates, CountMatchesGridCellsPerSide) {
    PlacementRule rule;
    rule.cellBlocks = 16.0F;  // 64 / 16 = 4 ⇒ 4×4 = 16 个格点
    EXPECT_EQ(vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 }).size(), 16U);

    rule.cellBlocks = 32.0F;  // 2×2 = 4
    EXPECT_EQ(vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 }).size(), 4U);
}

TEST(TileCandidates, EveryPointStaysInsideItsTile) {
    const PlacementRule rule;  // cell = 16 ⇒ cellsPerSide = 4，格网覆盖 [origin, origin + 64)
    for (int tileX = -2; tileX <= 2; ++tileX) {
        for (int tileZ = -2; tileZ <= 2; ++tileZ) {
            const std::vector<ScatterPoint> points = vx::PlanTileCandidates(rule, 7U, vx::TileCoord { tileX, tileZ });
            const double minX = static_cast<double>(tileX) * 64.0;
            const double minZ = static_cast<double>(tileZ) * 64.0;
            for (const ScatterPoint& point : points) {
                EXPECT_GE(static_cast<double>(point.x), minX);
                EXPECT_LT(static_cast<double>(point.x), minX + 64.0);
                EXPECT_GE(static_cast<double>(point.z), minZ);
                EXPECT_LT(static_cast<double>(point.z), minZ + 64.0);
            }
        }
    }
}

TEST(TileCandidates, PointsNeverOverlapAndKeepHalfCellSpacing) {
    PlacementRule rule;
    rule.cellBlocks = 16.0F;
    const std::vector<ScatterPoint> points = vx::PlanTileCandidates(rule, 99U, vx::TileCoord { 5, 5 });
    ASSERT_EQ(points.size(), 16U);

    double minimum = 1.0e9;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1U; j < points.size(); ++j) {
            minimum = std::min(minimum, Distance(points[i], points[j]));
        }
    }
    EXPECT_GT(minimum, 0.5 * 16.0 - 1.0e-3) << "格内位置 ∈ [0.25, 0.75]×cell ⇒ 最小间距 > 0.5×cell";
}

TEST(TileCandidates, DifferentSeedsGiveDifferentLayouts) {
    const PlacementRule             rule;
    const std::vector<ScatterPoint> a = vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 });
    const std::vector<ScatterPoint> b = vx::PlanTileCandidates(rule, 2U, vx::TileCoord { 0, 0 });
    ASSERT_EQ(a.size(), b.size());

    bool differs = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differs = differs || (a[i].x != b[i].x) || (a[i].z != b[i].z);
    }
    EXPECT_TRUE(differs) << "换 seed 必须换一套布局";
}

TEST(TileCandidates, AdjacentTilesDoNotSharePoints) {
    // tile 归属 ⇒ 相邻两个 tile 的候选点集合**互不相交**（流式增删不会重复）。
    const PlacementRule             rule;
    const std::vector<ScatterPoint> left  = vx::PlanTileCandidates(rule, 11U, vx::TileCoord { 0, 0 });
    const std::vector<ScatterPoint> right = vx::PlanTileCandidates(rule, 11U, vx::TileCoord { 1, 0 });

    for (const ScatterPoint& l : left) {
        for (const ScatterPoint& r : right) {
            EXPECT_GT(Distance(l, r), 0.0) << "相邻 tile 不得重复计算同一个点";
        }
    }
}

TEST(TileCandidates, DegenerateRulesYieldNoPoints) {
    PlacementRule rule;
    rule.cellBlocks = 0.0F;
    EXPECT_TRUE(vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 }).empty());  // 步长 0

    rule.cellBlocks = -4.0F;
    EXPECT_TRUE(vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 }).empty());  // 负步长

    rule.cellBlocks = 128.0F;  // > tile 边长 64 ⇒ 一格都放不下
    EXPECT_TRUE(vx::PlanTileCandidates(rule, 1U, vx::TileCoord { 0, 0 }).empty());
}

}  // namespace
