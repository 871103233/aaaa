// T24：纹理显存估算（含 mip 链）的纯函数单测。
//
// 记账口径见 ADR 0010：渲染层必须对 GPU 纹理显存记账，其中纹理数组按"第 0 级 × 4/3"估算
// （mip 链各级面积之和的几何级数上限）。本用例把该估算钉死，防止记账公式被无意改动。

#include "render/mesh_renderer.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using vx::EstimateTextureArrayBytes;

}  // namespace

// 256×256 × 4 层 R8G8B8A8：第 0 级 = 256·256·4·4 = 1,048,576 字节；
// 含 mip 链 ≈ 1,048,576 × 4/3 = 1,398,101 字节（整数除法向下取整）。
TEST(RenderStats, MipChainEstimateFollowsFourThirdsRule) {
    EXPECT_EQ(EstimateTextureArrayBytes(256, 256, 4), 1398101ULL);
    // 单层 2×2：第 0 级 16 字节 → 16 × 4 / 3 = 21 字节。
    EXPECT_EQ(EstimateTextureArrayBytes(2, 2, 1), 21ULL);
    // 尺寸不变时层数越多记账越高。
    EXPECT_GT(EstimateTextureArrayBytes(128, 128, 2), EstimateTextureArrayBytes(128, 128, 1));
    // 零尺寸 / 零层 → 0（不出现异常取值）。
    EXPECT_EQ(EstimateTextureArrayBytes(0, 0, 0), 0ULL);
    EXPECT_EQ(EstimateTextureArrayBytes(0, 256, 4), 0ULL);
}
