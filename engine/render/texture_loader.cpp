#include "render/texture_loader.hpp"

#include <cstddef>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

// stb_image 是第三方单头库。本工程在 MSVC 侧是 /W4 + /WX、GCC/Clang 侧是 -Wall -Wextra -Werror，
// 直接编它会产生大量告警 ⇒ 只在**本翻译单元**内把 MSVC 告警关掉；GCC/Clang 侧由
// `engine/CMakeLists.txt` 把 stb 头目录设为 SYSTEM 包含目录（系统头的告警不报）来承担。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vx {
namespace {

/// 一次读入整个文件（不触碰 GPU，可从任意线程调用）。
///
/// 之所以自己读进内存、再把指针交给 stb_image，而不是让 stb 按路径 `fopen`：
/// ① `std::filesystem::path` 在 Windows 上的窄字符转换不保证 UTF-8，交给 `fopen` 会打不开
/// 非 ASCII 路径；② 先拿到完整字节才能在**解码前**做尺寸守卫（见 `RequireDimensionsWithinLimit`）。
[[nodiscard]] std::vector<stbi_uc> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error(path.string() + ": 无法打开图像文件（不存在或没有读取权限）");
    }

    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        throw std::runtime_error(path.string() + ": 图像文件为空");
    }

    std::vector<stbi_uc> bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error(path.string() + ": 图像文件读取不完整");
    }
    return bytes;
}

/// 尺寸守卫：用**文件头声明的**宽高在解码前判定，超过 `kMaxImageDimension` 直接拒绝。
void RequireDimensionsWithinLimit(const std::filesystem::path& path, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error(path.string() + ": 图像头声明的尺寸非法（宽或高 ≤ 0）");
    }
    if (static_cast<std::uint32_t>(width) > kMaxImageDimension ||
        static_cast<std::uint32_t>(height) > kMaxImageDimension) {
        throw std::runtime_error(path.string() + ": 图像尺寸 " + std::to_string(width) + "x" +
                                 std::to_string(height) + " 超出上限 " +
                                 std::to_string(kMaxImageDimension) + "（拒绝解码）");
    }
}

/// 读文件头拿尺寸（**不解码像素**）。返回 false 表示"不是可识别的图像"。
[[nodiscard]] bool ReadHeader(const std::vector<stbi_uc>& bytes, int& width, int& height) {
    int channels = 0;
    return stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) != 0;
}

[[nodiscard]] std::string DescribeDecodeFailure(const std::filesystem::path& path) {
    const char* reason = stbi_failure_reason();
    return path.string() + ": 图像解码失败（" +
           (reason != nullptr ? std::string(reason) : std::string("原因未知")) + "）";
}

}  // namespace

ImageRgba8 LoadImageRgba8(const std::filesystem::path& path) {
    const std::vector<stbi_uc> bytes = ReadFileBytes(path);

    int width  = 0;
    int height = 0;
    if (!ReadHeader(bytes, width, height)) {
        throw std::runtime_error(path.string() + ": 不是可识别的图像文件（读不出文件头）");
    }
    RequireDimensionsWithinLimit(path, width, height);

    int      decodedChannels = 0;
    stbi_uc* decoded         = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                                     &decodedChannels, 4);  // 强制 4 通道（缺 A 时补 255）
    if (decoded == nullptr) {
        throw std::runtime_error(DescribeDecodeFailure(path));
    }

    ImageRgba8 image;
    image.width  = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const std::size_t byteCount =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U;
    image.pixels.assign(decoded, decoded + byteCount);
    stbi_image_free(decoded);
    return image;
}

ImageRgb32f LoadImageHdr(const std::filesystem::path& path) {
    const std::vector<stbi_uc> bytes = ReadFileBytes(path);

    if (stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size())) == 0) {
        throw std::runtime_error(path.string() + ": 不是 HDR 图像（只接受 Radiance .hdr）");
    }

    int width  = 0;
    int height = 0;
    if (!ReadHeader(bytes, width, height)) {
        throw std::runtime_error(path.string() + ": HDR 文件头无法解析");
    }
    RequireDimensionsWithinLimit(path, width, height);

    int    decodedChannels = 0;
    float* decoded = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                            &decodedChannels, 3);
    if (decoded == nullptr) {
        throw std::runtime_error(DescribeDecodeFailure(path));
    }

    ImageRgb32f image;
    image.width  = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const std::size_t floatCount =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3U;
    image.pixels.assign(decoded, decoded + floatCount);
    stbi_image_free(decoded);
    return image;
}

}  // namespace vx
