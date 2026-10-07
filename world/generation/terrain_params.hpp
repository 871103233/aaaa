#pragma once

#include <cstdint>
#include <filesystem>

namespace vx {

/// 地貌类型（W3）：三类，由**低频地貌掩罩**的取值分档得到。
enum class LandformKind : std::uint8_t {
    Plains    = 0,  ///< 平原：低而平
    Hills     = 1,  ///< 丘陵：中等起伏
    Mountains = 2,  ///< 山川：高而崎岖
};

/// 地形幅度调制：把三层噪声的**叠加起伏**乘以 `amplitudeScale`、并把**基线高度**平移 `offsetBlocks`。
///
/// `landform.enabled == false` 时恒为 `{1.0F, 0.0F}`（⇒ 生成结果与引入地貌层之前**逐位一致**）。
struct LandformModulation {
    float amplitudeScale = 1.0F;
    float offsetBlocks   = 0.0F;
};

/// 地貌分区参数（`assets/config/terrain.toml` 的 `[landform]` 段）。
///
/// 语义：掩罩值 `m ∈ [0, 1]`（由低频噪声归一化得到）分三段 ——
///   `m < hillsStart` ⇒ 平原；`hillsStart ≤ m < mountainsStart` ⇒ 丘陵；`m ≥ mountainsStart` ⇒ 山川。
/// 段之间用宽度 `blend` 的 **smoothstep** 过渡（避免硬边界）。约束：`0 ≤ hillsStart ≤ mountainsStart ≤ 1`、
/// `hillsStart + blend ≤ mountainsStart`、`blend ≥ 0`（加载时校验，非法即抛，ADR 0005 口径）。
struct TerrainLandformParams {
    bool  enabled = false;  ///< 缺省**关闭** ⇒ 与引入本层之前逐位一致
    float frequency        = 0.0012F;  ///< 掩罩频率（很低频 ⇒ 成片的大区域）
    float hillsStart       = 0.42F;    ///< 平原 → 丘陵的分界
    float mountainsStart   = 0.66F;    ///< 丘陵 → 山川的分界
    float blend            = 0.08F;    ///< 段间过渡带宽

    /// 各段的地形幅度倍数（相对三层噪声叠加起伏）。
    float plainsAmplitudeScale    = 0.30F;
    float hillsAmplitudeScale     = 0.85F;
    float mountainsAmplitudeScale = 1.45F;

    /// 各段的基线高度平移（格；正 = 抬高）。
    float plainsOffsetBlocks    = -8.0F;
    float hillsOffsetBlocks     = 0.0F;
    float mountainsOffsetBlocks = 24.0F;

    /// 掩罩噪声的**通道种子**（与高度 / 细节 / 粗糙 / 变化四层不同通道，避免层间耦合）。
    std::uint64_t seedChannel = 5;
};

/// 悬垂噪声参数（W4 地表体积壳使用的 **3D** 噪声；`assets/config/terrain.toml` 的 `[overhang]` 段）。
///
/// 语义：体积壳把 `overhang(x,y,z) × amplitudeBlocks` 叠加到"距地表的有符号距离"上。
/// 当 `amplitudeBlocks × 2π × frequency > 1`（噪声梯度足以压过距离场的 1）时，等值面会**折叠** ⇒ 出现**悬垂 / 洞穴**。
struct TerrainOverhangParams {
    float         frequency       = 0.05F;   ///< 3D 噪声频率（波长 ≈ 20 格）
    float         amplitudeBlocks = 7.0F;    ///< 悬垂位移幅度（格）
    std::uint64_t seedChannel     = 6;       ///< 与既有通道 1~5 不重复
};

/// 洞穴网络参数（W5 地表体积壳使用的 **3D** 噪声；`assets/config/terrain.toml` 的 `[caves]` 段）。
///
/// 语义（业界口径 = Minecraft 的 *spaghetti carver*）：用两条**互不相关**的 3D 噪声 `a`、`b`
/// 构成**隧道网络** —— 在 `sqrt(a² + b²) < tunnelRadius` 处雕刻岩体。理由：`a = 0` 与 `b = 0` 各是一张
/// 曲面，两曲面的**交线**是一条曲线；把交线邻域加粗成管 ⇒ 得到**连续、分叉、四通八达**的隧道网
/// （对照 Deep Rock Galactic 的程序化洞穴网络）。
///
/// `carveStrengthBlocks` = 隧道的**最大雕刻量**（格）：只有雕刻量超过"该点到地表的深度"时才真正挖空
/// ⇒ 越深越难成洞，隧道**自然收敛**、不会把整块壳挖穿。
struct TerrainCaveParams {
    bool          enabled             = false;  ///< 缺省**关闭** ⇒ `CaveCarveAt` 恒 0 ⇒ 与 W4 逐位一致
    float         frequency           = 0.02F;  ///< 隧道噪声频率（波长 ≈ 50 格）
    float         tunnelRadius        = 0.32F;  ///< 隧道半径（噪声值域口径）；世界半径 ≈ `radius / (2π·frequency)`
    float         carveStrengthBlocks = 22.0F;  ///< 最大雕刻量（格）
    float         depthFadeBlocks     = 8.0F;   ///< 壳底附近的淡出带宽（格）⇒ 隧道不穿出壳的可视边界
    std::uint64_t seedChannel         = 7;      ///< 与既有通道 1~6 不重复（只占一个通道号，见 Impl）
};

/// 河流参数（W6；`assets/config/terrain.toml` 的 `[river]` 段）。
///
/// 语义（[ADR 0027](../../docs/adr/0027-water-representation.md)）：河道由**下坡路径**给出（从高处沿最陡下降方向 +
/// 确定性抖动），河床按"下切场"刻进地表壳（层②），水面按**静态水位**生成。水位 = 河床底 + `waterDepthBlocks`，
/// 并沿程强制**单调不升**（水往低处流）。**不做**流体模拟 / 游泳 / 动态水位。
struct TerrainRiverParams {
    bool          enabled                = false;  ///< 缺省**关闭** ⇒ 下切恒 0 ⇒ 与 W5 逐位一致
    float         stepBlocks             = 8.0F;   ///< 路径采样步长（格）
    int           maxNodes               = 512;    ///< 路径节点上限（防失控；到上限即停）
    float         channelDepthBlocks     = 4.0F;   ///< 河道中心的下切深度（格）
    float         channelHalfWidthBlocks = 6.0F;   ///< 河道下切的半宽（格）
    float         bankHalfWidthBlocks    = 4.0F;   ///< 河岸过渡半宽（格；下切由河道半宽线性衰减到 0）
    float         waterDepthBlocks       = 2.0F;   ///< 水面距河床底的高度（格）
    float         jitterRadians          = 0.35F;  ///< 下坡方向的确定性抖动**最大偏角**（弧度，域扭曲）
    float         jitterFrequency        = 0.01F;  ///< 抖动噪声频率（波长 ≈ 100 格 ⇒ 河道蜿蜒尺度）
    std::uint64_t seedChannel            = 8;      ///< 与既有通道 1~7 不重复
};

/// 气候参数（V0.6 C7）：**温度 / 湿度两张 2D 低频噪声** —— `tech-plan-v2.0.md` §3.1
/// "2D 温度与湿度出生物群系，且**与高度图解耦**"。
///
/// 语义：两张噪声各自归一化到 `[0, 1]`（与 `LandformMaskAt` 同口径），**只供内容放置判据使用**
/// （`PlacementRule` 的温度 / 湿度区间），**不参与任何地形生成** ⇒ 调参 / 缺省不影响既有地形输出。
/// 通道号必须与既有 8 个通道（1~8）不重复。
struct TerrainClimateParams {
    float         temperatureFrequency   = 0.0009F;  ///< 温度噪声频率（波长 ≈ 1100 格 ⇒ 大尺度气候带）
    float         humidityFrequency      = 0.0014F;  ///< 湿度噪声频率（波长 ≈ 700 格）
    std::uint64_t temperatureSeedChannel = 9;        ///< 与既有通道 1~8 不重复
    std::uint64_t humiditySeedChannel    = 10;       ///< 与既有通道 1~9 不重复
};

/// 地表生成参数（W3 起从代码常量外提为配置；**默认值 == 引入本层之前的三层噪声数值**）。
///
/// 依据：`docs/tech-plan-v2.0.md` §3.1「生成参数全部来自 TOML」与 ADR 0005（启动期校验、非法即抛）。
struct TerrainGenerationParams {
    // ---- 既有三层噪声（默认与历史常量逐值相同）----
    float baseFrequency   = 0.0035F;
    float detailFrequency = 0.015F;
    float roughFrequency  = 0.06F;

    float baseAmplitude   = 96.0F;
    float detailAmplitude = 22.0F;
    float roughAmplitude  = 4.0F;

    float heightOffsetBlocks  = 128.0F;  ///< 让基线地面落在世界垂直范围（0~512）的中段
    float variationFrequency  = 0.05F;   ///< 材质过渡带抖动（`VariationAt`）的频率

    TerrainLandformParams landform {};

    /// 悬垂噪声（W4 地表体积壳使用）。默认值即启用壳时的推荐值；不启用壳时**不参与**任何现有路径。
    TerrainOverhangParams overhang {};

    /// 洞穴网络（W5 地表体积壳使用）。默认**关闭** ⇒ 不参与任何现有路径。
    TerrainCaveParams caves {};

    /// 河流（W6 地表体积壳使用）。默认**关闭** ⇒ 不参与任何现有路径。
    TerrainRiverParams river {};

    /// 气候（V0.6 C7：温度 / 湿度；只供内容放置判据，**不参与地形生成**）。
    TerrainClimateParams climate {};

    /// **默认参数**：地貌分区**关闭** ⇒ 输出与引入本层之前逐位一致（既有测试与既有世界不受影响）。
    [[nodiscard]] static TerrainGenerationParams Default() noexcept { return {}; }

    /// 从 TOML 加载并校验；文件缺失 / 语法错 / 取值非法一律抛 `std::runtime_error`（**禁止静默回退**，ADR 0005）。
    [[nodiscard]] static TerrainGenerationParams LoadFromFile(const std::filesystem::path& path);
};

/// **地形生成参数的内容哈希**（世界定义一致性；[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策六）。
///
/// 口径：决定"**未改动的地形数据重建后是否仍与原档一致**"。因此**只覆盖参与地形生成的字段** ——
/// 三层噪声（频率 / 幅度）、`heightOffsetBlocks`、`variationFrequency`、地貌分区全部字段、悬垂 / 洞穴 / 河流；
/// **排除 `climate`**（其注释明示"**不参与任何地形生成**"、只供内容放置判据）⇒ **改气候不会误判为生成不一致**。
/// **纯函数、确定性**：逐字段规范编码（**不** hash 结构体裸字节 —— 有填充字节 ⇒ 不确定）；同输入必得同值（红线 7）。
[[nodiscard]] std::uint64_t TerrainParamsContentHash(const TerrainGenerationParams& params) noexcept;

/// 按掩罩值判类（用于统计 / 内容，纯函数）。
/// 判据：`m < hillsStart + blend/2` ⇒ 平原；`m < mountainsStart + blend/2` ⇒ 丘陵；否则山川。
[[nodiscard]] LandformKind ClassifyLandform(float mask, const TerrainLandformParams& params) noexcept;

/// 由掩罩值求**幅度倍数与基线平移**（纯函数、连续、单调不减）。
/// `params.enabled == false` ⇒ 恒返回 `{1.0F, 0.0F}`。
[[nodiscard]] LandformModulation EvaluateLandformModulation(float mask,
                                                            const TerrainLandformParams& params) noexcept;

}  // namespace vx
