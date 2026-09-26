#pragma once

#include <cstdint>

namespace vx {

/// SplitMix64：由全局 64-bit 种子派生子系统种子（方案 §3.2、ADR 0006）。
///
/// 意图：让噪声层、材质抖动等子系统使用**互不相关**的种子，避免它们彼此耦合，
/// 同时保持「给定种子 + 给定坐标 → 给定结果」的纯函数性质（red line 7）。
/// 前置条件：无。纯函数，可从任意线程并发调用。
[[nodiscard]] constexpr std::uint64_t SplitMix64(std::uint64_t value) noexcept {
    std::uint64_t z = value + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// 从全局种子派生某个「通道」的 32-bit 种子，供 FastNoiseLite 使用。
///
/// `channel` 是通道编号（例如 1 = 基础高度、2 = 细节、3 = 粗糙、4 = 材质抖动），
/// 不同通道得到互不相关的种子。返回值为**非负** int32，因为 FastNoiseLite 的 `SetSeed` 只取 int。
[[nodiscard]] constexpr std::int32_t DeriveChannelSeed(std::uint64_t worldSeed, std::uint64_t channel) noexcept {
    const std::uint64_t mixed = SplitMix64(worldSeed ^ SplitMix64(channel));
    return static_cast<std::int32_t>(mixed & 0x7FFFFFFFULL);
}

}  // namespace vx
