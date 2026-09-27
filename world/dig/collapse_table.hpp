#pragma once

#include <filesystem>

namespace vx {

/// 塌落规则参数（T29 / T33）：来自 `assets/config/collapse.toml`，是"破坏后支撑失效"行为的唯一事实来源。
///
/// 设计意图与备选方案见 [ADR 0015](../../docs/adr/0015-structure-units-and-rigid-collapse.md)
/// （2026-09-27 修订：**统一连通分量刚体化**，删除逐列下落）；
/// **scope 由 [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一定案**
/// （T49：支撑求解的 scope = "以被改动采样为起点的实心**连通域**"，固定窗口只在超上限时作为**回退**）；
/// 单位口径：长度 = 格（= 体素边长）、质量 = 任意一致的"格³"质量单位、角度 = 弧度。
struct CollapseSpec {
    bool  enabled                 = true;  ///< 是否启用塌落
    float maxCantileverBlocks     = 4.0F;  ///< 悬挑上限（格），必须 ≥ 0
    /// **起始**窗口在"派生邻域"之外再额外外扩的块数（1 块 = 32 格），必须 ∈ [0, 4]。
    ///
    /// T49 起它只决定**起始**窗口：连通域触到窗口边界时窗口会**只向那一侧翻倍扩张**，
    /// 直到把连通域包住（或撞上 `kMaxRegionSamples` 8M 的**累计**上限 ⇒ 告警 + 回退到这个固定窗口）。
    int   neighborhoodMarginBlocks = 0;

    // ---- T33 / T43：刚体化倒塌 ----
    //
    // **质量 / 摩擦 / 弹性不在这里**：它们由 `assets/config/materials.toml` 的**材质物理参数**决定
    // （T43 / [ADR 0016](../../docs/adr/0016-collapse-realism-impulse-material-debris.md)：石 ≠ 土 ≠ 草 ≠ 沙）。
    // 曾经的全局 `mass_per_voxel` / `friction` 已删除 —— 避免两处事实来源漂移。

    float settleLinearSpeed  = 0.6F;   ///< 落定判据：线速度阈值（格/秒，> 0）
    float settleAngularSpeed = 0.7F;   ///< 落定判据：角速度阈值（rad/s，> 0）
    int   settleSteps        = 12;     ///< 落定判据：连续多少个固定步都低于阈值才算静止，必须 ≥ 1
    /// 初始角速度（rad/s，≥ 0）：**仅在无爆心冲量时**使用（人工不对称，否则细长塔只会原地垂直落）。
    /// 有冲量时（`impulseSpeed > 0` 且种子带爆心）冲量是真实的不对称来源，此值不叠加。
    float initialTiltSpeed   = 0.9F;
    /// **爆心冲量速度**（格/秒，≥ 0；0 = 关闭 T43 的冲量）：爆心处的体素获得的速度上限，
    /// 按 `1 − 距离/半径` 线性衰减（见 ADR 0016 决策一）。
    float impulseSpeed       = 8.0F;
    /// **小碎片清除阈值**（体素数，≥ 0；0 = 关闭清除）：失去支撑的分量若体素数 ≤ 它 ⇒ **直接清除**
    /// （"当炸没了"）；含 `indestructible` 材质的分量**不删**（ADR 0016 决策三）。
    int   debrisDeleteMaxVoxels = 24;
    int   maxActiveUnits     = 4;      ///< **每次爆炸最多抽出几个整体**（必须 ≥ 1）；渲染网格池另有槽位（T46 起 16 槽）
};

/// 塌落规则表：启动期从 `assets/config/collapse.toml` 一次性读入（ADR 0005）。
///
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（口径与材质表 / 光照表 / 笔刷表 / 弹丸表一致）。
class CollapseTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    /// v2（T33，2026-09-27）：删除 `pile_spread_blocks`，新增刚体化倒塌参数（质量 / 摩擦 / 落定阈值 / 初始不对称 / 活跃上限）。
    /// v3（T43，2026-09-27）：**删除** `mass_per_voxel` / `friction`（改由材质表驱动），新增 `impulse_speed` / `debris_delete_max_voxels`。
    static constexpr int kSchemaVersion = 3;

    /// 悬挑上限的合法上界（格）：再大就等于"什么都不塌"，失去意义。
    static constexpr float kMaxCantileverLimit = 32.0F;

    /// 从 TOML 装载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    [[nodiscard]] static CollapseTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试使用。**不是** `LoadFromFile` 失败时的回退路径。
    [[nodiscard]] static CollapseTable Default();

    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

    [[nodiscard]] const CollapseSpec& Spec() const noexcept { return m_spec; }

private:
    CollapseSpec m_spec;
    int          m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
