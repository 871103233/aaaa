// 程序化散布（V8）单测：`PlanObjectScatter` 的确定性、个数、半径约束、不重叠与洗牌子集语义。
// 见 docs/plans/v0.5.md §1.9（V8c）。

#include "object/object_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::ObjectScatter;
using vx::PlanObjectScatter;
using vx::ScatterPoint;

[[nodiscard]] ObjectScatter MakeScatter(float radius, int count, std::uint64_t seed) {
    ObjectScatter scatter;
    scatter.typeId  = "tree_default";
    scatter.centerX = 0.0F;
    scatter.centerZ = 0.0F;
    scatter.radius  = radius;
    scatter.count   = count;
    scatter.seed    = seed;
    return scatter;
}

[[nodiscard]] double Distance(const ScatterPoint& left, const ScatterPoint& right) {
    const double dx = static_cast<double>(left.x) - static_cast<double>(right.x);
    const double dz = static_cast<double>(left.z) - static_cast<double>(right.z);
    return std::sqrt(dx * dx + dz * dz);
}

TEST(ObjectScatter, ProducesRequestedCountInsideRadiusWithYawInRange) {
    const ObjectScatter             scatter = MakeScatter(80.0F, 18, 1001U);
    const std::vector<ScatterPoint> points  = PlanObjectScatter(scatter);

    ASSERT_EQ(points.size(), 18U);
    for (const ScatterPoint& point : points) {
        const double distance = std::sqrt(static_cast<double>(point.x) * static_cast<double>(point.x) +
                                          static_cast<double>(point.z) * static_cast<double>(point.z));
        EXPECT_LE(distance, 80.0 + 1.0e-3) << "点必须落在半径内（含抖动余量）";
        EXPECT_GE(point.yawDegrees, 0.0F);
        EXPECT_LT(point.yawDegrees, 360.0F);
    }
}

TEST(ObjectScatter, IsBitForBitReproducibleForTheSameSeed) {
    const ObjectScatter             scatter = MakeScatter(80.0F, 24, 777U);
    const std::vector<ScatterPoint> first   = PlanObjectScatter(scatter);
    const std::vector<ScatterPoint> second  = PlanObjectScatter(scatter);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i].x, second[i].x);  // 逐位（红线 7）
        EXPECT_FLOAT_EQ(first[i].z, second[i].z);
        EXPECT_FLOAT_EQ(first[i].yawDegrees, second[i].yawDegrees);
    }
}

TEST(ObjectScatter, DifferentSeedsGiveDifferentLayouts) {
    const std::vector<ScatterPoint> a = PlanObjectScatter(MakeScatter(80.0F, 18, 1U));
    const std::vector<ScatterPoint> b = PlanObjectScatter(MakeScatter(80.0F, 18, 2U));
    ASSERT_EQ(a.size(), b.size());
    bool differs = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differs = differs || (a[i].x != b[i].x) || (a[i].z != b[i].z);
    }
    EXPECT_TRUE(differs) << "换 seed 必须换一套布局";
}

TEST(ObjectScatter, PointsNeverOverlapEvenInADenseDisc) {
    // 密排（半径小、个数多）⇒ 会触发"加密重试"；仍然**任意两点不重合**，且间距有下界。
    const ObjectScatter             scatter = MakeScatter(6.0F, 40, 4242U);
    const std::vector<ScatterPoint> points  = PlanObjectScatter(scatter);
    ASSERT_EQ(points.size(), 40U);

    double minimum = 1.0e9;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1U; j < points.size(); ++j) {
            minimum = std::min(minimum, Distance(points[i], points[j]));
        }
    }
    EXPECT_GT(minimum, 0.0) << "不得有点重合 / 堆叠";
}

TEST(ObjectScatter, SparseDiscKeepsAGenerousMinimumSpacing) {
    // 稀疏（半径大、个数少）⇒ 抖动网格的步长足够大：最小间距应与"面积/个数"同量级（≥ 1 格，实测远大于它）。
    const ObjectScatter             scatter = MakeScatter(80.0F, 18, 1001U);
    const std::vector<ScatterPoint> points  = PlanObjectScatter(scatter);
    ASSERT_EQ(points.size(), 18U);

    double minimum = 1.0e9;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1U; j < points.size(); ++j) {
            minimum = std::min(minimum, Distance(points[i], points[j]));
        }
    }
    EXPECT_GT(minimum, 1.0) << "稀疏散布不应出现两点贴在一起（有界抖动保证最小间距 ≈ 0.5 × 网格步长）";
}

TEST(ObjectScatter, OffCenterDiscOffsetsEveryPoint) {
    ObjectScatter scatter = MakeScatter(30.0F, 10, 5U);
    scatter.centerX       = 1000.0F;
    scatter.centerZ       = -250.0F;
    const std::vector<ScatterPoint> points = PlanObjectScatter(scatter);
    ASSERT_EQ(points.size(), 10U);
    for (const ScatterPoint& point : points) {
        const double dx = static_cast<double>(point.x) - 1000.0;
        const double dz = static_cast<double>(point.z) + 250.0;
        EXPECT_LE(std::sqrt(dx * dx + dz * dz), 30.0 + 1.0e-3);
    }
}

TEST(ObjectScatter, InvalidParametersYieldNoPoints) {
    EXPECT_TRUE(PlanObjectScatter(MakeScatter(0.0F, 10, 1U)).empty());   // 半径 0
    EXPECT_TRUE(PlanObjectScatter(MakeScatter(-5.0F, 10, 1U)).empty());  // 负半径
    EXPECT_TRUE(PlanObjectScatter(MakeScatter(10.0F, 0, 1U)).empty());   // 个数 0
    EXPECT_TRUE(PlanObjectScatter(MakeScatter(10.0F, -3, 1U)).empty());  // 负个数
}

}  // namespace
