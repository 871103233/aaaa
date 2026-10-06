// 坐标拾取辅助（V0.5 E2）单测：纯函数 `RaycastGround` 的命中 / 未命中 / 无地表数据 / 非法入参。
// 见 docs/plans/v0.5.md §1.18。

#include "ground_pick.hpp"

#include <functional>
#include <optional>

#include <gtest/gtest.h>

namespace {

using vx::GroundPick;
using vx::RaycastGround;

/// 无限平坦地面（高度恒为 `height`）。
[[nodiscard]] std::function<bool(float, float, float&)> FlatGround(float height) {
    return [height](float, float, float& outHeight) {
        outHeight = height;
        return true;
    };
}

TEST(GroundPick, HitsFlatGroundStraightDown) {
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 100.0F, 0.5F, FlatGround(0.0F));
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->x, 0.0F, 1e-4F);
    EXPECT_NEAR(hit->z, 0.0F, 1e-4F);
    EXPECT_NEAR(hit->surfaceY, 0.0F, 1e-4F);
    EXPECT_NEAR(hit->distance, 10.0F, 0.5F);
}

TEST(GroundPick, HitsAtExpectedColumnOnObliqueRay) {
    // 45° 斜向下：y 由 10 降到 0 时，水平正好前进 10 格（见方向归一化）。
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, 10.0F, 0.0F, 1.0F, -1.0F, 0.0F, 100.0F, 0.25F, FlatGround(0.0F));
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->x, 10.0F, 0.5F);
    EXPECT_NEAR(hit->z, 0.0F, 1e-4F);
}

TEST(GroundPick, NoHitWhenPointingUp) {
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, 1.0F, 0.0F, 100.0F, 0.5F, FlatGround(0.0F));
    EXPECT_FALSE(hit.has_value());
}

TEST(GroundPick, NoHitWhenNoTerrainData) {
    const auto heightAt = [](float, float, float&) { return false; };
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 100.0F, 0.5F, heightAt);
    EXPECT_FALSE(hit.has_value());
}

TEST(GroundPick, ImmediateHitWhenStartingBelowSurface) {
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, -5.0F, 0.0F, 0.0F, -1.0F, 0.0F, 100.0F, 0.5F, FlatGround(0.0F));
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 0.0F, 1e-4F);
}

TEST(GroundPick, SkipsColumnsWithoutTerrainData) {
    // 无数据的"洞"正好覆盖解析命中点（x≈10）：命中应被推迟到洞之后的第一列（x > 10.5）。
    const auto heightAt = [](float x, float, float& outHeight) {
        if (x > 9.5F && x < 10.5F) {
            return false;  // 该段无地表数据
        }
        outHeight = 0.0F;
        return true;
    };
    const std::optional<GroundPick> hit =
        RaycastGround(0.0F, 10.0F, 0.0F, 1.0F, -1.0F, 0.0F, 100.0F, 0.25F, heightAt);
    ASSERT_TRUE(hit.has_value());
    EXPECT_GT(hit->x, 10.5F);
}

TEST(GroundPick, InvalidInputsReturnNoHit) {
    const auto ground = FlatGround(0.0F);
    EXPECT_FALSE(RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.5F, ground).has_value());
    EXPECT_FALSE(RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 100.0F, 0.0F, ground).has_value());
    EXPECT_FALSE(RaycastGround(0.0F, 10.0F, 0.0F, 0.0F, 0.0F, 0.0F, 100.0F, 0.5F, ground).has_value());
}

}  // namespace
