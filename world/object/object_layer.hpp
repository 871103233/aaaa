#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vx {

/// 静态资产的**内容形态**（V0 只有程序化代形，避免依赖尚未就位的美术资源）。
///
/// 说明：本模块是"物件层"（ADR 0004 **层③「物件 / 建造」**）的**机制侧**；
/// 这些"形态"是**内容侧**的最小占位，正式资产（glTF 模型 + 材质 + LOD）由后续阶段扩展。
enum class ObjectAssetKind : std::uint8_t {
    DirtPile,  ///< 土堆（锥形隆起）—— V6「可破坏土堆」的第一个内容物
    Stone,     ///< 石块
    Crate,     ///< 木箱
    /// **传送门**（V3）：立起来的门环；**必须**带 `ObjectPlacement::targetWorldId`（走近按 `E` 切到目标世界）。
    /// **占位形态**（无 kit 美术资产）；"门叫什么 / 为何能传送"属世界观设定，待所有者提供（SKILL 六.9）。
    Portal,
    /// **外部模型**（V8）：从 GLB 文件载入的**静态资产**（树 / 灌木 / 岩石 / 营地小道具……）。
    /// **必须**带 `ObjectType::modelFile`；几何由 `BuildObjectMeshFromModel` 生成（等比装进 `half_extent` 的盒、底面贴地）。
    /// **注意**：本阶段只渲染**几何**，模型自带的贴图 / UV **不出**（渲染器只有地表 4 槽材质）—— 见 `plans/v0.5.md` §1.9。
    Model,
};

/// 物件类型（配置表 `[[type]]`）：静态资产的"模板"。
struct ObjectType {
    std::string     id;                    ///< 唯一标识（同一配置表内不可重复）
    ObjectAssetKind kind = ObjectAssetKind::Stone;
    /// 半尺寸（米），三个分量都必须 > 0。
    ///
    /// - **程序化形态**（土堆 / 石块 / 木箱 / 传送门）：几何**正好**装进 `2*half_extent` 的盒（底面 `y = 0`）。
    /// - **`Model` 形态**（V8）：**目标包围盒半尺寸** —— 载入的模型**等比缩放"装进"该盒**（保持长宽比、不拉伸），
    ///   底面贴 `y = 0`、水平居中。因此同一模型配不同 `half_extent` 即得到不同大小的实例。
    float           halfExtentX = 0.5f;
    float           halfExtentY = 0.5f;
    float           halfExtentZ = 0.5f;
    bool            destructible = false;  ///< 是否可破坏（ADR 0013 器物模型；V0 仅登记，破坏在 V6）

    /// **外部模型文件**（V8，配置 `model_file`；仓库相对路径，如 `assets/models/nature/tree_default.glb`）。
    ///
    /// 规则（加载时校验，非法即抛）：**`kind == Model` ⇒ 必填且非空**；**其它形态 ⇒ 必须为空**
    /// （与 `ObjectPlacement::targetWorldId` 同一口径：写了却不生效 = 静默配置，一律拒绝）。
    std::string modelFile;

    /// **逐顶点材质槽位覆盖**（V8，配置 `material_slot`；`0` 草 / `1` 土 / `2` 岩 / `3` 沙）。
    ///
    /// 语义：`Model` 形态**没有**程序化形态那样的"形态 → 槽位"映射（`ObjectMaterialSlot`），
    /// 因此外观由本字段给出 —— 树给草槽、岩石给岩槽、营地小道具给土槽（模型自带贴图本阶段不出，见 §1.9）。
    /// 规则：**仅 `Model` 可给**；`-1` = 未指定 ⇒ 取 `0`（草）；非 `Model` 给出（≠ -1）⇒ 非法即抛。
    int materialSlot = -1;

    /// **仓库 / 类别**（阶段 V0.5 的 E3，配置 `category`；**可选**，缺省 `misc`）。
    ///
    /// 用途：`F2` 物件选择器的**一级列表**（按类别分组；顺序 = 配置中**首次出现**顺序，确定性）。
    /// 值域固定（`IsValidObjectCategory`，非法即抛）：`vegetation` / `rock` / `prop` / `building` / `portal` / `misc`。
    /// **不按文件名 / 形态推断**（显式字段才可校验、可判定，见 [ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策二）。
    std::string category = "misc";
};

/// 一条放置（配置表 `[[placement]]`）：把某个类型摆到世界坐标。
///
/// `x/y/z` 为物件的**底面中心**（世界坐标，米）——即"摆在地面上的落点"；
/// 竖直摆放（把底面贴到地表）由调用方按地表高度求解。
struct ObjectPlacement {
    std::string typeId;
    float       x = 0.0f;
    float       y = 0.0f;
    float       z = 0.0f;
    float       yawDegrees = 0.0f;  ///< 绕 Y 轴朝向（度）

    /// **传送门的目标世界 id**（V3，配置 `target_world`）。
    ///
    /// 规则（加载时校验，非法即抛）：**`typeId` 指向的类型是 `Portal` ⇒ 必填且非空**；
    /// **其它类型 ⇒ 必须为空**（避免"写了却不生效"的静默配置）。
    /// 是否**注册过**该世界由游戏层（`WorldManager`）判定 —— 物件层不认识世界注册表。
    std::string targetWorldId;

    /// **门的显示名**（V3c，配置 `portal_name`；**可选**）。
    ///
    /// 规则（加载时校验，非法即抛）：**仅 `Portal` 可给**；**其它类型 ⇒ 必须为空**；
    /// 给出**空串视为"未给出"**（UI 层取缺省名「神秘传送门」）。
    /// 命名口径来自所有者（2026-10-06："先用**直白的名字**占用，措辞后续我自己补"）⇒
    /// 措辞由所有者在配置里改，**不在代码里硬编码**（SKILL 六.9）。
    std::string portalName;
};

/// 物件实体（运行期实例）：类型 + 变换 + 器物状态。
struct ObjectInstance {
    std::uint32_t     id = 0;          ///< 稳定 id（从 1 递增；0 表示无效）
    const ObjectType* type = nullptr;  ///< 指向类型表条目（生命周期 = 所属 `ObjectTable`）
    float             x = 0.0f;        ///< 底面中心（世界坐标，米），与 `ObjectPlacement` 同口径
    float             y = 0.0f;
    float             z = 0.0f;
    float             yawDegrees = 0.0f;
    bool              intact = true;   ///< 器物状态（V0 恒为 true；破坏在 V6）
};

/// 一条**程序化散布**（V8，配置 `[[scatter]]`）：在圆域内按**确定性抖动网格**摆放若干**同类型**物件。
///
/// 为什么需要它（而不是手写几百条 `[[placement]]`）：A 世界要"充实"成片植被 / 岩石，逐条手写坐标既冗长又易错；
/// 散布由**纯函数** `PlanObjectScatter`（`world/object/object_scatter.hpp`）按 `seed` 生成 ⇒
/// **同种子逐位可复现**（红线 7），且用**分块抖动网格**（红线 15）天然保证**最小间距**（不重叠堆叠）。
///
/// 落点求解：散布只给**平面点 + 朝向**；竖直方向（底面 Y）仍由调用方按**地表高度**求解（与 `[[placement]]` 同口径）。
struct ObjectScatter {
    std::string   typeId;            ///< 引用 `ObjectTable::types` 里的 id（必须存在）
    float         centerX = 0.0f;    ///< 圆域中心（世界列坐标，格）
    float         centerZ = 0.0f;
    float         radius  = 0.0f;    ///< 圆域半径（格），必须 > 0
    int           count   = 0;       ///< 目标个数，必须 > 0
    std::uint64_t seed    = 0;       ///< 确定性种子（同种子 ⇒ 同一布局）
};

/// **一条删除项**（阶段 V0.5 的 E3，可编辑层配置 `[[remove]]`）：表达"删掉发布清单里的某个落点"。
///
/// 匹配口径（[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策五）：**同 `typeId` 且平面距离 ≤ `tolerance`**。
/// **已知限制**：同类型同位置的多个落点**无法区分**（登记为该 ADR 的后果）；后续若要精确，改为给落点分配稳定 id。
struct ObjectRemoval {
    std::string typeId;            ///< 引用类型表里的 id（必须存在）
    float       x = 0.0F;          ///< 目标落点的平面坐标（格）
    float       z = 0.0F;
    float       tolerance = 0.5F;  ///< 匹配容差（格，> 0）
};

/// 物件配置：**类型表 + 放置清单**，来自同一个 TOML（`assets/config/objects.toml`）。
///
/// 加载失败（文件缺失 / 语法错 / 字段缺失 / 取值非法 / `id` 重复 / `placement` 引用不存在的类型）
/// 一律**抛 `std::runtime_error`** 并中止启动，**禁止**静默回退（与 `MapPreset` 同一口径，ADR 0005）。
struct ObjectTable {
    /// 当前文件格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    int schemaVersion = kSchemaVersion;

    /// **物件可破坏能力总开关**（配置 `destructible_enabled`，**缺省 `true`**）。
    ///
    /// 语义（[`plans/v0.5.md`](../../docs/plans/v0.5.md) §1.5；SKILL「已实现能力只允许配置项关闭」）：
    ///   - `true`  ⇒ 爆炸命中时按**每个类型的** `destructible` 分流（可破坏者被摧毁、不可破坏者只被炸飞）；
    ///   - `false` ⇒ **任何物件都不被摧毁**（一律只被炸飞 / 失支撑掉落）。
    /// 为什么需要它：让"物件破坏"这一已实现能力**可被一个配置项关掉**（而不是删代码或改代码）。
    bool destructibleEnabled = true;

    std::vector<ObjectType>      types;       ///< 按文件顺序（确定性）
    std::vector<ObjectPlacement> placements;  ///< 按文件顺序（确定性）
    std::vector<ObjectScatter>   scatters;    ///< 按文件顺序（确定性；V8）
    std::vector<ObjectRemoval>   removals;    ///< 按文件顺序（确定性；E3：可编辑层的删除项）

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`。
    [[nodiscard]] static ObjectTable LoadFromFile(const std::filesystem::path& path);

    /// 从 **TOML 可编辑层文件**（阶段 V0.5 的 E1）加载并校验（`base` = 该世界的**发布清单**）。
    ///
    /// 与 `LoadFromFile` 的差异 —— 可编辑层**只放落点**：
    ///   - `[[type]]` **可选**（省略 = 只放落点；给出则**新增类型**，但**不得与 `base` 重复**）；
    ///   - `[[placement]]` / `[[scatter]]` 引用的类型必须存在于 `base` **或**本文件新增的类型中；
    ///   - `destructible_enabled` 若给出，**必须与 `base` 一致**（编辑层不得改变破坏总开关）。
    /// 失败抛 `std::runtime_error`。加载顺序 = 发布清单 → 本层（见 `plans/v0.5.md` §1.18）。
    [[nodiscard]] static ObjectTable LoadOverlayFromFile(const std::filesystem::path& path, const ObjectTable& base);

    /// 按 `id` 查类型；不存在返回 `nullptr`。
    [[nodiscard]] const ObjectType* Find(const std::string& id) const noexcept;
};

/// 把**可编辑层**（`overlay`，手工摆放的落点）**叠加**到**发布清单**（`base`）之上（阶段 V0.5 的 E1）。
///
/// 语义（确定性，红线 7）：
///   - 加载顺序 = **发布清单 → 应用 `overlay.removals` → 追加 `overlay.placements` → 追加 `overlay.scatters`**
///     （[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策五）：先按删除项过滤 `base` 的落点
///     （`RemovePlacementsByRemoval`），再**按文件顺序追加**本层条目（不排序、不合并同 id 的条目）；
///   - `overlay` 的类型 id 与 `base` **重复 ⇒ 抛**（同一类型在两处定义 = 静默歧义，必须由作者消歧）；
///   - `destructible_enabled` 两表**必须一致**（编辑层不得悄悄改变破坏总开关）⇒ 不一致即抛。
/// 为什么需要：可编辑层是"自己摆放"的落点，必须与**发布清单分开存、叠加读**（ADR 0028 的"A 不入库"口径）。
[[nodiscard]] ObjectTable MergeObjectTables(const ObjectTable& base, const ObjectTable& overlay);

/// 类别值域校验（`ObjectType.category`，[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策二）。
[[nodiscard]] bool IsValidObjectCategory(const std::string& category) noexcept;

/// 应用**删除项**：返回过滤后的落点 —— 同 `typeId` 且**平面距离 ≤ `tolerance`** 的落点被剔除。
///
/// **纯函数、确定性**（保持原顺序）；用于"发布清单 → 应用编辑层 `[[remove]]` → 追加 `[[placement]]`"。
[[nodiscard]] std::vector<ObjectPlacement> RemovePlacementsByRemoval(const std::vector<ObjectPlacement>& placements,
                                                                    const std::vector<ObjectRemoval>& removals);

/// **物件层**：持有世界中的"物件实体"（**EnTT 注册表**，见 ADR 0003 / 0004 层③）。
///
/// 硬约束（ADR 0004 硬约束 1）：物件的几何与状态**绝不写入地形场**（高度场 / 体积密度场）——
/// 它是独立的层③实体；因此本模块只依赖引擎的 ECS，不触碰地形数据。
///
/// 公共头**不暴露 EnTT 类型**（PIMPL），与"公共头不泄漏第三方类型"口径一致（同 Jolt 做法）。
///
/// 确定性（红线 7）：`ForEach` 的遍历顺序恒为**放置顺序**。
class ObjectLayer {
public:
    ObjectLayer();
    ~ObjectLayer();
    ObjectLayer(const ObjectLayer&)            = delete;
    ObjectLayer& operator=(const ObjectLayer&) = delete;
    ObjectLayer(ObjectLayer&&) noexcept;
    ObjectLayer& operator=(ObjectLayer&&) noexcept;

    /// 放置一个物件并返回其稳定 id（从 1 递增）。
    /// 前置条件：`placement.typeId` 必须存在于 `table`；否则抛 `std::invalid_argument`。
    std::uint32_t Place(const ObjectTable& table, const ObjectPlacement& placement);

    /// 移除物件；不存在返回 `false`。
    bool Remove(std::uint32_t id) noexcept;

    /// 按 id 取实例；不存在返回 `false`（`out` 不被修改）。
    [[nodiscard]] bool Get(std::uint32_t id, ObjectInstance& out) const;

    /// 按**放置顺序**遍历所有物件（确定性）。
    void ForEach(const std::function<void(const ObjectInstance&)>& fn) const;

    [[nodiscard]] std::size_t Count() const noexcept;

    /// 清空所有物件并把 id 计数重置为 1（供世界切换使用，见 `plans/v0.5.md` V2）。
    void Clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vx
