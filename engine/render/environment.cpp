#include "render/environment.hpp"

#include <algorithm>
#include <cstring>

namespace vx {

float PrefilterRoughnessForMip(std::uint32_t mip) noexcept {
    const std::uint32_t clamped = std::min(mip, kEnvironmentPrefilterLodMax);
    return static_cast<float>(clamped) / static_cast<float>(kEnvironmentPrefilterLodMax);
}

std::uint64_t EstimateTextureMipChainBytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels,
                                          std::uint64_t bytesPerPixel) noexcept {
    std::uint64_t total = 0;
    std::uint32_t w     = width;
    std::uint32_t h     = height;
    for (std::uint32_t level = 0; level < levels; ++level) {
        total += bytesPerPixel * static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
        w = std::max(1U, w >> 1U);
        h = std::max(1U, h >> 1U);
    }
    return total;
}

std::uint16_t HalfFromFloat(float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const std::uint32_t sign         = (bits >> 16U) & 0x8000U;
    const std::uint32_t exponentBits = (bits >> 23U) & 0xFFU;
    const std::uint32_t mantissaBits = bits & 0x007FFFFFU;

    if (exponentBits == 0xFFU) {  // inf / NaN：保留符号，NaN 保留一句静默位
        return static_cast<std::uint16_t>(sign | 0x7C00U | ((mantissaBits != 0U) ? 0x0200U : 0U));
    }

    const std::int32_t exponent = static_cast<std::int32_t>(exponentBits) - 127 + 15;

    if (exponent >= 0x1F) {  // 溢出（|v| > 65504）：按符号饱和到最大有限值，不产生 inf / NaN
        return static_cast<std::uint16_t>(sign | 0x7BFFU);
    }

    if (exponent <= 0) {  // 次正规数（或下溢为 ±0）
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        const std::uint32_t mantissa    = mantissaBits | 0x00800000U;  // 补回隐含的前导 1
        const std::uint32_t shift       = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t       half        = mantissa >> shift;
        const std::uint32_t remainder   = mantissa & ((1U << shift) - 1U);
        const std::uint32_t halfway     = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (half & 1U) != 0U)) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }

    std::uint32_t       half      = (static_cast<std::uint32_t>(exponent) << 10U) | (mantissaBits >> 13U);
    const std::uint32_t remainder = mantissaBits & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U)) {
        ++half;  // 进位可能把阶码推到 0x1F —— 下面的钳制负责饱和
    }
    if (half >= 0x7C00U) {
        return static_cast<std::uint16_t>(sign | 0x7BFFU);
    }
    return static_cast<std::uint16_t>(sign | half);
}

}  // namespace vx
