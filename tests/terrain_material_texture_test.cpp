// T22 / ADR 0010 P2 的占位材质贴图回归（在 T19 的基础上扩展）：
//   - 生成必须是**确定性**的（同种子 ⇒ 逐字节相同，红线 7）；
//   - 不同种子必须产生不同字节（种子真的参与生成）；
//   - 数据尺寸与取值范围"san值"：五张图等长结构正确、albedo 有变化、法线朝外；
//   - 生成的法线解码后必须是**单位长度**（误差仅来自 8 位量化）；
//   - 新增 roughness / AO / macro 三张图：尺寸、层数、确定性、值域与非恒定；
//   - albedo 的**多尺度叠加**必须带来显著的高频能量（"细腻"的可测代理）。

#include "terrain/material_textures.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using vx::GenerateMaterialTextures;
using vx::kMaterialMacroLayerCount;
using vx::kMaterialSlotCount;
using vx::MaterialTextureBuilder;
using vx::MaterialTextureSet;

constexpr std::uint64_t kSeed     = 0x5EED0019ULL;
constexpr std::uint32_t kTestSize = 32;

/// 取出某层 R 通道为 `size × size` 的浮点图（用于高频统计）。
[[nodiscard]] std::vector<double> ExtractRed(const std::vector<std::uint8_t>& rgba, std::uint32_t size,
                                             std::size_t layerOffset) {
    std::vector<double> out(static_cast<std::size_t>(size) * size, 0.0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<double>(rgba[(layerOffset + i) * 4U]);
    }
    return out;
}

/// 高频能量代理：相邻像素一阶差分的均方。单一低频结构时很小，叠加高频"颗粒"后显著变大。
[[nodiscard]] double HighFrequencyEnergy(const std::vector<double>& image, std::uint32_t size) {
    double      sum   = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x + 1U < size; ++x) {
            const double delta = image[static_cast<std::size_t>(y) * size + x + 1U] -
                                 image[static_cast<std::size_t>(y) * size + x];
            sum += delta * delta;
            ++count;
        }
    }
    return (count > 0) ? sum / static_cast<double>(count) : 0.0;
}

/// 3×3 盒式低通（边界用 clamp 延拓，避免边缘伪造出大的相邻差分）：
/// 近似去掉高频段，作为"叠加前（纯低频结构）"的高频能量对照。
[[nodiscard]] std::vector<double> BoxBlur3x3(const std::vector<double>& image, std::uint32_t size) {
    const auto clampIndex = [size](int v) {
        if (v < 0) {
            return std::size_t { 0 };
        }
        if (v >= static_cast<int>(size)) {
            return static_cast<std::size_t>(size) - 1U;
        }
        return static_cast<std::size_t>(v);
    };
    std::vector<double> out(image.size(), 0.0);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            double sum = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const std::size_t sx = clampIndex(static_cast<int>(x) + dx);
                    const std::size_t sy = clampIndex(static_cast<int>(y) + dy);
                    sum += image[sy * size + sx];
                }
            }
            out[static_cast<std::size_t>(y) * size + x] = sum / 9.0;
        }
    }
    return out;
}

/// 某层某通道是否非恒定（至少有一个像素与该层首像素不同）。
[[nodiscard]] bool LayerChannelVaries(const std::vector<std::uint8_t>& rgba, std::size_t layerBytes,
                                      std::size_t layerIndex, std::size_t channel) {
    const std::size_t begin = layerIndex * layerBytes;
    const std::uint8_t first = rgba[begin + channel];
    for (std::size_t i = begin + channel; i < begin + layerBytes; i += 4U) {
        if (rgba[i] != first) {
            return true;
        }
    }
    return false;
}

}  // namespace

// 同一 (种子, 尺寸) ⇒ 逐字节相同。
TEST(TerrainMaterialTexture, GenerationIsDeterministic) {
    const MaterialTextureSet first  = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet second = GenerateMaterialTextures(kSeed, kTestSize);

    EXPECT_EQ(first.albedoRgba, second.albedoRgba);
    EXPECT_EQ(first.normalRgba, second.normalRgba);
}

// 不同种子 ⇒ 不同字节。
TEST(TerrainMaterialTexture, DifferentSeedChangesBytes) {
    const MaterialTextureSet a = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet b = GenerateMaterialTextures(kSeed + 1U, kTestSize);

    EXPECT_NE(a.albedoRgba, b.albedoRgba);
    EXPECT_NE(a.normalRgba, b.normalRgba);
}

// 尺寸 / 层数 / 取值范围合理。
TEST(TerrainMaterialTexture, SizesAndRangesAreSane) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;
    const std::size_t expected   = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);

    EXPECT_EQ(set.size, kTestSize);
    EXPECT_EQ(set.layerCount, static_cast<std::uint32_t>(kMaterialSlotCount));
    EXPECT_EQ(set.albedoRgba.size(), expected);
    EXPECT_EQ(set.normalRgba.size(), expected);

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        const std::size_t begin = static_cast<std::size_t>(layer) * layerBytes;
        const std::size_t end   = begin + layerBytes;

        // albedo：每层都要有噪声变化，不能是纯色。
        const std::uint8_t firstRed = set.albedoRgba[begin];
        bool               varies   = false;
        for (std::size_t i = begin; i < end; i += 4U) {
            if (set.albedoRgba[i] != firstRed) {
                varies = true;
                break;
            }
        }
        EXPECT_TRUE(varies) << "layer=" << layer << " 的 albedo 不应是纯色";
    }
}

// 法线：解码后单位长度，且 z 分量朝外（> 0）。
TEST(TerrainMaterialTexture, GeneratedNormalsAreUnitLength) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    for (std::size_t i = 0; i + 3U < set.normalRgba.size(); i += 4U) {
        const float nx = static_cast<float>(set.normalRgba[i + 0U]) / 255.0F * 2.0F - 1.0F;
        const float ny = static_cast<float>(set.normalRgba[i + 1U]) / 255.0F * 2.0F - 1.0F;
        const float nz = static_cast<float>(set.normalRgba[i + 2U]) / 255.0F * 2.0F - 1.0F;

        EXPECT_NEAR(std::sqrt(nx * nx + ny * ny + nz * nz), 1.0F, 0.02F);
        EXPECT_GT(nz, 0.0F);
    }
}

// T22：roughness / AO / macro 三张新图的尺寸与层数。
TEST(TerrainMaterialTexture, NewArraysHaveExpectedSizeAndLayerCount) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;
    const std::size_t expected4   = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);
    const std::size_t expected1   = layerBytes * static_cast<std::size_t>(kMaterialMacroLayerCount);

    EXPECT_EQ(set.roughnessRgba.size(), expected4);
    EXPECT_EQ(set.aoRgba.size(), expected4);
    EXPECT_EQ(set.macroRgba.size(), expected1);
    EXPECT_EQ(kMaterialMacroLayerCount, 1U) << "宏观变化图必须是单层";
}

// T22：roughness / AO / macro 的确定性（同种子 ⇒ 逐字节相同；红线 7）。
TEST(TerrainMaterialTexture, NewArraysAreDeterministic) {
    const MaterialTextureSet first  = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet second = GenerateMaterialTextures(kSeed, kTestSize);

    EXPECT_EQ(first.roughnessRgba, second.roughnessRgba);
    EXPECT_EQ(first.aoRgba, second.aoRgba);
    EXPECT_EQ(first.macroRgba, second.macroRgba);
}

// T22：不同种子必须改变三张新图的字节（种子确实参与各通道派生）。
TEST(TerrainMaterialTexture, NewArraysDifferBySeed) {
    const MaterialTextureSet a = GenerateMaterialTextures(kSeed, kTestSize);
    const MaterialTextureSet b = GenerateMaterialTextures(kSeed + 7U, kTestSize);

    EXPECT_NE(a.roughnessRgba, b.roughnessRgba);
    EXPECT_NE(a.aoRgba, b.aoRgba);
    EXPECT_NE(a.macroRgba, b.macroRgba);
}

// T22：三张新图必须是"有细节"的灰阶图（RGB 三通道相同、A = 255；R 通道非恒定）。
TEST(TerrainMaterialTexture, NewArraysAreGrayAndNonConstant) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);
    const std::size_t layerBytes = static_cast<std::size_t>(kTestSize) * kTestSize * 4U;

    struct ArrayView {
        const std::vector<std::uint8_t>& rgba;
        std::size_t                      layerCount;
    };
    const ArrayView views[] = { { set.roughnessRgba, static_cast<std::size_t>(kMaterialSlotCount) },
                                { set.aoRgba, static_cast<std::size_t>(kMaterialSlotCount) },
                                { set.macroRgba, static_cast<std::size_t>(kMaterialMacroLayerCount) } };

    for (const ArrayView& view : views) {
        for (std::size_t layer = 0; layer < view.layerCount; ++layer) {
            for (std::size_t i = layer * layerBytes; i < (layer + 1U) * layerBytes; i += 4U) {
                EXPECT_EQ(view.rgba[i + 0U], view.rgba[i + 1U]);  // 灰阶：rgb 同值
                EXPECT_EQ(view.rgba[i + 1U], view.rgba[i + 2U]);
                EXPECT_EQ(view.rgba[i + 3U], 255U);               // a 恒为 255
            }
            EXPECT_TRUE(LayerChannelVaries(view.rgba, layerBytes, layer, 0U)) << "layer=" << layer << " 不应恒定";
        }
    }
}

// T22：AO 细节值域必须落在 [0.6, 1.0]（1 = 完全开阔），避免把环境项压黑到 0。
TEST(TerrainMaterialTexture, AoDetailStaysInConfiguredRange) {
    const MaterialTextureSet set = GenerateMaterialTextures(kSeed, kTestSize);

    for (std::size_t i = 0; i + 3U < set.aoRgba.size(); i += 4U) {
        const double value = static_cast<double>(set.aoRgba[i]) / 255.0;
        EXPECT_GE(value, 0.6 - 0.01);
        EXPECT_LE(value, 1.0 + 0.01);
    }
}

// T22 多尺度：albedo 由"低频结构 + 高频颗粒"两段叠加而成。用**相邻像素一阶差分的均方**作为
// 高频能量代理：叠加后必须显著大于低通近似（≈ 只保留低频结构，即"叠加前"）的高频能量。
//
// 为什么这份用例用 128² 而非 32²：噪声周期是"整张贴图内的特征数"，32² 下高频段（周期 30）也只有
// 约 1 个周期，逐像素差分根本体现不出"高频"，测不出频段差异；128² 下高频段约 4 个像素一个特征，
// 与低频段（约 13 个像素）在频谱上明显分开。
TEST(TerrainMaterialTexture, AlbedoMultiScaleAddsHighFrequencyEnergy) {
    constexpr std::uint32_t kMultiScaleSize = 128;
    const MaterialTextureSet set            = GenerateMaterialTextures(kSeed, kMultiScaleSize);
    const std::size_t        layerPixels    = static_cast<std::size_t>(kMultiScaleSize) * kMultiScaleSize;

    for (int layer = 0; layer < kMaterialSlotCount; ++layer) {
        const std::vector<double> red =
            ExtractRed(set.albedoRgba, kMultiScaleSize, static_cast<std::size_t>(layer) * layerPixels);
        const std::vector<double> lowOnly = BoxBlur3x3(red, kMultiScaleSize);

        const double highFrequency    = HighFrequencyEnergy(red, kMultiScaleSize);
        const double lowFrequencyBase = HighFrequencyEnergy(lowOnly, kMultiScaleSize);

        EXPECT_GT(highFrequency, 0.0) << "layer=" << layer << " albedo 必须有细节";
        EXPECT_GT(highFrequency, lowFrequencyBase * 2.0)
            << "layer=" << layer << " 多尺度叠加后的高频能量必须显著大于纯低频结构（叠加前）";
    }
}

// T36 / SKILL「不冻结画面」：**分步**生成必须与一次性生成**逐字节相同** —— 分帧只允许改变
// "何时可见"，不得改变结果（红线 7）。这条是启动加载"每帧只算几个像素行"的正确性凭据。
TEST(TerrainMaterialTexture, SteppedBuilderMatchesOneShotByteForByte) {
    const MaterialTextureSet oneShot = GenerateMaterialTextures(kSeed, kTestSize);

    MaterialTextureBuilder builder(kSeed, kTestSize);
    EXPECT_FALSE(builder.Done()) << "刚构造时不应视为已完成";

    // 每批只走 1 行（最细分帧）：进度必须单调不减，且未完成时严格小于 100%。
    float previous = builder.Progress();
    EXPECT_GE(previous, 0.0F);
    int steps = 0;
    while (!builder.Step(1)) {
        const float progress = builder.Progress();
        EXPECT_GE(progress, previous);
        EXPECT_LT(progress, 1.0F);
        previous = progress;
        ASSERT_LT(++steps, 100000) << "分步生成未在合理步数内结束";
    }
    EXPECT_TRUE(builder.Done());
    EXPECT_FLOAT_EQ(builder.Progress(), 1.0F);

    const MaterialTextureSet stepped = builder.Take();
    EXPECT_EQ(stepped.size, oneShot.size);
    EXPECT_EQ(stepped.layerCount, oneShot.layerCount);
    EXPECT_EQ(stepped.albedoRgba, oneShot.albedoRgba);
    EXPECT_EQ(stepped.normalRgba, oneShot.normalRgba);
    EXPECT_EQ(stepped.roughnessRgba, oneShot.roughnessRgba);
    EXPECT_EQ(stepped.aoRgba, oneShot.aoRgba);
    EXPECT_EQ(stepped.macroRgba, oneShot.macroRgba);
}

// ---------------------------------------------------------------------------
// T66 / V0.3 ⓒ：**真实 CC0 美术贴图**的解析 / 降采样 / 失败回落
//
// 说明：真实资源**不入库**（所有者 2026-09-29 裁定），故这里刻意**不依赖任何真实贴图文件** ——
// 只覆盖"纯函数 + 失败路径"（成功路径由启动冒烟日志给出证据：`真实美术贴图已加载` + 显存记账）。
// ---------------------------------------------------------------------------

namespace {

using vx::DownscaleBoxRgba8;
using vx::ImageRgba8;
using vx::MaterialTextureAssetLoader;
using vx::MaterialTextureAssetSpec;
using vx::ResolveMapFile;
using vx::TerrainMaterialTable;

/// 测试用临时目录（析构即清理）：避免污染仓库，也避免依赖 `assets/` 下不入库的资源。
class TempDirectory final {
public:
    explicit TempDirectory(const char* name)
        : m_path(std::filesystem::temp_directory_path() / (std::string("vx_t66_") + name)) {
        std::error_code code;
        std::filesystem::remove_all(m_path, code);
        std::filesystem::create_directories(m_path, code);
    }
    ~TempDirectory() {
        std::error_code code;
        std::filesystem::remove_all(m_path, code);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

/// 写一个**空**文件（只为验证"存在性 / 扩展名解析"，不参与解码）。
void TouchFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << 'x';
}

}  // namespace

// 扩展名解析：**固定顺序**（jpg → jpeg → png → tga → bmp），找不到返回空路径（⇒ 调用方回落，不崩）。
TEST(RealTextures, ResolveMapFilePrefersFixedExtensionOrderAndReportsMissing) {
    const TempDirectory directory("resolve");

    // 只有 png：找到它。
    TouchFile(directory.Path() / "albedo.png");
    EXPECT_EQ(ResolveMapFile(directory.Path(), "albedo").extension().string(), ".png");

    // 再加 jpg：**jpg 优先**（固定顺序 ⇒ 跨机器结果一致，红线 7）。
    TouchFile(directory.Path() / "albedo.jpg");
    EXPECT_EQ(ResolveMapFile(directory.Path(), "albedo").extension().string(), ".jpg");

    // 完全不存在 ⇒ 空路径。
    EXPECT_TRUE(ResolveMapFile(directory.Path(), "roughness").empty());
}

// 整数倍 box 降采样：逐块算术平均、确定性（同输入 ⇒ 逐字节相同）；**非整数倍 ⇒ 明确失败**（返回 nullopt）。
TEST(RealTextures, DownscaleBoxAveragesExactlyAndRejectsNonIntegerFactor) {
    ImageRgba8 source;
    source.width  = 4;
    source.height = 4;
    source.pixels.assign(4U * 4U * 4U, 0);
    // 4×4 的 R 通道按"每个 2×2 块 = 一个常量"填：块 (0,0)=10、(1,0)=20、(0,1)=30、(1,1)=40。
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint8_t value = static_cast<std::uint8_t>((y / 2 == 0 ? 10 : 30) + (x / 2 == 0 ? 0 : 10));
            const std::size_t  index = (static_cast<std::size_t>(y) * 4U + x) * 4U;
            source.pixels[index]     = value;       // R
            source.pixels[index + 3] = 255;         // A
        }
    }

    const auto scaled = DownscaleBoxRgba8(source, 2);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->width, 2U);
    EXPECT_EQ(scaled->height, 2U);
    EXPECT_EQ(scaled->pixels[(0U * 2U + 0U) * 4U], 10);  // 左上块均值
    EXPECT_EQ(scaled->pixels[(0U * 2U + 1U) * 4U], 20);  // 右上块均值
    EXPECT_EQ(scaled->pixels[(1U * 2U + 0U) * 4U], 30);  // 左下块均值
    EXPECT_EQ(scaled->pixels[(1U * 2U + 1U) * 4U], 40);  // 右下块均值

    // 确定性：再跑一次逐字节相同。
    const auto again = DownscaleBoxRgba8(source, 2);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->pixels, scaled->pixels);

    // 4 → 3 不是整数倍 ⇒ 明确失败（不引入任意重采样，保证确定性且实现简单）。
    EXPECT_FALSE(DownscaleBoxRgba8(source, 3).has_value());
    EXPECT_FALSE(DownscaleBoxRgba8(source, 0).has_value());
}

// **回落契约**（P6 的连带要求）：资源缺失时加载器**不抛异常**，只置失败位 + 可读原因；
// 干净克隆（`assets/textures/` 被 gitignore）必然走这条路径 ⇒ 游戏必须照常启动并回落程序生成贴图。
TEST(RealTextures, LoaderFailsSoftlyWithReasonWhenAssetsAreMissing) {
    const TempDirectory          directory("missing");
    const TerrainMaterialTable   table = TerrainMaterialTable::Default();  // 生命周期必须覆盖 loader（见其前置条件）

    const MaterialTextureAssetSpec spec { directory.Path() / "does_not_exist", 64 };
    MaterialTextureAssetLoader    loader(table, spec);
    EXPECT_TRUE(loader.Step(0)) << "根目录不存在 ⇒ 构造时即已判定失败（Step 直接返回“已结束”）";
    EXPECT_TRUE(loader.Failed());
    EXPECT_FALSE(loader.Reason().empty());
    EXPECT_NE(loader.Reason().find("资源根目录不存在"), std::string::npos) << "原因要能指向具体路径";

    // 根目录存在、但缺贴图 ⇒ 同样**软失败**并指出缺哪一张。
    const MaterialTextureAssetSpec spec2 { directory.Path(), 64 };
    MaterialTextureAssetLoader    loader2(table, spec2);
    (void)loader2.Step(100);
    EXPECT_TRUE(loader2.Failed());
    EXPECT_NE(loader2.Reason().find("grass"), std::string::npos) << "原因要指出是哪个材质目录 / 贴图；实际：" << loader2.Reason();
}
