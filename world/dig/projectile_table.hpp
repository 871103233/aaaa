#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 一种弹丸的规格（T27）：**弹道 + 命中后的地形破坏 + 外观**，全部来自
/// `assets/config/projectiles.toml` 的一条 `[[projectile]]`。
///
/// 为什么把"弹道"和"破坏"放同一张表：一枚弹丸的命中效果是它的属性之一，拆成两份表会诱导出现
/// "参数在 A、效果在 B"的漂移（口径同 `brush.toml` 的唯一事实来源原则）。表中的形状参数与
/// `brush.toml` 的爆破笔刷是**两套独立能力**，互不引用、互不覆盖。
///
/// 单位口径：长度 = 格；速度 = 格 / 秒；时间 = 秒；`gravity_scale` 为**角色重力的倍数**；
/// `emissive` 是**线性光**颜色（可 > 1，交给 HDR 通路发光）。
struct ProjectileSpec {
    std::string id;  ///< 类型标识（当前只有 `light_orb`；`[[projectile]]` 数组留出多类型扩展）
    float radius                 = 0.6F;   ///< 渲染半径（格），必须 > 0
    float speed                  = 42.0F;  ///< 初速（格/秒），必须 > 0
    float gravityScale           = 1.0F;   ///< 重力系数（× 角色重力），必须 ≥ 0（0 = 无重力弹道）
    float lifetimeSeconds        = 6.0F;   ///< 存活时间（秒），必须 > 0
    float fireIntervalSeconds    = 0.30F;  ///< 连发冷却（秒），必须 ≥ 0
    /// 爆炸半径（格）：体积挖除半径 / 地表坑半径，必须 > 0。
    /// **T71（2026-09-29）**：`6.0 → 4.0`（性能；见 `assets/config/projectiles.toml` 该字段的注释与
    /// `docs/devlog.md` 的 T71 条目）。本默认值必须与仓库内 `projectiles.toml` **一致**（有单测钉住：
    /// `ProjectileTable.LoadsShippedTableAndRejectsMissingFile`）。
    float explosionRadiusBlocks  = 4.0F;
    float explosionDepthBlocks   = 6.0F;   ///< **地表**爆破的坑心下挖深度（格），必须 > 0
    float explosionRimBlocks     = 2.0F;   ///< **地表**爆破的外环隆起（格），必须 ≥ 0
    float explosionFalloff       = 0.6F;   ///< 衰减带占半径的比例，必须 ∈ (0, 1]
    /// **伤害**（点，必须 ≥ 0）—— T31 / [ADR 0013](../../docs/adr/0013-destructible-elements.md)：
    /// 命中时的破坏预算是 `damage × destruction.toml 的 points_per_cubic_block`（点），
    /// 由 `DigVolumeWorld::CarveByDamage` **逐格³ 扣减**该格材质的 `toughness`。
    /// 地表爆破路径（区域外）仍按半径剖面（见 ADR 0013 §二.5 的范围界定）。
    float damage                 = 10.0F;
    float emissiveRgb[3] = { 1.60F, 1.25F, 0.65F };  ///< 自发光颜色（线性光），每通道 ≥ 0
};

/// 弹丸规格表：启动期从 `assets/config/projectiles.toml` 一次性读入（ADR 0005）。
///
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符 / 没有条目）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（口径与材质表 / 光照表 / 笔刷表一致）。
class ProjectileTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    /// 1：首版（T27，光球）。
    /// 2：每条 `[[projectile]]` 新增**必填**的 `damage`（T31 / [ADR 0013](../../docs/adr/0013-destructible-elements.md)
    ///    的伤害点数）。**必填 ⇒ 破坏性变更 ⇒ 升版**。
    static constexpr int kSchemaVersion = 2;

    /// 同时存在的弹丸数上限（`max_active` 的合法上界；池容量按此分配，运行期不再分配）。
    static constexpr int kMaxActiveLimit = 64;

    /// 从 TOML 装载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    [[nodiscard]] static ProjectileTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试使用。**不是** `LoadFromFile` 失败时的回退路径。
    [[nodiscard]] static ProjectileTable Default();

    [[nodiscard]] const std::vector<ProjectileSpec>& Projectiles() const noexcept { return m_projectiles; }

    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

    /// 同时存在的弹丸数上限（≥ 1）。
    [[nodiscard]] int MaxActive() const noexcept { return m_maxActive; }

    /// 默认发射的类型（当前 = 表中第一项；将来做多类型选择时由 `game/` 决定选谁）。
    /// 前置条件：表非空（`LoadFromFile` / `Default` 都保证至少一项）。
    [[nodiscard]] const ProjectileSpec& DefaultProjectile() const noexcept { return m_projectiles.front(); }

private:
    std::vector<ProjectileSpec> m_projectiles;
    int                         m_schemaVersion = kSchemaVersion;
    int                         m_maxActive     = 16;
};

}  // namespace vx
