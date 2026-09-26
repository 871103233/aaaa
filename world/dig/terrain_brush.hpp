#pragma once

#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace vx {

class TerrainWorld;

/// 一次笔刷操作的姿态：水平圆心 + 半径（格）。
///
/// 语义：笔刷是**竖直轴与地表列对齐的球体**，其水平投影为一个圆盘；落在圆盘内的列被整体
/// 抬升 / 下沉一个固定增量。采用"半径内恒定增量"是为了让受影响列集合可精确预测、可测试；
/// 球体的起伏由地表本身的高低体现。
struct BrushPose {
    float centerX = 0.0F;  ///< 圆心世界 X（格）
    float centerZ = 0.0F;  ///< 圆心世界 Z（格）
    float radius  = 0.0F;  ///< 半径（格）；`<= 0` 时不做任何事
};

/// 一次笔刷操作的结果。
struct BrushResult {
    /// 高度**实际发生变化**的列数（因钳制到垂直范围而未变化的列不计入）。
    std::size_t changedColumns = 0;

    /// 受影响（需要重网格）的 tile 集合，已去重并按坐标升序排列。
    std::vector<TileCoord> dirtyTiles;
};

/// 在世界的地表上执行一次笔刷挖掘 / 堆建。
///
/// - 只改动圆盘内的列，且**只标记受影响 tile** 为脏（red line：禁止整世界重网格）；
/// - 高度钳制到世界垂直范围 `0 ~ 512` 格（ADR 0008）；
/// - 圆盘触及 tile 共享边界列时，相邻 tile 的边界几何同样改变，因此**一并**标记为脏；
/// - 完全落在单个 tile 内部的笔刷**不会**弄脏邻 tile。
///
/// 前置条件：目标列的 tile 已加载（未加载的列按"不存在"跳过）。
/// `deltaHeightUnits` 为定点增量：负 = 挖，正 = 堆。
[[nodiscard]] BrushResult ApplyTerrainBrush(TerrainWorld& world, const BrushPose& brush, int deltaHeightUnits);

/// 笔刷衰减：把"半径内满强度 → 边缘平滑归零"的隶属度抽成**纯函数**（CPU 侧可单测）。
///
/// 返回 `1 − SmoothStep(r·(1−band), r, distance)`：
///   - `distance = 0` → 1；`distance ≥ radius` → 0；区间内**单调不增**；
///   - 在 `distance = r·(1−band)` 与 `distance = r` 处一阶导为 0 ⇒ **边界一阶连续**（无硬台阶）。
/// `band` = 过渡带占半径的比例；`band ≤ 0` 退化为硬边（`distance < radius` → 1）。
/// 曲线风格与 `world/terrain/material_blender.cpp` 的 `SmoothStep` 一致；不读全局、不分配。
[[nodiscard]] float BrushFalloff(float distance, float radius, float band) noexcept;

/// 平整笔刷的方向：`Fill` 只把低于目标处抬高（填平）；`Shave` 只把高于目标处削低（削平）。
enum class LevelMode {
    Fill,
    Shave,
};

/// 平整笔刷（T26）：半径 `brush.radius` 内每列高度向 `targetHeightBlocks` 收敛。
///
///   `delta = clamp(target − h, ±maxStepBlocks) × BrushFalloff(d / r)`
///
/// 再按 `mode` 只保留单向分量：`Fill` 抬升低于目标处、`Shave` 削低高于目标处，另一侧**不动**
/// （`Shave` 绝不把低处抬起）。为保证收敛，衰减后本步不足 1 个定点单位时仍推进 1 单位（且不过冲目标，
/// 结果精确落在目标上）。`maxStepBlocks` 为本次最大改动（= `strength × dt`，由 `game/` 按固定步长折算）。
///
/// 与 `ApplyTerrainBrush` 同约束：只改动圆盘内的列、**只标脏受影响 tile**、高度钳制到世界垂直范围。
[[nodiscard]] BrushResult ApplyTerrainLevel(TerrainWorld& world, const BrushPose& brush, float targetHeightBlocks,
                                            float maxStepBlocks, float falloffBand, LevelMode mode);

/// 爆破笔刷（T26）：半径 `craterRadiusBlocks` 内下沉、外环隆起，**边界平滑**（无硬台阶）。
///
/// 剖面上的高度改变量 = `rimBlocks · RimProfile(d/R) − depthBlocks · BrushFalloff(d / R)`：
///   - 坑体：`−depthBlocks × BrushFalloff`，中心最深、随 d 平滑收口到 0；
///   - 外环：`+rimBlocks ×` 一个平滑环状剖面（中心与边界均归零、一阶导在两端为 0，
///     峰值位于 `d ≈ craterRadius` 附近，模拟抛土堆）；`rimBlocks = 0` 时无外环。
///   - `R = craterRadiusBlocks`；外环与坑体在 `d = R` 处值与一阶导均归零，故与"半径外不变"无缝衔接。
///
/// 与其它笔刷同约束：只改动圆盘内的列、**只标脏受影响 tile**、高度钳制到世界垂直范围。
[[nodiscard]] BrushResult ApplyTerrainCrater(TerrainWorld& world, const BrushPose& brush, float depthBlocks,
                                             float rimBlocks, float craterRadiusBlocks, float falloffBand);

/// 笔刷配置（T26）：来自 `assets/config/brush.toml`，是 game/ 侧笔刷行为的**唯一事实来源**。
///
/// 单位：半径 / 深度 / 隆起 / 坑半径 = 格；`strength` = 格 / 秒（平整收敛速率）；`falloff` = 过渡带占比。
struct BrushSettings {
    float radius        = 6.0F;  ///< 通用笔刷半径（格），必须 > 0
    float strength      = 6.0F;  ///< 平整收敛速率（格/秒），必须 > 0
    float falloff       = 0.6F;  ///< smoothstep 过渡带占半径的比例，必须 ∈ (0, 1]
    float craterDepth   = 6.0F;  ///< 爆破坑中心下挖深度（格），必须 > 0
    float craterRim     = 2.0F;  ///< 爆破坑外环隆起高度（格），必须 ≥ 0
    float craterRadius  = 8.0F;  ///< 爆破坑半径（格），必须 > 0
};

/// 笔刷配置表：启动期从 `assets/config/brush.toml` 一次性读入（ADR 0005）。
///
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（口径与 `TerrainMaterialTable` / `LightingTable` 一致）。
class BrushTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    [[nodiscard]] static BrushTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试使用。
    /// **不是** `LoadFromFile` 失败时的回退路径 —— 那条路径必须报错。
    [[nodiscard]] static BrushTable Default();

    [[nodiscard]] const BrushSettings& Settings() const noexcept { return m_settings; }
    [[nodiscard]] int                 SchemaVersion() const noexcept { return m_schemaVersion; }

private:
    BrushSettings m_settings;
    int           m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
