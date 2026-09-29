// T67 / ADR 0021：环境贴图口径的**纯函数**单测（不触碰 GPU、不读全局）。
//
// 覆盖：预过滤 mip ↔ 粗糙度映射（含端点与越界钳制）、mip 链字节数的精确求和、
//       float32 → float16 的舍入（正常数 / 次正规 / 溢出饱和 / ±0 / inf / NaN）。
//
// 为什么这些必须钉住：① 映射是"烘焙侧与运行期共用同一口径"的唯一来源
// （烘焙时 mip i 写入 uniform，运行时按 `lod = roughness × lodMax` 取级 —— 错一处高光就会整体偏移）；
// ② 显存记账是 ADR 0010 的强制义务；③ 半精度转换错了会让天空 / 环境反射整体偏色（且不可见地错）。

#include "render/environment.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using vx::EstimateTextureMipChainBytes;
using vx::HalfFromFloat;
using vx::kEnvironmentPrefilterLodMax;
using vx::kEnvironmentPrefilterMipCount;
using vx::PrefilterRoughnessForMip;

}  // namespace

// 级数必须 ≥ 2：否则 lodMax = 0，映射退化成"只有 roughness = 0 一档"（配置常量写错即在此暴露）。
TEST(Environment, PrefilterLevelCountIsConsistent) {
    EXPECT_GE(kEnvironmentPrefilterMipCount, 2U);
    EXPECT_EQ(kEnvironmentPrefilterLodMax, kEnvironmentPrefilterMipCount - 1U);
}

// mip i ⇒ roughness = i / (级数 − 1)：两端恰好是 0（镜面）与 1（完全粗糙）。
TEST(Environment, PrefilterRoughnessMapsEndpointsToZeroAndOne) {
    EXPECT_FLOAT_EQ(PrefilterRoughnessForMip(0), 0.0F);
    EXPECT_FLOAT_EQ(PrefilterRoughnessForMip(kEnvironmentPrefilterLodMax), 1.0F);
}

// 逐级单调递增且均匀（用级数通用的公式手算对照，避免把常量抄一遍）。
TEST(Environment, PrefilterRoughnessIsMonotonicAndUniform) {
    const float step = 1.0F / static_cast<float>(kEnvironmentPrefilterLodMax);
    for (std::uint32_t mip = 0; mip < kEnvironmentPrefilterMipCount; ++mip) {
        EXPECT_NEAR(PrefilterRoughnessForMip(mip), static_cast<float>(mip) * step, 1e-6F) << "mip=" << mip;
    }
}

// 越界 mip 钳制到端点（不崩、不产生 > 1 的粗糙度）。
TEST(Environment, PrefilterRoughnessClampsOutOfRangeMip) {
    EXPECT_FLOAT_EQ(PrefilterRoughnessForMip(kEnvironmentPrefilterMipCount), 1.0F);
    EXPECT_FLOAT_EQ(PrefilterRoughnessForMip(1000U), 1.0F);
}

// mip 链字节数 = 逐级面积之和 × 每像素字节（各级减半、下限 1）；单级时恰为 宽×高×bpp。
TEST(Environment, MipChainBytesSumsEveryLevel) {
    EXPECT_EQ(EstimateTextureMipChainBytes(2048, 1024, 1, 8), 2048ULL * 1024ULL * 8ULL);
    // 8×4 两级：8*4 + 4*2 = 40 像素 × 8 字节 = 320。
    EXPECT_EQ(EstimateTextureMipChainBytes(8, 4, 2, 8), 320ULL);
    // 2×2 三级：2*2 + 1*1 + 1*1 = 6 像素（下限钳到 1，不再减半）× 8 = 48。
    EXPECT_EQ(EstimateTextureMipChainBytes(2, 2, 3, 8), 48ULL);
}

// 提交配置的预过滤贴图（128×64、6 级、RGBA16F）与 BRDF LUT（256²）的字节数必须与 ADR 0021 的估算同量级。
TEST(Environment, CommittedSizesMatchAdrEstimate) {
    const std::uint64_t prefilter = EstimateTextureMipChainBytes(128, 64, kEnvironmentPrefilterMipCount, 8);
    // 128*64=8192 像素：各级合计 = 8192+2048+512+128+32+8 = 10920 像素 × 8 B = 87360。
    EXPECT_EQ(prefilter, 87360ULL);
    const std::uint64_t lut = EstimateTextureMipChainBytes(256, 256, 1, 8);
    EXPECT_EQ(lut, 256ULL * 256ULL * 8ULL);
}

// ---- 半精度转换 ----

// 常见值必须**逐位**等于 IEEE 754 的已知编码（不是"近似相等"）。
TEST(Environment, HalfFromFloatMatchesKnownBitPatterns) {
    EXPECT_EQ(HalfFromFloat(0.0F), 0x0000U);
    EXPECT_EQ(HalfFromFloat(-0.0F), 0x8000U);
    EXPECT_EQ(HalfFromFloat(1.0F), 0x3C00U);
    EXPECT_EQ(HalfFromFloat(-1.0F), 0xBC00U);
    EXPECT_EQ(HalfFromFloat(0.5F), 0x3800U);
    EXPECT_EQ(HalfFromFloat(1.5F), 0x3E00U);
    EXPECT_EQ(HalfFromFloat(2.0F), 0x4000U);
    EXPECT_EQ(HalfFromFloat(2048.0F), 0x6800U);
    EXPECT_EQ(HalfFromFloat(65504.0F), 0x7BFFU);  // 最大有限半精度
    EXPECT_EQ(HalfFromFloat(-65504.0F), 0xFBFFU);
}

// 溢出（|v| > 65504）**饱和**到最大有限值：天空高亮被截顶，而**不是** inf / NaN
// （inf 会让整片环境反射变白、NaN 会让像素变黑，两者都是"看起来坏了"的典型症状）。
TEST(Environment, HalfFromFloatSaturatesOnOverflow) {
    EXPECT_EQ(HalfFromFloat(1.0e6F), 0x7BFFU);
    EXPECT_EQ(HalfFromFloat(-1.0e6F), 0xFBFFU);
    EXPECT_EQ(HalfFromFloat(std::numeric_limits<float>::max()), 0x7BFFU);
}

// 极小值下溢为 ±0（HDRI 里常见），次正规区按就近舍入。
TEST(Environment, HalfFromFloatHandlesUnderflowAndSubnormals) {
    EXPECT_EQ(HalfFromFloat(1.0e-30F), 0x0000U);
    EXPECT_EQ(HalfFromFloat(-1.0e-30F), 0x8000U);
    // 最小正次正规 2^-24 = 5.9604645e-8 → 0x0001。
    EXPECT_EQ(HalfFromFloat(5.9604645e-8F), 0x0001U);
    // 最小正规格 2^-14 = 6.1035156e-5 → 0x0400。
    EXPECT_EQ(HalfFromFloat(6.1035156e-5F), 0x0400U);
}

// inf / NaN 保留符号位（NaN 保持为 NaN，不变成 inf 或 0）。
TEST(Environment, HalfFromFloatPreservesInfAndNan) {
    EXPECT_EQ(HalfFromFloat(std::numeric_limits<float>::infinity()), 0x7C00U);
    EXPECT_EQ(HalfFromFloat(-std::numeric_limits<float>::infinity()), 0xFC00U);
    const std::uint16_t nan = HalfFromFloat(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(nan & 0x7C00U, 0x7C00U) << "阶码必须全 1（仍是 inf / NaN 类）";
    EXPECT_NE(nan & 0x03FFU, 0U) << "尾数必须非零（否则变成 inf）";
}

// 舍入必须是**就近偶数**：1.0 与下一个半精度值的中点上取偶数尾数。
TEST(Environment, HalfFromFloatRoundsToNearestEven) {
    // 0x3C01 = 1 + 2^-10 = 1.0009765625。其中点 1.00048828125 恰好落在两个半精度值之间 ⇒ 取偶（0x3C00）。
    EXPECT_EQ(HalfFromFloat(1.00048828125F), 0x3C00U);
    // 略大于中点 ⇒ 进位到 0x3C01。
    EXPECT_EQ(HalfFromFloat(1.0004884F), 0x3C01U);
}
