#pragma once

#include <filesystem>

namespace vx {

/// 塌落规则参数（T29）：来自 `assets/config/collapse.toml`，是"破坏后支撑失效"行为的唯一事实来源。
///
/// 设计意图与备选方案见 [ADR 0012](../../docs/adr/0012-collision-takeover-by-volumes.md) 第二节；
/// 单位口径：长度 = 格（= 体素边长）。
struct CollapseSpec {
    bool  enabled                 = true;  ///< 是否启用塌落
    float maxCantileverBlocks     = 4.0F;  ///< 悬挑上限（格），必须 ≥ 0
    int   pileSpreadBlocks        = 1;     ///< 落点碎堆的摊开幅度（格），必须 ∈ [0, 2]
    int   neighborhoodMarginBlocks = 0;    ///< 在"派生邻域"之外再额外外扩的块数（1 块 = 32 格），必须 ∈ [0, 4]
};

/// 塌落规则表：启动期从 `assets/config/collapse.toml` 一次性读入（ADR 0005）。
///
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（口径与材质表 / 光照表 / 笔刷表 / 弹丸表一致）。
class CollapseTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

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
