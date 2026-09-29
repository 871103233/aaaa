#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vx {

/// 单张图像任一边的像素上限。文件头里声明的宽 / 高超过它即**拒绝加载**（在**解码前**判定）。
///
/// 用途是挡住"解压炸弹"（几 KB 的文件解出几 GB 像素）与显存失控。4096 对当前阶段要用的
/// 1K / 2K PBR 贴图与 HDRI 有富余；需要更大时改这里（唯一旋钮）。
inline constexpr std::uint32_t kMaxImageDimension = 4096;

/// LDR 图像像素：**RGBA8**、行主序、**行间无填充**、**从上到下**（第 0 行在图像顶部）。
struct ImageRgba8 {
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> pixels;  ///< 长度恒为 `width * height * 4`（A 由 stb 补为 255）
};

/// HDR 图像像素：**线性光**、RGB 三通道、float32、行主序、行间无填充、从上到下。
struct ImageRgb32f {
    std::uint32_t      width  = 0;
    std::uint32_t      height = 0;
    std::vector<float> pixels;  ///< 长度恒为 `width * height * 3`
};

/// 从文件载入 LDR 图像并统一转成 RGBA8（格式由 stb_image 支持：PNG / JPG / BMP / TGA / PSD / …）。
///
/// 语义：**纯函数** —— 同一份文件字节 ⇒ 逐字节相同的输出（红线 7）。
/// 失败一律抛 `std::runtime_error` 并说明原因（打不开 / 读不出文件头 / 解码失败 / 超出
/// `kMaxImageDimension`），**不静默回退**（与 ADR 0005 的配置加载口径一致）。
///
/// 线程约定：不触碰 GPU、不读全局状态，可从任意线程调用；**纹理上传必须留在渲染线程**
/// （SDL_gpu 的命令缓冲是单线程的，见 SKILL「所有重活都必须离开渲染帧」）。
[[nodiscard]] ImageRgba8 LoadImageRgba8(const std::filesystem::path& path);

/// 从文件载入 HDR 图像（只接受 Radiance `.hdr`）并转成**线性** float RGB。
///
/// 失败语义与线程约定同 `LoadImageRgba8`；喂进 LDR 文件（PNG / TGA / …）会显式报错。
[[nodiscard]] ImageRgb32f LoadImageHdr(const std::filesystem::path& path);

}  // namespace vx
