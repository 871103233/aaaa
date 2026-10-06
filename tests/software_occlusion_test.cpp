// P3（[ADR 0031](../../docs/adr/0031-occlusion-culling-software.md)）：CPU 软件遮挡的纯函数单测。
//
// 契约（保守性优先，误剔 = 缺陷）：
//   ① 只有"**全部**条件成立"才判被遮挡 —— 任一角点出屏、任一格无遮挡面、覆盖过大、深度未超偏置 ⇒ **不剔**；
//   ② 每格取**最远**遮挡面深度（max 聚合）⇒ 判定偏保守；
//   ③ 纯函数、固定顺序 ⇒ **确定性**（不读全局、不分配热路径）。

#include "render/software_occlusion.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>

namespace {

using vx::IsAabbOccluded;
using vx::kNoOccluderDepth;
using vx::OcclusionDepthGrid;
using vx::ResetOcclusionDepthGrid;
using vx::SplatOccluderPoint;

/// 本项目的投影约定：右手系 + **0~1 深度**（与 `camera.cpp` / SDL_gpu 一致）。
[[nodiscard]] glm::mat4 TestViewProjection() {
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    const glm::mat4 view       = glm::lookAt(glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                                             glm::vec3(0.0F, 1.0F, 0.0F));
    return projection * view;
}

/// 单点的 NDC 深度（测试内自用）。
[[nodiscard]] float NdcDepth(const glm::mat4& viewProjection, const glm::vec3& point) {
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0F);
    return clip.z / clip.w;
}

/// 把整张图填成同一个遮挡面深度（用于构造"全覆盖"场景）。
void FillGrid(OcclusionDepthGrid& grid, float depth) {
    for (float& value : grid.depth) {
        value = depth;
    }
}

}  // namespace

TEST(SoftwareOcclusion, ResetClearsAndResizes) {
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 4, 3);
    EXPECT_EQ(grid.width, 4);
    EXPECT_EQ(grid.height, 3);
    ASSERT_EQ(grid.depth.size(), 12U);
    for (const float value : grid.depth) {
        EXPECT_FLOAT_EQ(value, kNoOccluderDepth);
    }

    // 写脏后重置 ⇒ 必须再次全清（不保留上一帧的遮挡面）。
    grid.depth[0] = 0.5F;
    ResetOcclusionDepthGrid(grid, 4, 3);
    EXPECT_FLOAT_EQ(grid.depth[0], kNoOccluderDepth);

    // 尺寸变化 ⇒ 重新分配为新的格数。
    ResetOcclusionDepthGrid(grid, 2, 2);
    ASSERT_EQ(grid.depth.size(), 4U);
}

TEST(SoftwareOcclusion, SplatKeepsFarthestOccluderDepth) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 8, 8);

    // 远、近两点都落在中心格 ⇒ 该格必须保留**更远**者（max 聚合，保守）。
    const glm::vec3 farPoint(0.0F, 0.0F, -80.0F);
    const glm::vec3 nearPoint(0.0F, 0.0F, -20.0F);
    const float     farDepth  = NdcDepth(viewProjection, farPoint);
    const float     nearDepth = NdcDepth(viewProjection, nearPoint);
    ASSERT_GT(farDepth, nearDepth);

    SplatOccluderPoint(grid, viewProjection, farPoint);
    SplatOccluderPoint(grid, viewProjection, nearPoint);  // 更近 ⇒ 不得覆盖更远者
    const float afterBoth = grid.depth[4U * 8U + 4U];
    EXPECT_NEAR(afterBoth, farDepth, 1.0e-6F);

    // 再写一个更远的 ⇒ 必须更新。
    SplatOccluderPoint(grid, viewProjection, glm::vec3(0.0F, 0.0F, -95.0F));
    EXPECT_GT(grid.depth[4U * 8U + 4U], afterBoth);
}

TEST(SoftwareOcclusion, SplatOutsideNdcBoxIsNoOp) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 8, 8);

    SplatOccluderPoint(grid, viewProjection, glm::vec3(0.0F, 0.0F, 10.0F));     // 相机后方（w < 0）
    SplatOccluderPoint(grid, viewProjection, glm::vec3(1000.0F, 0.0F, -10.0F)); // 出屏（|x| > 1）
    SplatOccluderPoint(grid, viewProjection, glm::vec3(0.0F, 0.0F, -200.0F));   // 超远平面（z > 1）
    for (const float value : grid.depth) {
        EXPECT_FLOAT_EQ(value, kNoOccluderDepth);
    }
}

TEST(SoftwareOcclusion, AabbBehindFullyCoveringOccluderIsOccluded) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 16, 16);
    const float occluderDepth = NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -50.0F));
    FillGrid(grid, occluderDepth);

    // 完全在遮挡面**之后**（更远）⇒ 判被遮挡。
    const bool occluded = IsAabbOccluded(grid, viewProjection, glm::vec3(-1.0F, -1.0F, -81.0F),
                                         glm::vec3(1.0F, 1.0F, -79.0F), /*depthBias=*/0.0F,
                                         /*maxCoveredCells=*/16 * 16);
    EXPECT_TRUE(occluded);

    // 在遮挡面**之前**（更近）⇒ 不得剔。
    EXPECT_FALSE(IsAabbOccluded(grid, viewProjection, glm::vec3(-1.0F, -1.0F, -21.0F),
                                glm::vec3(1.0F, 1.0F, -19.0F), 0.0F, 16 * 16));
}

TEST(SoftwareOcclusion, AabbNotOccludedWhenAnyCoveredCellHasNoOccluder) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 16, 16);
    FillGrid(grid, NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -50.0F)));
    grid.depth[8U * 16U + 8U] = kNoOccluderDepth;  // 中心格没有遮挡面

    EXPECT_FALSE(IsAabbOccluded(grid, viewProjection, glm::vec3(-1.0F, -1.0F, -81.0F),
                                glm::vec3(1.0F, 1.0F, -79.0F), 0.0F, 16 * 16));
}

TEST(SoftwareOcclusion, AabbNotOccludedWhenCornerOutsideNdcBox) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 16, 16);
    FillGrid(grid, NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -50.0F)));

    // 明显偏出画面（部分不在 NDC 盒内）⇒ 保守不剔。
    EXPECT_FALSE(IsAabbOccluded(grid, viewProjection, glm::vec3(120.0F, -1.0F, -81.0F),
                                glm::vec3(125.0F, 1.0F, -79.0F), 0.0F, 16 * 16));
}

TEST(SoftwareOcclusion, AabbNotOccludedWhenCoverageExceedsLimit) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 16, 16);
    FillGrid(grid, NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -50.0F)));

    // 覆盖格数上限过小 ⇒ 按保守处理（近处大网格不做遮挡判定）。
    EXPECT_FALSE(IsAabbOccluded(grid, viewProjection, glm::vec3(-20.0F, -20.0F, -81.0F),
                                glm::vec3(20.0F, 20.0F, -79.0F), 0.0F, /*maxCoveredCells=*/4));
}

TEST(SoftwareOcclusion, DepthBiasKeepsNearbyGeometryVisible) {
    const glm::mat4 viewProjection = TestViewProjection();
    OcclusionDepthGrid grid;
    ResetOcclusionDepthGrid(grid, 16, 16);
    const float occluderDepth = NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -80.0F));
    FillGrid(grid, occluderDepth);

    // 候选**略微**在遮挡面之后（更远）：无偏置 ⇒ 判被遮挡。
    const glm::vec3 minimum(-0.5F, -0.5F, -82.0F);
    const glm::vec3 maximum(0.5F, 0.5F, -81.0F);
    EXPECT_TRUE(IsAabbOccluded(grid, viewProjection, minimum, maximum, /*depthBias=*/0.0F, 16 * 16));

    // 同一个候选，加一个**大于深度差**的偏置 ⇒ 不得剔（吸收采样 / 浮点误差）。
    const float difference = NdcDepth(viewProjection, glm::vec3(0.0F, 0.0F, -81.0F)) - occluderDepth;
    ASSERT_GT(difference, 0.0F);
    EXPECT_FALSE(IsAabbOccluded(grid, viewProjection, minimum, maximum, difference * 1.5F, 16 * 16));
}
