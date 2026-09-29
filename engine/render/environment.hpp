#pragma once

#include <cstdint>

namespace vx {

/// 环境贴图（天空 + IBL 三件套）的**口径常量与纯函数**（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
///
/// 为什么单独成模块（原计划写作 `sky.*` / `ibl.*` 两个文件，实际合并为一个）：
/// 天空与三条烘焙 pass 共享**同一套**等距柱状约定、同一套尺寸与级数，且这些常量必须同时被
/// 渲染器（烘焙）与 `game/`（投影进光照 uniform 的启用位与 mip 级号）使用 ⇒ 放一处才不会各写一份。
///
/// **唯一事实来源**：烘焙尺寸、预过滤级数、粗糙度 → mip 的映射都只在这里定义。
/// 着色器**不**硬编码级数：它通过光照 uniform 的 `fogParams.w` 拿到（见 `lighting_table.hpp`）。

/// 漫反射 irradiance 贴图尺寸（等距柱状，ADR 0021 第二条）：漫反射是极低频信号，32×16 足够。
inline constexpr std::uint32_t kEnvironmentIrradianceWidth  = 32;
inline constexpr std::uint32_t kEnvironmentIrradianceHeight = 16;

/// 预过滤高光贴图的 mip 级数（ADR 0021 第三条）：mip `i` ⇒ `roughness = i / (级数 − 1)`。
inline constexpr std::uint32_t kEnvironmentPrefilterMipCount = 6;

/// 预过滤高光贴图第 0 级的尺寸（等距柱状，2:1）。
inline constexpr std::uint32_t kEnvironmentPrefilterBaseWidth  = 128;
inline constexpr std::uint32_t kEnvironmentPrefilterBaseHeight = 64;

/// BRDF LUT 边长（正方形，ADR 0021 第四条）。
inline constexpr std::uint32_t kEnvironmentBrdfLutSize = 256;

/// 预过滤贴图的**最大 mip 级号** = 级数 − 1：运行期 `textureLod` 的 lod 上限
/// （片元按 `lod = roughness × 本值` 选级，见 `mesh.frag` 的环境项）。
inline constexpr std::uint32_t kEnvironmentPrefilterLodMax = kEnvironmentPrefilterMipCount - 1;

/// 纯函数：预过滤贴图第 `mip` 级对应的粗糙度 = `mip / (级数 − 1)`（超出范围按端点钳制）。
///
/// 烘焙侧（每条 mip 的 uniform）与文档口径同源；不读全局、不分配，可直接单测。
[[nodiscard]] float PrefilterRoughnessForMip(std::uint32_t mip) noexcept;

/// 纯函数：**精确**的 mip 链字节数（`bytesPerPixel × Σ 各级面积`，各级尺寸逐级减半、下限 1）。
///
/// 用于显存记账（ADR 0010 的记账义务）：与 `EstimateTextureArrayBytes` 的 4/3 近似不同，
/// 这里逐级求和 —— 环境贴图级数少（≤ 7），精确求和更贴近实际分配。
[[nodiscard]] std::uint64_t EstimateTextureMipChainBytes(std::uint32_t width, std::uint32_t height,
                                                        std::uint32_t levels, std::uint64_t bytesPerPixel) noexcept;

/// 纯函数：IEEE 754 binary32 → binary16（**就近舍入到偶数**）。
///
/// 用途：HDRI（`.hdr` 解出的 float32 RGB）要上传成 `R16G16B16A16_FLOAT` 纹理 —— 半精度
/// 对辐照度足够（动态范围 ±65504、相对精度 ~1e-3），而显存只有 float32 的一半
/// （2048×1024 由 33.5 MB 降到 16.8 MB，直接决定是否守得住 ADR 0008 的 300 MB）。
/// 溢出（|v| > 65504）按符号钳到 ±inf，再落为 ±65504 —— 天空高亮被饱和而非变成 NaN。
[[nodiscard]] std::uint16_t HalfFromFloat(float value) noexcept;

}  // namespace vx
