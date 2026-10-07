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
    /// **模块化建筑构件**（V0.8；[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md)）：
    /// 人工可进入空间（建筑 / 房间）的**代理几何构件** —— 见 `ObjectKitRole`。
    ///
    /// **必须**带 `ObjectType::kitRole` 与 `moduleBlocks > 0`；几何由 `BuildKitPieceMesh` 生成
    /// （程序化代理体：正式 kit 美术资产由后续阶段引入，见 ADR 0035 决策二）。
    Kit,
};

/// **模块化 kit 的构件角色**（V0.8，配置 `kit_role`；**仅 `kind = "kit"` 可给**）。
///
/// 为什么需要"角色"而不是只给一段几何：构件必须**按统一模数对齐**才能拼成没有错缝的房间
/// （业界参照：UE5 Modular Building Kit + Grid Snapping、Unity ProBuilder、Godot GridMap）；
/// 角色还决定**可进入性**的关键尺寸（只有 `WallDoor` 留净高达标的门洞）。
enum class ObjectKitRole : std::uint8_t {
    Floor,     ///< 地板：有厚度的板，**顶面 = 可站面**（构件局部 `y ∈ [0, 厚度]`）
    Wall,      ///< 墙：占满一个模数格的整块地面投影，几何 = 中间夹一层薄板
    WallDoor,  ///< 带**门洞**的墙：洞口净高 ≥ `kKitDoorClearanceBlocks` ⇒ 角色可通过（可进入性的硬保证）
    Roof,      ///< 屋顶：有厚度的板（与地板同形，语义为"封顶"）
};

/// 门洞的**最小净高**（格）：角色总高 1.80 格 ⇒ 留 0.4 格余量。低于它的门洞被解析期拒绝（非法即抛）。
inline constexpr float kKitDoorClearanceBlocks = 2.2F;

/// 墙板 / 地板 / 屋顶的**默认厚度**（格）与**墙默认高**（格）；由构件几何使用（模数由 `module_blocks` 给出）。
inline constexpr float kKitSlabThicknessBlocks = 0.30F;
inline constexpr float kKitWallHeightBlocks   = 3.00F;
inline constexpr float kKitDoorWidthBlocks    = 1.60F;


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

    /// **构件角色**（V0.8，配置 `kit_role`；**仅 `kind == Kit` 可给**，非法即抛）。
    /// 默认 `Floor` ⇒ 对其它形态**无意义**（解析期会拒绝在非 `Kit` 上给出该字段）。
    ObjectKitRole kitRole = ObjectKitRole::Floor;

    /// **模数**（格，配置 `module_blocks`；**仅 `kind == Kit`** 必填且 `> 0`）。
    ///
    /// 语义：本构件的**水平占地**必须正好覆盖整数个模数格 ⇒ 解析期强制
    /// `2*half_extent.x` 与 `2*half_extent.z` 都是 `module_blocks` 的整数倍（容差 1e-4）。
    /// **为什么强制**：这是"拼起来不出现错缝"的**可判定不变量**（ADR 0035 判据③：同类构件接缝错位 = 0）。
    float moduleBlocks = 0.0F;
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

/// **一条流式（地形感知）散布**（V0.6 C3，配置 `[[scatter_tiled]]`；[ADR 0033](../../docs/adr/0033-world-content-placement-and-streaming.md) 决策五）。
///
/// 与 `ObjectScatter`（**圆域**、局部手工散布）的差异：这是**按 tile 归属**的规则 —— 由 `PlanTileCandidates`
/// 在每个 tile 内生成候选点，再按 `IsPlacementAllowed` 的地形判据过滤；内容随 **tile 常驻窗口**（ADR 0024）
/// **增删**（ADR 0033 决策三）。字段 = `tech-plan-v2.0.md` §3.3 的四项判据（**坡度 / 高度带 / 地貌 / 互斥间距**）。
///
/// 缺省语义：**配置文件不写本段 ⇒ 本表为空 ⇒ 与引入本形态之前逐位一致**。
struct ObjectScatterTiled {
    std::string   typeId;                    ///< 引用 `ObjectTable::types` 里的 id（必须存在）
    std::uint64_t seed = 0;                  ///< 确定性种子（同种子 + 同 tile ⇒ 同一批候选点）

    float cellBlocks      = 16.0F;           ///< 互斥间距：抖动网格步长（格，必须 > 0）⇒ 最小间距 > `0.5 × cell`
    float minSlopeDegrees = 0.0F;            ///< 坡度下界（度，含端点）
    float maxSlopeDegrees = 45.0F;           ///< 坡度上界（度，含端点）
    float minHeightBlocks = 0.0F;            ///< 高度带下界（格，含端点）
    float maxHeightBlocks = 512.0F;          ///< 高度带上界（格，含端点）

    bool allowPlains    = true;  ///< 是否允许落在平原（`landforms` 给出时只放列出的地貌）
    bool allowHills     = true;  ///< 是否允许落在丘陵
    bool allowMountains = true;  ///< 是否允许落在山川

    /// **气候区间**（V0.6 C7，配置 `min_temperature` / `max_temperature` / `min_humidity` / `max_humidity`；
    /// 值域 `[0, 1]`，闭区间）。缺省 = 全区间 ⇒ 不约束（与引入气候判据之前**逐位一致**）。
    float minTemperature = 0.0F;
    float maxTemperature = 1.0F;
    float minHumidity    = 0.0F;
    float maxHumidity    = 1.0F;
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

/// **成套建筑的一个构件**（V0.8，配置 `[[building]].pieces` 的一项）。
///
/// 语义：相对**锚点**的偏移（未旋转，格）+ 附加朝向。构件世界位置 = 锚点（地表高度已解算）+ 旋转(偏移, 锚点 yaw)；
/// 朝向 = 锚点 yaw + 本项 `yaw_deg`。**这是"堆叠"的唯一来源** —— 逐件 `[[placement]]` 的 y 一律按地表求解
/// （见 `game/main.cpp`），因此"墙压在地板上、屋顶压在墙上"只能由成套声明给出相对高度。
struct ObjectBuildingPiece {
    std::string typeId;              ///< 引用 `ObjectTable::types` 里的 id（必须存在；不得是 `Portal`）
    float       offsetX = 0.0F;      ///< 相对锚点的水平偏移（格，未旋转）
    float       offsetY = 0.0F;      ///< 相对**锚点地表**的高度（格）—— 堆叠层高的来源
    float       offsetZ = 0.0F;
    float       yawDegrees = 0.0F;   ///< 相对锚点朝向的附加 yaw（度，绕 +Y）
};

/// **成套建筑的落点处理模式**（V0.9，配置 `landing_mode`；[ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策四）。
///
/// 口径：**逐建筑记录、随保存落进可编辑层**；`Unspecified` = 与 V0.8 逐位一致（保证"模式外逐位不变"）。
/// 四种模式**均已放行**：`Sink` / `FlatOnly` **不改地形**；`Flatten` / `Fill` 会改地形，
/// 由 V0.10 / S5 起实现（[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 解除 ADR 0035 决策五，
/// 且地形改动**有持久化** ⇒ 在**摆放时**按 footprint 改地形，改动随 `.voxr` 落盘）。
enum class ObjectBuildingLandingMode : std::uint8_t {
    Unspecified,  ///< 未给出 ⇒ 锚点 = 地表高度（V0.8 行为：不下沉、不校验）
    Sink,         ///< ② 向下半埋：整体下沉 `kBuildingSinkBlocks`（层高相对偏移不变）
    FlatOnly,     ///< ④ 落地必须平整：footprint 内高差 ≤ `kBuildingFlatToleranceBlocks` 才允许放置
    Flatten,      ///< ① 顺手压平地形：footprint 内**双向**收敛到锚点高度（V0.10 / S5 实现）
    Fill,         ///< ③ 悬空处填充：只抬升 footprint 内低于锚点高度处（V0.10 / S5 实现）
};

/// 落点模式 **② 向下半埋**的下沉量（格）。
inline constexpr float kBuildingSinkBlocks = 0.5F;

/// 落点模式 **④ 落地必须平整** 的 footprint 内**允许高差**（格）。
inline constexpr float kBuildingFlatToleranceBlocks = 0.5F;

/// 落点模式 **① 压平 / ③ 填充地形** 改地形时的**边缘过渡带宽度**（格）。
///
/// 语义：footprint **内**精确压平 / 填充到锚点高度；footprint **外**该宽度内用 smoothstep
/// 把地形**平滑过渡回原样**（距离越远改动越小、外缘归零）。
///
/// **为什么必须有它（V0.10 缺陷修复，2026-10-07）**：`0`（无过渡带的硬边）会在斜坡上留下**近垂直台阶** ——
/// 地表是**单面网格**（主通道 `CULLMODE_BACK`），这类台阶在画面里会露出"看穿"的破口。
/// 这与 [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) §八
/// 原本就写明的"**边界平滑收敛**"一致；业界做法（UE5 Landscape 的 *Flatten* falloff、Valheim 地面平整）同为边缘平滑过渡。
inline constexpr float kBuildingLandingFalloffBlocks = 3.0F;

/// **交互摆放时的缺省落点模式**（V0.9 / ADR 0036 决策四）：**② 向下半埋**（**暂定，待所有者确认**）。
/// 为什么取它：① 不改地形（最保守，不产生持久化副作用）；② 永不拒绝放置（摆放工具不应"点了没反应"）。
/// V0.10 / S5 已放行 ①/③，但**缺省仍不改为 ①/③**（ADR 0037 决策八未改判）。
inline constexpr ObjectBuildingLandingMode kDefaultBuildingLandingMode = ObjectBuildingLandingMode::Sink;

/// **一座成套建筑**（V0.8，配置 `[[building]]`；[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策三）。
///
/// 为什么需要它：ADR 0028 决策二要求"人工可进入空间 = 模块化 kit"；kit 由**多件**拼成、且**竖直方向要堆叠**，
/// 而逐件 `[[placement]]` 的 y 会被地表高度覆盖 ⇒ 必须有"锚点 + 相对偏移"的成套声明。
/// 锚点的地表高度在**加载期**解算一次（`game/main.cpp`），之后所有构件用同一个基准 ⇒ 不会各piece各贴各的地表。
struct ObjectBuilding {
    std::string id;                  ///< 唯一标识（同一配置表内不可重复）
    float       x = 0.0F;            ///< 锚点平面位置（世界列坐标，格）
    float       z = 0.0F;
    float       yawDegrees = 0.0F;   ///< 锚点朝向（度）—— 整座建筑绕 Y 的朝向
    std::vector<ObjectBuildingPiece> pieces;  ///< 构件清单（**非空**；按文件顺序 ⇒ 确定性）

    /// **逐建筑室内变暗覆盖**（V0.9，配置 `interior_darkening`；**可选**，
    /// [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策二）。
    ///
    /// 语义：`-1` = **未给出** ⇒ 用**全局值**（`--interior-darkening=<0~1>`）；`[0, 1]` = 该建筑覆盖
    /// （`1.0` = 完全不调暗）。承载 = 实例缓冲的空闲分量 `enclosureB.y`（**不加宽记录**）。
    /// **不写该字段 ⇒ 与只设全局值时逐位一致**（判据②）。
    /// 非法值（越界 / 非数）⇒ 解析期抛（ADR 0005）。
    float interiorDarkening = -1.0F;

    /// **落点处理模式**（V0.9，配置 `landing_mode`；**可选**，缺省 `Unspecified` = V0.8 行为）。
    /// 让安装 anchor Y 的计算随模式不同：`Sink` ⇒ 下沉 `kBuildingSinkBlocks`；`Unspecified` ⇒ 不下沉。
    /// `FlatOnly` 的"是否平整"需运行时用地形采样判定（见 `ComputeBuildingFootprintXZ`）。
    ObjectBuildingLandingMode landingMode = ObjectBuildingLandingMode::Unspecified;
};

/// **一条成套建筑删除项**（V0.9，可编辑层配置 `[[remove_building]]`；
/// [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策四）。
///
/// 与单件 `[[remove]]` 的差异：**按建筑 `id` 精确匹配** ⇒ 不存在"同类型同位置无法区分"的限制
/// （[ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 的已知限制只适用于单件）。
struct ObjectBuildingRemoval {
    std::string buildingId;  ///< 目标建筑 id（必须非空）
};

/// **一条室内变暗覆盖**（V0.9，可编辑层配置 `[[building_darkening]]`；
/// [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策三）。
///
/// 用途：对**发布清单 / 上一轮编辑层**里已有的建筑**就地改** `interior_darkening`（发布清单只读 ⇒ 差异落在本层）。
struct ObjectBuildingDarkening {
    std::string buildingId;      ///< 目标建筑 id（必须非空、且应存在于合并后的建筑表中）
    float       darkening = 0.0F; ///< 目标值，必须 ∈ [0, 1]
};

struct ObjectTable;  // 前置声明：`ComputeBuildingEnclosure` 只按引用使用它（定义在下方）

/// **一座建筑的「围合体」代理**（V0.8；[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策四）。
///
/// 语义：把建筑的**屋顶构件**并集近似为一个**方盒**（世界 XZ 包围盒 + 屋檐下沿高度），供片元着色器
/// 判定"该片元是否处在室内" ⇒ 是则把**环境项（天空光 / IBL）**按 `kInteriorSkyVisibility` 调暗。
///
/// 为什么需要（而不是逐顶点烘焙 AO）：本阶段 kit 构件是**共享原型**（同一原型被多座建筑复用，见 ADR 0034 实例化），
/// 逐原型烘焙无法区分"墙的内侧 / 外侧"。业界标准做法（UE5 Lightmass / Volumetric Lightmap、Unity Lightmap + Light Probes）
/// 需要**离线烘焙管线** —— 本阶段尚未具备 ⇒ 按「降级必须先问」的口径取**最接近的替代**：
/// 用**解析式围合体**做实例级的天空可见性近似（无烘焙、无新 GPU 资源、零运行期几何）。
/// **已知限制**：非轴对齐朝向（yaw 非 90° 倍数）时包围盒略大于真实footprint；屋檐下沿本身那一层不判定为室内。
struct ObjectEnclosure {
    bool  enabled  = false;  ///< false ⇒ 该建筑没有屋顶构件 ⇒ 不是"可进入空间"，不做室内变暗
    float centerX  = 0.0F;   ///< 围合体中心 XZ（世界列坐标，格）
    float centerZ  = 0.0F;
    float halfX    = 0.0F;   ///< 围合体半尺寸 XZ（格），必须 > 0（否则 `enabled = false`）
    float halfZ    = 0.0F;
    float ceilingY = 0.0F;   ///< **屋檐下沿**的绝对世界高度（格）= 最低屋顶构件的底面高度
    /// **逐建筑变暗覆盖**（V0.9 / [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策二）：
    /// 取自 `ObjectBuilding::interiorDarkening`；`-1` = 用全局值、`[0,1]` = 覆盖。
    float darkening = -1.0F;
};

/// **纯函数、确定性**（红线 7）：由建筑声明 + 类型表 + 锚点地表高度，求该建筑的围合体代理。
///
/// 口径：
///   - 只**统计 `kit_role = "roof"` 的构件**（它们决定"封顶"，也就决定"是不是室内"）；无屋顶 ⇒ `enabled = false`；
///   - 构件水平包围盒 = 其**绕 Y 旋转后的 AABB**（`|cos|·hx + |sin|·hz`，轴对齐时精确）；
///   - `ceilingY` 取**最低**屋顶构件的底面高度（= `anchorSurfaceY + offset.y`，因为屋顶构件的局部底面在 `y = 0`）；
///   - 引用了不存在 / 非 `Kit` 构件的条目**跳过**（加载期 `ParseBuildings` 已保证存在，这里只做兜底）。
///
/// 前置条件：`anchorSurfaceY` = 该建筑锚点的**地表高度**（格），由调用方按地表求解。
[[nodiscard]] ObjectEnclosure ComputeBuildingEnclosure(const ObjectBuilding& building, const ObjectTable& table,
                                                       float anchorSurfaceY) noexcept;

/// **纯函数、确定性**（红线 7）：由建筑声明 + 类型表求该建筑**全部构件**的水平并集 AABB（世界 XZ，格）。
///
/// 与 `ComputeBuildingEnclosure`（只看屋顶）的差异：这是**整座建筑的地面投影**（所有 `kit` 构件参与），
/// 供落点模式 ④（"落地必须平整"）在地形上采样 footprint 内的起伏。
/// 返回 `false` ⇒ 无有效构件（调用方按"无法判定 ⇒ 拒绝 + 提示"处理）。
[[nodiscard]] bool ComputeBuildingFootprintXZ(const ObjectBuilding& building, const ObjectTable& table, float& outMinX,
                                              float& outMaxX, float& outMinZ, float& outMaxZ) noexcept;

/// 物件配置：**类型表 + 放置清单**，来自同一个 TOML（`assets/config/objects.toml`）。
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
    std::vector<ObjectScatterTiled> tiledScatters;  ///< 按文件顺序（确定性；V0.6 C3：`[[scatter_tiled]]` 流式形态）
    std::vector<ObjectRemoval>   removals;    ///< 按文件顺序（确定性；E3：可编辑层的删除项）
    std::vector<ObjectBuilding>  buildings;   ///< 按文件顺序（确定性；V0.8：成套建筑）
    std::vector<ObjectBuildingRemoval>   buildingRemovals;   ///< 按文件顺序（确定性；V0.9：`[[remove_building]]`）
    std::vector<ObjectBuildingDarkening> buildingDarkenings;  ///< 按文件顺序（确定性；V0.9：`[[building_darkening]]`）

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

    /// 按 `id` 查**成套建筑**（V0.9）；不存在返回 `nullptr`。
    [[nodiscard]] const ObjectBuilding* FindBuilding(const std::string& id) const noexcept;
};

/// 把**可编辑层**（`overlay`，手工摆放的落点）**叠加**到**发布清单**（`base`）之上（阶段 V0.5 的 E1）。
///
/// 语义（确定性，红线 7）：
///   - 加载顺序 = **发布清单 → 应用 `overlay.removals` → 追加 `overlay.placements` → 追加 `overlay.scatters` → 追加 `overlay.tiledScatters`**
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
