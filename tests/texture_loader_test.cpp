// T57：纹理资源加载能力（`engine/render/texture_loader.*`）的单元测试。
//
// 口径依据：ADR 0005（资产 / 配置加载失败一律报错、不静默回退）、
//           docs/plans/v0.1.md 的 T57（本批只交付"能力"，美术资源接入留 ⓒ）。
//
// 夹具**不引入任何二进制资产**（ADR 0009 的口径：仓库不放占位二进制图）：测试自行在临时目录里
// 手工拼一张**未压缩 24 位 TGA**（18 字节头 + BGR 像素），覆盖"能读 / 像素正确 / 确定性"；
// 其余用例只需畸形字节即可。
//
// 覆盖：TGA → RGBA8 且通道顺序正确（BGR 还原为 RGB、A 补 255）/ 同一文件两次载入逐字节一致 /
//       文件不存在抛 / 空文件抛 / 非图像内容抛 / 文件头声明超上限**在解码前**抛 /
//       把 LDR 文件喂给 HDR 载入器抛。

#include "render/texture_loader.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using vx::ImageRgba8;
using vx::LoadImageHdr;
using vx::LoadImageRgba8;

/// 临时目录下的一次性文件；析构时尽力删除（删不掉也不让测试失败）。
class TempFile final {
public:
    explicit TempFile(const std::string& fileName)
        : m_path(std::filesystem::temp_directory_path() / fileName) {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return stream.good();
}

/// 未压缩真彩 TGA 的像素：TGA 按 **BGR** 存。
struct TgaPixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

/// 拼一张**从上到下**的未压缩 24 位 TGA（`imageDescriptor = 0x20` 表示左上角为原点）。
/// 只用于单测夹具 —— 生产路径只**读**图、不写图。
[[nodiscard]] std::vector<std::uint8_t> BuildTga(std::uint16_t width, std::uint16_t height,
                                                 const std::vector<TgaPixel>& pixelsTopDown) {
    std::vector<std::uint8_t> bytes(18, 0);
    bytes[2]  = 2;  // imageType：未压缩真彩
    bytes[12] = static_cast<std::uint8_t>(width & 0xFFU);
    bytes[13] = static_cast<std::uint8_t>((width >> 8U) & 0xFFU);
    bytes[14] = static_cast<std::uint8_t>(height & 0xFFU);
    bytes[15] = static_cast<std::uint8_t>((height >> 8U) & 0xFFU);
    bytes[16] = 24;    // pixelDepth
    bytes[17] = 0x20;  // imageDescriptor：左上角为原点

    for (const TgaPixel& pixel : pixelsTopDown) {
        bytes.push_back(pixel.b);
        bytes.push_back(pixel.g);
        bytes.push_back(pixel.r);
    }
    return bytes;
}

/// 读第 `index` 个字节的通道值（统一成 `unsigned`，避免 8 位与 32 位比较触发符号性告警）。
[[nodiscard]] unsigned Channel(const ImageRgba8& image, std::size_t index) {
    return static_cast<unsigned>(image.pixels[index]);
}

TEST(TextureLoader, LoadsUncompressedTgaAsRgba8WithCorrectChannelOrder) {
    const TempFile file("voxel_texture_loader_ok.tga");

    // 2x2：红 / 绿（上排）、蓝 / 白（下排）；TGA 里按 BGR 存，读出来必须是 RGB。
    const std::vector<TgaPixel> pixels {
        { 0, 0, 255 },      // 红
        { 0, 255, 0 },      // 绿
        { 255, 0, 0 },      // 蓝
        { 255, 255, 255 },  // 白
    };
    ASSERT_TRUE(WriteBytes(file.Path(), BuildTga(2, 2, pixels)));

    const ImageRgba8 image = LoadImageRgba8(file.Path());

    ASSERT_EQ(image.width, 2U);
    ASSERT_EQ(image.height, 2U);
    ASSERT_EQ(image.pixels.size(), std::size_t { 2 } * 2U * 4U);

    EXPECT_EQ(Channel(image, 0), 255U);  // 红 R
    EXPECT_EQ(Channel(image, 1), 0U);    // 红 G
    EXPECT_EQ(Channel(image, 2), 0U);    // 红 B
    EXPECT_EQ(Channel(image, 3), 255U);  // 红 A（源无 A 通道，由 stb 补不透明）

    EXPECT_EQ(Channel(image, 4), 0U);    // 绿 R
    EXPECT_EQ(Channel(image, 5), 255U);  // 绿 G
    EXPECT_EQ(Channel(image, 6), 0U);    // 绿 B

    EXPECT_EQ(Channel(image, 8), 0U);    // 蓝 R
    EXPECT_EQ(Channel(image, 9), 0U);    // 蓝 G
    EXPECT_EQ(Channel(image, 10), 255U); // 蓝 B

    EXPECT_EQ(Channel(image, 12), 255U);  // 白 R
    EXPECT_EQ(Channel(image, 13), 255U);  // 白 G
    EXPECT_EQ(Channel(image, 14), 255U);  // 白 B
}

TEST(TextureLoader, SameFileAlwaysDecodesToTheSameBytes) {
    const TempFile file("voxel_texture_loader_determinism.tga");

    std::vector<TgaPixel> pixels;
    for (std::uint16_t i = 0; i < 64U; ++i) {
        pixels.push_back(TgaPixel { static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i * 3U),
                                    static_cast<std::uint8_t>(255U - i) });
    }
    ASSERT_TRUE(WriteBytes(file.Path(), BuildTga(8, 8, pixels)));

    const ImageRgba8 first  = LoadImageRgba8(file.Path());
    const ImageRgba8 second = LoadImageRgba8(file.Path());

    EXPECT_EQ(first.width, second.width);
    EXPECT_EQ(first.height, second.height);
    EXPECT_EQ(first.pixels, second.pixels);
}

TEST(TextureLoader, ThrowsWhenFileDoesNotExist) {
    const TempFile file("voxel_texture_loader_absent.tga");  // 构造时即删除，故一定不存在

    EXPECT_THROW((void)LoadImageRgba8(file.Path()), std::runtime_error);
}

TEST(TextureLoader, ThrowsWhenFileIsEmpty) {
    const TempFile file("voxel_texture_loader_empty.tga");
    ASSERT_TRUE(WriteBytes(file.Path(), {}));

    EXPECT_THROW((void)LoadImageRgba8(file.Path()), std::runtime_error);
}

TEST(TextureLoader, ThrowsWhenContentIsNotAnImage) {
    const TempFile file("voxel_texture_loader_garbage.tga");
    const std::string garbage = "这不是图像，只是一段文本。";
    ASSERT_TRUE(WriteBytes(file.Path(), std::vector<std::uint8_t>(garbage.begin(), garbage.end())));

    EXPECT_THROW((void)LoadImageRgba8(file.Path()), std::runtime_error);
}

/// 关键用例：**只给文件头、不给像素数据**，且头里声明 8192×8192。
/// 加载器必须在**解码前**就凭头里的尺寸拒绝 —— 若只在解码后检查，这条会变成"解码失败"而非"超上限"。
TEST(TextureLoader, ThrowsWhenHeaderDeclaresOversizedImageBeforeDecoding) {
    const TempFile file("voxel_texture_loader_oversized.tga");
    ASSERT_TRUE(WriteBytes(file.Path(), BuildTga(8192, 8192, {})));

    try {
        (void)LoadImageRgba8(file.Path());
        FAIL() << "声明 8192x8192 的图像必须被拒绝";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("超出上限"), std::string::npos);
    }
}

TEST(TextureLoader, HdrLoaderRejectsLdrFile) {
    const TempFile file("voxel_texture_loader_ldr_for_hdr.tga");
    ASSERT_TRUE(WriteBytes(file.Path(), BuildTga(2, 2, { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } })));

    EXPECT_THROW((void)LoadImageHdr(file.Path()), std::runtime_error);
}

}  // namespace
