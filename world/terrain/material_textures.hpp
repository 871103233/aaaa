#pragma once

#include "terrain/material_table.hpp"

#include <cstdint>
#include <vector>

namespace vx {

/// 程序生成的占位材质贴图的默认每层边长（像素）。
///
/// 256 × 256 × R8G8B8A8 × 4 层 ≈ 1.0 MB/张（不含 mip）；albedo + 法线两张约 2.7 MB（含 mip）。
inline constexpr std::uint32_t kMaterialTextureSize = 256;

/// 占位材质贴图集（ADR 0009）：4 层 albedo + 4 层法线，均为 `R8G8B8A8_UNORM`、layer-major 连续。
///
/// - **albedo**：由已有噪声（FastNoiseLite，通道种子由全局种子派生）生成的**单色细节**；
///   实际层色由 `assets/config/materials.toml` 的 `tint_*` 决定（着色器里 `albedo * tint`），
///   因此"颜色"只有一个来源，贴图只负责细节与颗粒。
/// - **法线**：由一层高度噪声的梯度生成（`n = normalize(-dH/du, -dH/dv, 1)`），
///   编码为 `[0,255]`；解码后单位长度（误差仅来自 8 位量化）。
///
/// 确定性：同一 `worldSeed` + 同一 `size` ⇒ **逐字节相同**（红线 7；不依赖时间 / 线程顺序）。
/// 无缝：亮度 / 高度噪声在 tile 上做周期混合，重复平铺不出现硬接缝。
struct MaterialTextureSet {
    std::uint32_t             size       = kMaterialTextureSize;               ///< 每层边长（像素）
    std::uint32_t             layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);
    std::vector<std::uint8_t> albedoRgba;  ///< layer-major：层 L 的像素从 `L * size * size * 4` 开始
    std::vector<std::uint8_t> normalRgba;  ///< 同上；xyz 存于 rgb，a 恒为 255
};

/// 生成占位材质贴图集（纯函数、确定性、无二进制资产）。
///
/// 前置条件：`size ≥ 4`（法线用中心差分，需要邻域）。
/// 返回的 `albedoRgba.size() == normalRgba.size() == layerCount * size * size * 4`。
[[nodiscard]] MaterialTextureSet GenerateMaterialTextures(std::uint64_t worldSeed,
                                                          std::uint32_t   size = kMaterialTextureSize);

}  // namespace vx
