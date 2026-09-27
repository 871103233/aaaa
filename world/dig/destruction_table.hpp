#pragma once

#include <filesystem>

namespace vx {

/// 全局**破坏表**（T31 / [ADR 0013](../../docs/adr/0013-destructible-elements.md) §四）：
/// 伤害预算的**换算系数**（手感旋钮）与器物参数（占位，待层 ③）。
///
/// 单位口径：`damage`（点，弹丸表）× `pointsPerCubicBlock` ⇒ **预算（整数点）**；
/// 每破坏一格³ 消耗该格材质的 `toughness`（点/格³，材质表）。
struct DestructionSpec {
    /// 换算系数（必须 > 0）：`预算(点) = damage × pointsPerCubicBlock`。
    /// **手感锚点**：271 ⇒ 伤害 10 的光球在纯泥土（toughness 3）中挖出 r ≈ 6 格、纯岩石（5）r ≈ 5.06 格。
    float pointsPerCubicBlock = 271.0F;

    /// **器物**破坏阈值（点，必须 ≥ 0）：累计伤害 ≥ 本值 ⇒ `Intact → Broken`；0 = 单发命中即破。
    ///
    /// **待消费**：固定器物属 [ADR 0004](../../docs/adr/0004-hybrid-layered-world-representation.md) 的
    /// **层 ③（物件与建造层）**，尚未建 ⇒ 本字段当前只做**解析 + 校验 + 启动日志**（见阶段计划 T31 的
    /// 登记范围）。先落进配置表是为了避免器物落地时再升一次 `schema_version`。
    float propDamageThreshold = 0.0F;

    /// 器物被破坏后的**占位表现色**（线性 RGB，各通道 ∈ [0, 1]）；同上，**待层 ③ 消费**。
    float propBrokenTint[3] = { 0.05F, 0.05F, 0.05F };
};

/// 破坏表：启动期从 `assets/config/destruction.toml` 一次性读入（ADR 0005 口径）。
///
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（与材质表 / 弹丸表 / 塌落表同口径）。
class DestructionTable {
public:
    /// 当前表格式版本。
    static constexpr int kSchemaVersion = 1;

    [[nodiscard]] static DestructionTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试使用。**不是** `LoadFromFile` 失败时的回退路径。
    [[nodiscard]] static DestructionTable Default();

    [[nodiscard]] const DestructionSpec& Spec() const noexcept { return m_spec; }
    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

private:
    DestructionSpec m_spec;
    int             m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
