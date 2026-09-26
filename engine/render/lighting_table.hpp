#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace vx {

/// 三通道颜色：**sRGB 作者色**，各分量 ∈ [0, 1]。
/// 转线性（`pow(c, 2.2)`）由 `SrgbToLinear` / `BuildLightingUniform` 在 **CPU 侧**完成。
using ColorRgb = std::array<float, 3>;

/// 太阳（方向光）配置。
struct SunLight {
    /// **由地表指向太阳**的单位方向（世界空间）。着色器按 `max(dot(N, direction), 0)` 使用，
    /// 故"光照行进方向"是它的相反数。加载时要求长度非零；`BuildLightingUniform` 负责归一化。
    std::array<float, 3> direction { 0.45F, 0.80F, 0.30F };

    /// 日光颜色（sRGB 作者色）。
    ColorRgb color { 1.00F, 0.96F, 0.88F };

    /// 日光强度（无量纲，线性光倍数），必须 ≥ 0。
    float intensity = 1.30F;
};

/// 半球天空光配置：给背光面以天空色，而不是死黑（ADR 0010 P1）。
struct SkyLight {
    /// 天顶色（sRGB）：法线朝上时的天空光颜色。
    ColorRgb zenithColor { 0.42F, 0.62F, 0.92F };

    /// 地平色（sRGB）：同时作为清屏色与 `fog.color` 缺失时的默认雾色。
    ColorRgb horizonColor { 0.70F, 0.80F, 0.92F };

    /// 地面反弹色（sRGB）：法线朝下时的天空光颜色。
    ColorRgb groundColor { 0.34F, 0.29F, 0.23F };

    /// 天空光强度（无量纲），必须 ≥ 0。替代旧的写死环境项。
    float intensity = 1.00F;
};

/// 级联阴影（CSM）配置（T21b / ADR 0010 P1）。
///
/// 这里只承载**数值**；级联分割与光空间矩阵是相机相关的每帧几何，由 `shadow_cascade.hpp` 的纯函数
/// 依据这些数值 + 相机参数算出（见 `BuildShadowUniform`）。
struct ShadowSettings {
    /// false 时片元着色器**整段跳过**阴影采样，且不执行阴影通道。
    bool enabled = true;

    /// 级联数，必须 ∈ [1, 4]（4 为引擎上限，见 `kMaxShadowCascades`）。
    int cascadeCount = 3;

    /// 每级阴影图分辨率（正方形边长），必须是 **2 的幂且 ≥ 256**。
    int resolution = 2048;

    /// 阴影覆盖距离（格，相机视距）：超出它的片元不投影（远景本就融入雾）。
    /// 必须 > 0，且实际使用值取 `min(相机远平面, maxDistance)`。
    float maxDistance = 180.0F;

    /// 级联分割的 log/uniform 混合系数，必须 ∈ [0, 1]：0 = 均匀分割、1 = 完全对数分割。
    float splitLambda = 0.75F;

    /// 深度偏移（阴影图深度单位，∈ [0, 1] 的深度范围内），必须 ≥ 0：抗自遮挡（acne）。
    float depthBias = 0.0015F;

    /// 法线偏移（世界单位 / 格），必须 ≥ 0：沿几何法线偏移采样点，抗 acne。
    float normalOffset = 0.05F;

    /// 投射体扩展的**下限兜底**（格，必须 ≥ 0）：即使 `game/` 从地形推导出的"最高投射体高度"为 0
    /// （空地形 / 误传），也保证每级阴影盒覆盖到级联中心之上该高度，避免高大投射体被裁掉（缺陷 1）。
    /// 默认 160 格足以覆盖测试地图的地标塔顶端（塔顶 260 格 − 基底 120 格 ≈ 140 格）。
    float casterHeightMin = 160.0F;
};

/// 指数高度雾配置（T21c）。
struct FogLayer {
    bool  enabled       = true;    ///< false 时着色器完全跳过雾计算
    float density       = 0.0030F; ///< 密度（1 / 格），必须 ≥ 0
    float heightFalloff = 0.02F;   ///< 高度衰减（1 / 格），必须 ≥ 0

    /// 雾色（sRGB 作者色）。配置里**可选**：缺失时加载器填入 `sky.horizon_color`（见 `LoadFromFile`）。
    ColorRgb color { 0.70F, 0.80F, 0.92F };
};

/// 片元着色器的光照 uniform 块（`set = 3, binding = 1`），std140 布局。
///
/// 字段排布与 `assets/shaders/mesh.frag` 的 `LightingBlock` **逐字对应**，每行一个 `vec4`：
///   - `sunDirectionIntensity` = `(方向 xyz, 强度)`
///   - `sunColorLinear`        = `(太阳色 rgb, 0)`
///   - `skyZenithIntensity`    = `(天顶色 rgb, 天空强度)`
///   - `skyHorizonLinear`      = `(地平色 rgb, 0)`
///   - `skyGroundLinear`       = `(地面反弹色 rgb, 0)`
///   - `cameraPositionWorld`   = `(相机世界位置 xyz, 0)`
///   - `fogColorDensity`       = `(雾色 rgb, 密度)`
///   - `fogParams`             = `(启用(1/0), 高度衰减, 0, 0)`
///
/// **颜色一律是线性光**（见 `BuildLightingUniform` 的说明）。
struct LightingUniform {
    float sunDirectionX    = 0.0F;
    float sunDirectionY    = 1.0F;
    float sunDirectionZ    = 0.0F;
    float sunIntensity     = 0.0F;

    float sunColorR        = 1.0F;
    float sunColorG        = 1.0F;
    float sunColorB        = 1.0F;
    float sunColorUnused   = 0.0F;

    float skyZenithR       = 0.0F;
    float skyZenithG       = 0.0F;
    float skyZenithB       = 0.0F;
    float skyIntensity     = 0.0F;

    float skyHorizonR      = 0.0F;
    float skyHorizonG      = 0.0F;
    float skyHorizonB      = 0.0F;
    float skyHorizonUnused = 0.0F;

    float skyGroundR       = 0.0F;
    float skyGroundG       = 0.0F;
    float skyGroundB       = 0.0F;
    float skyGroundUnused  = 0.0F;

    float cameraWorldX     = 0.0F;
    float cameraWorldY     = 0.0F;
    float cameraWorldZ     = 0.0F;
    float cameraWorldUnused = 0.0F;

    float fogColorR        = 0.0F;
    float fogColorG        = 0.0F;
    float fogColorB        = 0.0F;
    float fogDensity       = 0.0F;

    float fogEnabled       = 0.0F;  ///< 1 = 启用、0 = 禁用（着色器用它整体跳过雾）
    float fogHeightFalloff = 0.0F;
    float fogUnused0       = 0.0F;
    float fogUnused1       = 0.0F;
};

static_assert(sizeof(LightingUniform) == 8 * sizeof(float) * 4,
              "LightingUniform 必须与 mesh.frag 的 std140 布局逐字节一致");

/// sRGB 作者色 → 线性光：`pow(c, 2.2)`（逐分量）。
///
/// 唯一入口：`BuildLightingUniform` 与 `game/main.cpp` 的清屏色都走这里，不各自写一份换算。
[[nodiscard]] ColorRgb SrgbToLinear(const ColorRgb& srgb) noexcept;

class LightingTable;

/// 用**同一份**光照表构建 GPU uniform 块（唯一事实来源，ADR 0010）。
///
/// 这是 CPU→GPU 光照与雾参数的**唯一**入口：任何新增参数都必须先加进 `LightingTable`，
/// 再在这里投影；**禁止**在着色器里硬编码第二份方向 / 强度 / 颜色 / 雾参数。
///
/// **颜色语义**：全体颜色（太阳 / 天顶 / 地平 / 地面反弹 / 雾）在这里由 sRGB 作者色转成**线性光**
/// 后写入 uniform。之所以能在 CPU 侧预转：它们是**常量**，与片元无关，一次转换即对所有片元成立；
/// 而材质 albedo 来自纹理（逐像素不同）、无法预转，故仍由 `mesh.frag` 在片元内做 `pow(c, 2.2)`。
/// 两条路径的目标空间一致（线性光），只是转换时机不同。
///
/// `sun.direction` 在这里归一化，uniform 里恒为单位向量。
/// 前置条件：`table` 已通过 `LoadFromFile` 或 `Default()` 填充；
/// `cameraX/Y/Z` 为**绝对世界坐标**下的相机位置（雾按视距插值需要）。
[[nodiscard]] LightingUniform BuildLightingUniform(const LightingTable& table, double cameraX, double cameraY,
                                                   double cameraZ) noexcept;

/// 光照与雾配置表：启动期从 `assets/config/lighting.toml` 一次性读入（ADR 0005 / ADR 0010）。
///
/// 边界：配置表是**渲染参数的输入**，不是运行时状态；运行期只读，热路径不触碰解析器。
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值。
class LightingTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    /// v2（T21b）：新增 `[shadow]` 段（级联阴影参数）——缺失即报错，**不**按 v1 兼容读取。
    /// v4（缺陷 1 修复）：`[shadow]` 新增 `caster_height_min`（投射体扩展下限兜底）——缺失即报错。
    ///   （v3 未使用；版本号按缺陷修复要求直接升到 4。）
    static constexpr int kSchemaVersion = 4;

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    /// 前置条件：`path` 指向待加载的光照表文件。
    [[nodiscard]] static LightingTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试与离线工具使用。
    /// **不是** `LoadFromFile` 失败时的回退路径 —— 那条路径必须报错。
    [[nodiscard]] static LightingTable Default();

    [[nodiscard]] const SunLight& Sun() const noexcept { return m_sun; }
    [[nodiscard]] const SkyLight& Sky() const noexcept { return m_sky; }
    [[nodiscard]] const FogLayer& Fog() const noexcept { return m_fog; }
    [[nodiscard]] const ShadowSettings& Shadow() const noexcept { return m_shadow; }

    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

private:
    SunLight       m_sun;
    SkyLight       m_sky;
    FogLayer       m_fog;
    ShadowSettings m_shadow;
    int            m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
