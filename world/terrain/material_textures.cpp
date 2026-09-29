#include "terrain/material_textures.hpp"

#include "generation/seed.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "FastNoiseLite.h"  // third_party/FastNoiseLite（vendored，MIT）

namespace vx {
namespace {

/// 噪声通道基号：与地形高度（1~3）、材质抖动（4）错开，避免层间耦合（方案 §3.2）。
/// 每个"频段 / 用途"占一段连续的 4 个通道（按层递增），保证层与层、频段与频段都不同种子。
constexpr std::uint64_t kAlbedoLowChannelBase  = 10;  ///< 10~13：albedo 低频"结构"
constexpr std::uint64_t kAlbedoHighChannelBase = 30;  ///< 30~33：albedo 高频"颗粒"
constexpr std::uint64_t kNormalLowChannelBase  = 20;  ///< 20~23：法线高频段所用的低频高度
constexpr std::uint64_t kNormalHighChannelBase = 40;  ///< 40~43：法线低频段所用的高频高度
constexpr std::uint64_t kRoughnessChannelBase  = 50;  ///< 50~53：逐层粗糙度变化
constexpr std::uint64_t kAoChannelBase         = 60;  ///< 60~63：逐层 AO 细节
constexpr std::uint64_t kMacroChannel          = 70;  ///< 70：宏观变化（单层）

/// 多尺度叠加权重（ADR 0010 P2）：低频"结构"占主导、高频"颗粒"提供细节。
/// 单频噪声无论振幅多大都会"看起来平"，必须叠加高频段（见 ADR 0010 背景诊断 #4）。
constexpr float kLowBandWeight  = 0.65F;
constexpr float kHighBandWeight = 0.35F;

/// 每层风格参数，层序与材质槽一致（0 草 / 1 土 / 2 岩 / 3 沙）。
/// 高频段的周期约为低频段的 3 倍，保证两个频段在频谱上分开。
constexpr float kAlbedoLowPeriods[kMaterialSlotCount]  = { 10.0F, 6.0F, 7.0F, 14.0F };
constexpr float kAlbedoHighPeriods[kMaterialSlotCount] = { 30.0F, 18.0F, 21.0F, 42.0F };
constexpr float kAlbedoBase[kMaterialSlotCount]        = { 0.72F, 0.66F, 0.50F, 0.78F };
constexpr float kAlbedoRange[kMaterialSlotCount]       = { 0.28F, 0.34F, 0.50F, 0.22F };
constexpr float kHeightLowPeriods[kMaterialSlotCount]  = { 12.0F, 8.0F, 9.0F, 18.0F };
constexpr float kHeightHighPeriods[kMaterialSlotCount] = { 36.0F, 24.0F, 27.0F, 54.0F };
constexpr float kNormalStrength[kMaterialSlotCount]    = { 6.0F, 8.0F, 14.0F, 4.0F };
constexpr float kRoughnessPeriods[kMaterialSlotCount]  = { 16.0F, 10.0F, 12.0F, 24.0F };
constexpr float kAoPeriods[kMaterialSlotCount]         = { 8.0F, 6.0F, 7.0F, 12.0F };

/// 宏观变化：低频大尺度（周期数少 = 特征大）；与各层的 `macro_uv_scale`（远小于 `uv_scale`）配合，
/// 在约 30~60 格的尺度上起伏，用于打破基础贴图约 6~10 格一次的重复感。
constexpr float kMacroPeriods = 3.0F;

/// AO 细节值域：`[kAoFloor, 1]`——贴图下限不压到 0（真正的强度由材质表的 `ao` 决定）。
constexpr float kAoFloor = 0.60F;

/// 配置一个噪声实例：OpenSimplex2 + FBm，频率固定为 1（坐标以「特征数」为单位传入）。
void ConfigureNoise(FastNoiseLite& noise, std::int32_t seed) {
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(4);
    noise.SetFrequency(1.0F);
}

/// tile 上无缝采样：把噪声与自身按整块平移的四份按双线性权重叠起来，边界周期连续。
[[nodiscard]] float SeamlessNoise(const FastNoiseLite& noise, float u, float v, float periods) noexcept {
    const float x = u * periods;
    const float y = v * periods;
    const float n00 = noise.GetNoise(x, y);
    const float n10 = noise.GetNoise(x - periods, y);
    const float n01 = noise.GetNoise(x, y - periods);
    const float n11 = noise.GetNoise(x - periods, y - periods);
    return n00 * (1.0F - u) * (1.0F - v) + n10 * u * (1.0F - v) + n01 * (1.0F - u) * v + n11 * u * v;
}

[[nodiscard]] std::uint8_t EncodeUnorm(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

/// 把灰阶值写入 RGBA 像素的 rgb 三通道，a 固定 255（贴图只承载细节，颜色由 tint 决定）。
void WriteGray(std::vector<std::uint8_t>& rgba, std::size_t pixelIndex, std::uint8_t value) noexcept {
    const std::size_t base = pixelIndex * 4U;
    rgba[base + 0U]        = value;
    rgba[base + 1U]        = value;
    rgba[base + 2U]        = value;
    rgba[base + 3U]        = 255U;
}

}  // namespace

/// 分步生成器的实现细节（噪声实例 = `FastNoiseLite`，只允许出现在 .cpp：它是 world 的 PRIVATE 依赖）。
///
/// 状态机把原先生成函数的三段循环摊平成"一次一行"：
///   `LayerPixels`（逐层 albedo / roughness / AO / 高度）→ `LayerNormals`（逐层法线）→ `Macro`（宏观变化）。
/// 行序与原先完全一致，因此**逐字节结果不变**。
struct MaterialTextureBuilder::Impl {
    enum class Phase : std::uint8_t {
        LayerPixels,   ///< 当前层的像素行（albedo / roughness / AO / 高度缓冲）
        LayerNormals,  ///< 当前层的法线行（由高度缓冲的中心差分求）
        Macro,         ///< 宏观变化图的行
        Done,          ///< 全部完成
    };

    Impl(std::uint64_t seed, std::uint32_t textureSize) : worldSeed(seed), size(textureSize) {
        set.size       = size;
        set.layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);

        const std::size_t layerPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        const std::size_t layerBytes  = layerPixels * 4U;
        const std::size_t totalBytes  = layerBytes * static_cast<std::size_t>(kMaterialSlotCount);

        set.albedoRgba.assign(totalBytes, 0U);
        set.normalRgba.assign(totalBytes, 255U);  // a 通道恒为 255；rgb 稍后覆盖
        set.roughnessRgba.assign(totalBytes, 255U);
        set.aoRgba.assign(totalBytes, 255U);
        set.macroRgba.assign(layerBytes * static_cast<std::size_t>(kMaterialMacroLayerCount), 255U);

        heights.assign(layerPixels, 0.0F);
        rowsTotal = static_cast<std::size_t>(kMaterialSlotCount) * static_cast<std::size_t>(size) * 2U +
                    static_cast<std::size_t>(size);
        BeginLayer(0);
    }

    /// 进入一层：按层序建好该层的噪声实例（其余状态由 `Step` 维护）。
    void BeginLayer(int layerIndex) {
        layer                       = layerIndex;
        const std::uint64_t channel = static_cast<std::uint64_t>(layerIndex);
        ConfigureNoise(albedoLow, DeriveChannelSeed(worldSeed, kAlbedoLowChannelBase + channel));
        ConfigureNoise(albedoHigh, DeriveChannelSeed(worldSeed, kAlbedoHighChannelBase + channel));
        ConfigureNoise(heightLow, DeriveChannelSeed(worldSeed, kNormalLowChannelBase + channel));
        ConfigureNoise(heightHigh, DeriveChannelSeed(worldSeed, kNormalHighChannelBase + channel));
        ConfigureNoise(roughnessNoise, DeriveChannelSeed(worldSeed, kRoughnessChannelBase + channel));
        ConfigureNoise(aoNoise, DeriveChannelSeed(worldSeed, kAoChannelBase + channel));
    }

    /// 当前层的第 `y` 行：albedo（双频叠加）+ 高度缓冲 + 粗糙度变化 + AO 细节。
    void PixelRow(std::uint32_t y) {
        const std::size_t layerPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        const std::size_t layerOffset = static_cast<std::size_t>(layer) * layerPixels;
        const float       v           = static_cast<float>(y) / static_cast<float>(size);

        for (std::uint32_t x = 0; x < size; ++x) {
            const float u         = static_cast<float>(x) / static_cast<float>(size);
            const float structure = SeamlessNoise(albedoLow, u, v, kAlbedoLowPeriods[layer]);
            const float grain     = SeamlessNoise(albedoHigh, u, v, kAlbedoHighPeriods[layer]);

            // 两个频段叠加：低频"结构" + 高频"颗粒"。岩层对低频段取 ridge（脊状）塑形，
            // 暗缝 + 亮脊，读起来像开裂的岩石；高频段仍按普通颗粒叠加。
            const float lowShape = (layer == 2) ? (1.0F - std::fabs(structure)) : structure;
            const float detail   = kLowBandWeight * lowShape + kHighBandWeight * grain;

            const std::uint8_t value = EncodeUnorm(kAlbedoBase[layer] + kAlbedoRange[layer] * detail);
            WriteGray(set.albedoRgba, layerOffset + static_cast<std::size_t>(y) * size + x, value);

            // 高度场同样双频叠加：法线因此同时承载"大起伏"与"细小颗粒"。
            heights[static_cast<std::size_t>(y) * size + x] =
                kLowBandWeight * SeamlessNoise(heightLow, u, v, kHeightLowPeriods[layer]) +
                kHighBandWeight * SeamlessNoise(heightHigh, u, v, kHeightHighPeriods[layer]);

            // 粗糙度变化：居中在 0.5，着色器折算成 ±20% 乘子（±20% 之外仍由材质表的基准值缩放）。
            const float rough = 0.5F + 0.5F * SeamlessNoise(roughnessNoise, u, v, kRoughnessPeriods[layer]);
            WriteGray(set.roughnessRgba, layerOffset + static_cast<std::size_t>(y) * size + x, EncodeUnorm(rough));

            // AO 细节：值域 [kAoFloor, 1]，1 = 完全开阔；强度由材质表的 ao 决定。
            const float aoFactor =
                kAoFloor + (1.0F - kAoFloor) * (0.5F + 0.5F * SeamlessNoise(aoNoise, u, v, kAoPeriods[layer]));
            WriteGray(set.aoRgba, layerOffset + static_cast<std::size_t>(y) * size + x, EncodeUnorm(aoFactor));
        }
    }

    /// 当前层的第 `y` 行法线：由高度场梯度求切线空间法线（中心差分 + 环绕索引，平铺后仍连续）。
    void NormalRow(std::uint32_t y) {
        const std::size_t   layerPixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        const std::size_t   layerOffset = static_cast<std::size_t>(layer) * layerPixels;
        const std::uint32_t last        = size - 1U;
        const std::uint32_t yPrev       = (y == 0U) ? last : (y - 1U);
        const std::uint32_t yNext       = (y == last) ? 0U : (y + 1U);

        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint32_t xPrev = (x == 0U) ? last : (x - 1U);
            const std::uint32_t xNext = (x == last) ? 0U : (x + 1U);

            const float du = heights[static_cast<std::size_t>(y) * size + xNext] -
                             heights[static_cast<std::size_t>(y) * size + xPrev];
            const float dv = heights[static_cast<std::size_t>(yNext) * size + x] -
                             heights[static_cast<std::size_t>(yPrev) * size + x];

            const glm::vec3 normal = glm::normalize(
                glm::vec3(-du * kNormalStrength[layer], -dv * kNormalStrength[layer], 1.0F));

            const std::size_t base = (layerOffset + static_cast<std::size_t>(y) * size + x) * 4U;
            set.normalRgba[base + 0] = EncodeUnorm(normal.x * 0.5F + 0.5F);
            set.normalRgba[base + 1] = EncodeUnorm(normal.y * 0.5F + 0.5F);
            set.normalRgba[base + 2] = EncodeUnorm(normal.z * 0.5F + 0.5F);
            set.normalRgba[base + 3] = 255U;
        }
    }

    /// 宏观变化的第 `y` 行：**1 层**低频大尺度噪声（存入 R 通道，rgb 同值便于调试）。
    void MacroRow(std::uint32_t y) {
        const float v = static_cast<float>(y) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float value = 0.5F + 0.5F * SeamlessNoise(macroNoise, u, v, kMacroPeriods);
            WriteGray(set.macroRgba, static_cast<std::size_t>(y) * size + x, EncodeUnorm(value));
        }
    }

    std::uint64_t      worldSeed;
    std::uint32_t      size;
    MaterialTextureSet set;

    Phase         phase = Phase::LayerPixels;
    int           layer = 0;
    std::uint32_t row   = 0;
    std::size_t   rowsDone  = 0;
    std::size_t   rowsTotal = 0;

    FastNoiseLite albedoLow;
    FastNoiseLite albedoHigh;
    FastNoiseLite heightLow;
    FastNoiseLite heightHigh;
    FastNoiseLite roughnessNoise;
    FastNoiseLite aoNoise;
    FastNoiseLite macroNoise;
    std::vector<float> heights;
};

MaterialTextureBuilder::MaterialTextureBuilder(std::uint64_t worldSeed, std::uint32_t size)
    : m_impl(new Impl(worldSeed, size)) {}

MaterialTextureBuilder::~MaterialTextureBuilder() = default;

bool MaterialTextureBuilder::Step(std::size_t maxRows) {
    Impl&       impl = *m_impl;
    std::size_t rows = 0;

    while (rows < maxRows && impl.phase != Impl::Phase::Done) {
        switch (impl.phase) {
            case Impl::Phase::LayerPixels:
                impl.PixelRow(impl.row);
                ++impl.row;
                if (impl.row >= impl.size) {
                    impl.phase = Impl::Phase::LayerNormals;
                    impl.row   = 0;
                }
                break;
            case Impl::Phase::LayerNormals:
                impl.NormalRow(impl.row);
                ++impl.row;
                if (impl.row >= impl.size) {
                    impl.row = 0;
                    ++impl.layer;
                    if (impl.layer >= kMaterialSlotCount) {
                        impl.phase = Impl::Phase::Macro;
                        ConfigureNoise(impl.macroNoise, DeriveChannelSeed(impl.worldSeed, kMacroChannel));
                    } else {
                        impl.phase = Impl::Phase::LayerPixels;
                        impl.BeginLayer(impl.layer);
                    }
                }
                break;
            case Impl::Phase::Macro:
                impl.MacroRow(impl.row);
                ++impl.row;
                if (impl.row >= impl.size) {
                    impl.phase = Impl::Phase::Done;
                    impl.row   = 0;
                }
                break;
            case Impl::Phase::Done:
                break;
        }
        ++rows;
        ++impl.rowsDone;
    }
    return impl.phase == Impl::Phase::Done;
}

bool MaterialTextureBuilder::Done() const noexcept { return m_impl->phase == Impl::Phase::Done; }

float MaterialTextureBuilder::Progress() const noexcept {
    if (m_impl->rowsTotal == 0) {
        return 1.0F;
    }
    const float ratio = static_cast<float>(m_impl->rowsDone) / static_cast<float>(m_impl->rowsTotal);
    return std::clamp(ratio, 0.0F, 1.0F);
}

MaterialTextureSet MaterialTextureBuilder::Take() { return std::move(m_impl->set); }

MaterialTextureSet GenerateMaterialTextures(std::uint64_t worldSeed, std::uint32_t size) {
    MaterialTextureBuilder builder(worldSeed, size);
    while (!builder.Step(/*maxRows=*/64)) {
    }
    return builder.Take();
}

// ---------------------------------------------------------------------------
// T66 / V0.3 ⓒ：真实 CC0 美术贴图的解析与加载（资源不入库，见 NOTICE.md 的「美术资源台账」）
// ---------------------------------------------------------------------------

std::filesystem::path ResolveMapFile(const std::filesystem::path& directory, const std::string& stem) {
    // 扩展名顺序固定 ⇒ 同一份资源目录在任何机器上解析出**同一个文件**（红线 7）。
    static constexpr const char* kExtensions[] = { ".jpg", ".jpeg", ".png", ".tga", ".bmp" };
    std::error_code             code;
    for (const char* extension : kExtensions) {
        std::filesystem::path candidate = directory / (stem + extension);
        if (std::filesystem::is_regular_file(candidate, code) && !code) {
            return candidate;
        }
        code.clear();
    }
    return {};
}

std::optional<ImageRgba8> DownscaleBoxRgba8(const ImageRgba8& source, std::uint32_t target) {
    if (target == 0 || source.width == 0 || source.height == 0) {
        return std::nullopt;
    }
    if (source.pixels.size() != static_cast<std::size_t>(source.width) * source.height * 4U) {
        return std::nullopt;  // 输入不满足 ImageRgba8 的长度约定
    }
    if (source.width % target != 0 || source.height % target != 0) {
        return std::nullopt;  // 只支持**整数倍**降采样（不引入任意重采样，保持确定性且实现简单）
    }
    const std::uint32_t factorX = source.width / target;
    const std::uint32_t factorY = source.height / target;

    ImageRgba8 result;
    result.width  = target;
    result.height = target;
    result.pixels.assign(static_cast<std::size_t>(target) * target * 4U, 0);

    const std::uint32_t blockArea = factorX * factorY;
    for (std::uint32_t y = 0; y < target; ++y) {
        for (std::uint32_t x = 0; x < target; ++x) {
            std::uint32_t sum[4] = { 0, 0, 0, 0 };
            for (std::uint32_t dy = 0; dy < factorY; ++dy) {
                const std::size_t rowOffset =
                    (static_cast<std::size_t>(y * factorY + dy) * source.width + static_cast<std::size_t>(x * factorX)) * 4U;
                for (std::uint32_t dx = 0; dx < factorX; ++dx) {
                    const std::size_t index = rowOffset + static_cast<std::size_t>(dx) * 4U;
                    for (int channel = 0; channel < 4; ++channel) {
                        sum[channel] += source.pixels[index + static_cast<std::size_t>(channel)];
                    }
                }
            }
            const std::size_t destination =
                (static_cast<std::size_t>(y) * target + static_cast<std::size_t>(x)) * 4U;
            for (int channel = 0; channel < 4; ++channel) {
                // 四舍五入到最近整数（+ 半个分母），保证与"整数平均"的直觉一致。
                result.pixels[destination + static_cast<std::size_t>(channel)] =
                    static_cast<std::uint8_t>((sum[channel] + blockArea / 2U) / blockArea);
            }
        }
    }
    return result;
}

/// 真实贴图的总张数 = 层数 × 四件套（albedo / normal / roughness / ao）。
constexpr std::size_t kMaterialTextureAssetMapCount = static_cast<std::size_t>(kMaterialSlotCount) * 4U;

/// 真实贴图加载的**实现**（分步）：状态只是一个 (层, 件套) 游标 + 累加中的结果集。
/// 之所以要分步：解码 16 张 2048² JPEG + 降采样要数秒，一次跑完会让画面停下等待（SKILL「不冻结画面」）。
struct MaterialTextureAssetLoader::Impl {
    const TerrainMaterialTable* table = nullptr;
    MaterialTextureAssetSpec    spec;

    MaterialTextureAssetSet set;      ///< 累加中的结果（层-major 切片按游标逐块写入）
    std::size_t             cursor   = 0;  ///< 已处理的 (层, 件套) 数，总数 = 4 × 4 = 16
    std::uint32_t           sourceSize0 = 0;  ///< 第一张有效贴图的源边长（层间必须一致）
    bool                    failed   = false;
    std::string             reason;

    void Fail(const std::string& why) {
        failed = true;
        reason = why;
    }

    /// 处理第 `cursor` 张（层 = cursor / 4、件套 = cursor % 4）；异常一律转成"失败 + 原因"（不向上抛：启动期要能回落）。
    void ProcessOne() {
        const std::size_t slot = cursor / 4U;
        const int         map  = static_cast<int>(cursor % 4U);
        const MaterialLayer&    layer     = table->Layer(static_cast<int>(slot));
        const std::filesystem::path directory = spec.root / layer.name;

        std::error_code code;
        if (!std::filesystem::is_directory(directory, code) || code) {
            Fail("缺少材质目录：" + directory.string());
            return;
        }
        const std::filesystem::path file = ResolveMapFile(directory, kMaterialMapStems[map]);
        if (file.empty()) {
            Fail("缺少贴图：" + (directory / (std::string(kMaterialMapStems[map]) + ".<jpg|jpeg|png|tga|bmp>")).string());
            return;
        }

        ImageRgba8 source;
        try {
            source = LoadImageRgba8(file);
        } catch (const std::exception& error) {
            Fail(std::string("解码失败：") + file.string() + "（" + error.what() + "）");
            return;
        }
        if (source.width != source.height) {
            Fail("贴图不是正方形：" + file.string());
            return;
        }
        if (sourceSize0 == 0) {
            sourceSize0 = source.width;  // 第一张图定义全局源尺寸
        } else if (source.width != sourceSize0) {
            // 层间 / 件套间尺寸必须一致：纹理数组只支持**单一尺寸**，混用会让某一层被错误拉伸。
            Fail("贴图尺寸不一致：" + file.string() + "（" + std::to_string(source.width) + " vs " +
                 std::to_string(sourceSize0) + "）");
            return;
        }
        set.sourceSize[slot] = source.width;

        const std::optional<ImageRgba8> scaled = DownscaleBoxRgba8(source, spec.size);
        if (!scaled.has_value()) {
            Fail("源图边长必须是目标边长（" + std::to_string(spec.size) + "）的整数倍：" + file.string() + "（源 " +
                 std::to_string(source.width) + "）");
            return;
        }

        std::vector<std::uint8_t>& target = (map == 0)   ? set.albedoRgba
                                            : (map == 1) ? set.normalRgba
                                            : (map == 2) ? set.roughnessRgba
                                                         : set.aoRgba;
        const std::size_t offset = static_cast<std::size_t>(spec.size) * spec.size * 4U * slot;
        std::copy(scaled->pixels.begin(), scaled->pixels.end(),
                  target.begin() + static_cast<std::ptrdiff_t>(offset));
    }
};

MaterialTextureAssetLoader::MaterialTextureAssetLoader(const TerrainMaterialTable& table, MaterialTextureAssetSpec spec)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->table = &table;
    m_impl->spec  = std::move(spec);

    if (m_impl->spec.size == 0) {
        m_impl->Fail("规格非法：size == 0");
        return;
    }
    std::error_code code;
    if (!std::filesystem::is_directory(m_impl->spec.root, code) || code) {
        // 资源不入库（见 docs/plans/v0.3.md §1.1）：干净克隆下**必然**走到这里 ⇒ 只记录原因，由调用方 WARN 后回落。
        m_impl->Fail("资源根目录不存在：" + m_impl->spec.root.string());
        return;
    }

    Impl& impl = *m_impl;
    impl.set.size       = impl.spec.size;
    impl.set.layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);
    const std::size_t totalBytes =
        static_cast<std::size_t>(impl.spec.size) * impl.spec.size * 4U * static_cast<std::size_t>(kMaterialSlotCount);
    impl.set.albedoRgba.assign(totalBytes, 0);
    impl.set.normalRgba.assign(totalBytes, 0);
    impl.set.roughnessRgba.assign(totalBytes, 0);
    impl.set.aoRgba.assign(totalBytes, 0);
    impl.set.sourceSize.assign(static_cast<std::size_t>(kMaterialSlotCount), 0);
}

MaterialTextureAssetLoader::~MaterialTextureAssetLoader() = default;

bool MaterialTextureAssetLoader::Step(std::size_t maxMaps) {
    Impl& impl = *m_impl;
    std::size_t processed = 0;
    while (!impl.failed && impl.cursor < kMaterialTextureAssetMapCount && processed < maxMaps) {
        impl.ProcessOne();
        ++impl.cursor;
        ++processed;
    }
    return impl.failed || impl.cursor >= kMaterialTextureAssetMapCount;
}

float MaterialTextureAssetLoader::Progress() const noexcept {
    const float ratio = static_cast<float>(m_impl->cursor) / static_cast<float>(kMaterialTextureAssetMapCount);
    return std::clamp(ratio, 0.0F, 1.0F);
}

bool MaterialTextureAssetLoader::Failed() const noexcept { return m_impl->failed; }

const std::string& MaterialTextureAssetLoader::Reason() const noexcept { return m_impl->reason; }

MaterialTextureAssetSet MaterialTextureAssetLoader::Take() { return std::move(m_impl->set); }

std::optional<MaterialTextureAssetSet> TryLoadMaterialTextureAssets(const TerrainMaterialTable& table,
                                                                   const MaterialTextureAssetSpec& spec,
                                                                   std::string* reasonOut) {
    MaterialTextureAssetLoader loader(table, spec);
    while (!loader.Step(kMaterialTextureAssetMapCount)) {
    }
    if (loader.Failed()) {
        if (reasonOut != nullptr) {
            *reasonOut = loader.Reason();
        }
        return std::nullopt;
    }
    if (reasonOut != nullptr) {
        reasonOut->clear();
    }
    return loader.Take();
}

}  // namespace vx
