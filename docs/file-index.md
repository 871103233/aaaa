# 文件目录（File Index）

本文件是仓库结构的**职责索引**，回答"什么该放哪里"。
维护规则见 [开发规范 · 第六节](../.trae/skills/voxel-engine-dev-standards/SKILL.md)。

- **粒度**：目录 + 模块入口（公共头 / `CMakeLists.txt` / 脚本）。实现文件（`.cpp`）与测试用例不逐个登记。
- **更新时机**：任何目录或模块入口发生增删改时，与代码**同一次提交内**更新本文件。
- **最后核对**：2026-10-07（对照 `git ls-files`；阶段 W 期间新增 `world/premade/`、`world/shell/`、`tools/baker/` 与相应测试；阶段 V0.5 的 V0 + V0b + V0c 新增 `world/object/`（`object_layer.*` / `object_mesh.hpp` / `object_support.hpp`）、`assets/config/objects.toml`、`tests/object_layer_test.cpp`、`tests/object_mesh_test.cpp`、`tests/object_support_test.cpp`；V1 新增 `world/generation/level_manifest.{hpp,cpp}`、`assets/maps/world_{a,b,c}.toml` 与 `assets/maps/world_{b,c}_terrain.toml`、`tests/level_manifest_test.cpp`；V2a 新增 `game/world_manager.hpp`、`tests/world_manager_test.cpp`；V2b 为 `game/main.cpp` 的 `for(;;)` 世界装载循环 + 显式卸载（配套新增 `MeshRenderer::MeshSlotCount`、`RigidCollapseRuntime::ReleasePool`）；V3 新增 `game/portal_interaction.hpp`、`assets/maps/world_{a,b,c}_objects.toml`、`tests/portal_interaction_test.cpp`；V8 新增 `world/object/object_scatter.hpp`、`assets/models/nature/*.glb`（脚本取回）、`tools/fetch_assets.ps1` 的 `modelzip` 模式；V2c 新增 `game/world_fingerprint.hpp`、`tests/world_fingerprint_test.cpp`；V4 新增 `world/terrain/terrain_tile_source.hpp`、`world/premade/premade_terrain_source.{hpp,cpp}`、`tools/bake_premade_maps.ps1`、`tests/premade_terrain_source_test.cpp`；V0.6 新增 `world/object/object_placement_rule.hpp`、`world/object/terrain_sampling.hpp`、`assets/maps/world_a_objects.toml` 的 `[[scatter_tiled]]`、`assets/config/terrain.toml` 的 `[climate]`、`tests/{object_placement_rule,object_terrain_sampling,climate}_test.cpp`；V0.7 新增 `engine/render/instance_batch.hpp`、`assets/shaders/{mesh_instanced,shadow_instanced}.vert`、`tests/instance_batch_test.cpp`）

---

## 分层与依赖方向

```
platform  →  engine core  →  voxel world  →  game
```

依赖**只能向右**（下层不 include 上层）。

---

## 目录树

```
voxel-engine/
├── .github/workflows/         CI
├── .trae/skills/              AI 开发规范技能（含门禁脚本）
├── assets/                    运行时资源源文件
│   ├── config/                配置表（材质表 / 可挖区域表 / 生成参数；TOML，见 ADR 0005）
│   ├── maps/                  预设固定地图（TOML：种子 / 范围 / 出生点 / 地形编辑区）
│   ├── shaders/               GLSL 源
│   └── textures/              纹理源
├── cmake/                     自写构建辅助模块
├── docs/                      方案文档 / ADR / 本索引 / 开发记录 / 阶段计划 / 内容基线文档
│   ├── adr/                   架构决策记录 + 决策索引 README
│   └── plans/                 阶段计划（接手者每次开工先读）
├── engine/                    引擎核心层
│   ├── core/                  主循环 / 固定步长 / 计时 / 日志
│   ├── input/                 输入动作状态层（上层只消费动作，不读 SDL 事件）
│   ├── physics/               Jolt 薄封装（生命周期 / 固定步长 / 高度场 / 角色胶囊）
│   ├── platform/              平台抽象（唯一读取 SDL 事件队列处）
│   └── render/                渲染封装
├── game/                      游戏逻辑层（含 ImGui 调试面板、世界切换管理器、物件选择器 / 摆放模式 / 模型预览）
├── tests/                     单元测试
├── third_party/               vendored 单头文件库（FastNoiseLite）
├── tools/                     仓库级工具（不入构建的脚本 + 进构建的烘焙 CLI：fetch_assets / vx_perf_input / vx_stream_regression / baker）
├── world/                     世界层（分层混合，见 ADR 0004 / ADR 0023）
│   ├── dig/                   笔刷挖掘 / 堆建与脏 tile 收集；可挖体积 + 破坏结算 + 倒塌
│   ├── generation/            确定性种子与噪声 + 地形生成参数 / 地貌分区 + 世界清单（LevelManifest）
│   ├── object/                物件层（静态资产实体 + 放置表 + 模型形态 / 程序化散布 + 可编辑层与保存器，ADR 0004 层③）
│   ├── premade/               预制地图容器格式与离线烘焙（ADR 0026）
│   ├── shell/                 地表体积壳（世界表示 v2 层②：洞穴 / 悬垂，ADR 0023）
│   ├── streaming/             常驻调度：可挖体积（ADR 0020）、地表 tile（ADR 0024）、构建任务池（ADR 0022）
│   ├── terrain/               地表高度场 tile / 网格化 / 材质混合 / ITerrainQuery 实现
│   └── water/                 水体（样条河流 + 静态水位 + flow 着色，ADR 0027）
├── CMakeLists.txt             根构建
├── CMakePresets.json          构建预设
├── vcpkg.json                 依赖清单
├── LICENSE / NOTICE.md        许可与第三方清单
└── .clang-format / .clang-tidy / .editorconfig / .gitattributes / .gitignore
```

---

## 仓库级（根目录）

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `CMakeLists.txt` | 根构建：C++17、`/W4` + 警告即错误、`/utf-8`、ASAN/TSan 互斥断言、聚合子目录 | — | 不放业务逻辑 |
| `CMakePresets.json` | 预设 `debug` / `release` / `relwithdebinfo` / `asan` / `tsan` | — | 新增 configure 预设必须**同时**补 `buildPresets` 与 `testPresets`，否则 `ctest --preset` 报 no such preset |
| `vcpkg.json` | 依赖清单 + `builtin-baseline` + 构建期 host 工具 | — | baseline 已锁定；增删依赖需评审，并在提交信息说明原因；**构建期工具（`shaderc` / `sdl3-shadercross`）必须写成 `{ "name": "...", "host": true }`**；端口名一律用 vcpkg 名（`enkits`，不是 `enkiTS`） |
| `.clang-format` / `.clang-tidy` / `.editorconfig` | 风格与静态检查 | — | 与开发规范第三节保持一致 |
| `.gitattributes` | 行尾统一（`eol=lf`）+ LFS 追踪规则 | — | `.ps1` 保持 LF（不强制 CRLF） |
| `.gitignore` | 忽略 `build/`、`vcpkg_installed/`、`*.spv`、IDE 产物、**美术资源目录（`assets/textures/`、`assets/models/`）** | — | 保留 `.trae/skills/`（CI 依赖其中的门禁脚本）；**不得**忽略 `tools/assets.sha256`（它是资源台账的一部分） |
| `LICENSE` / `NOTICE.md` | 许可与第三方组件清单 | — | 引入新第三方库须同步 `NOTICE.md` |

---

## 引擎核心层

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `engine/` | 引擎核心：可复用的通用能力 | 可依赖 `platform` 与第三方 | 不放世界 / 游戏专有类型（`TerrainTile`、`DigVolume`、`Biome` 等），不含游戏内容 |
| `engine/core/` | 主循环装配、固定步长累加器、单调计时、统一日志接口 | 可依赖 `engine/platform` 与第三方 | 不放渲染与世界逻辑 |
| `engine/input/` | 输入动作状态层：按键 / 鼠标 → 动作，每帧采样一次 | 可依赖 `engine/platform` | 上层只消费动作；**本层之外不得读 SDL 事件队列** |
| `engine/physics/` | Jolt 薄封装：生命周期、固定步长推进、通用高度场 / 角色胶囊 / 动态凸包刚体（**公共头不含 Jolt 类型**；`ActivateBody` = T46 唤醒保留残骸；`RayCastDynamic` = T48 只查动态刚体的线段查询，命中点落在**真实凸包表面**并回报句柄；`ConvexHullDesc::rotation` = T50 按当前姿态原地重建刚体用） | 可依赖 `engine/core` 与第三方 | 不放地形专有类型；世界坐标进出须显式转换并注明精度（见待收敛项 7） |
| `engine/platform/` | 平台抽象：窗口、输入、计时、文件 IO、**命令行（UTF-8 取回）与控制台编码**（V0.10 / S10：启动时把控制台输出代码页设为 UTF-8，否则中文日志在 Windows 终端乱码） | 可依赖第三方（SDL3） | 不放渲染与游戏逻辑 |
| `engine/render/` | 渲染封装（RHI 薄层） | 可依赖 `engine/platform` | 不把具体图形 API 语义泄漏到上层 |
| `engine/CMakeLists.txt` | 聚合 `engine/` 源文件为 `voxel_engine` 静态库 | — | 新增源文件须在此登记 |

---

## 世界层

> 分层混合世界（ADR 0004 / `tech-plan-v2.0.md` §2）：地表高度场 + 可挖标记区域内的有界 SDF 体积 + 物件/建造层。
> **旧 `voxel/` 层与 `chunk/chunk_types.hpp` 已随迁移任务 M2 删除**（方块世界表示作废），不得再引用 `voxel/` 路径。

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `world/` | 世界层：生成、地表网格化、材质、挖掘、流式加载、可挖体积、**物件层**、存档 | 可依赖 `engine` | 硬件访问一律经引擎核心 / 平台抽象，**不直接调用平台 API** |
| `world/terrain/` | 地表高度场 tile（64×64、`int16` 1/16 格）、网格化与梯度法线、材质混合、`ITerrainQuery` 实现、**碰撞体采样构建**（`terrain_collision`）。tile 生命周期：`LoadTile`（生成 + 网格）/ **`UnloadTile`（W7-S2：释放高度与网格，返回是否原本常驻）** / `ResidentTiles`（升序）/ `ResidentTileCount`（O(1)）——供流式调度与"常驻量只随窗口变化"的核对 | 可依赖 `engine` | tile 网格须多采样一行/列（65×65），保证相邻 tile 边界**逐位相等、无裂缝**；世界定位用整数 / `double`；**"是否被玩家改过"不由本层记录**（编辑块留驻由流式层按 ADR 0020 决策五判定）。**V4**：新增 `terrain_tile_source.hpp`（`ITerrainTileSource` —— 地表 tile 的**数据来源**抽象：`TerrainWorld::SetTileSource` 与建块任务池共用，`nullptr` = 程序化）；`RefreshTileMaxSurfaceBlocks` 由匿名命名空间提为**公开自由函数**（worker 与主线程共用同一份实现） |
| `world/generation/` | 确定性种子派生与噪声（FastNoiseLite 封装，pimpl 隔离）、**预设固定地图加载**（`map_preset`：种子 / 范围 / 出生点 / 地形编辑区，TOML）、**世界清单**（`level_manifest`：ADR 0028 §一 —— `family` / `source`(+`premade_file`) / **引用 `terrain_preset`** / **`objects_file`（V3：每世界自己的物件 / 传送门放置清单）** / 策略位（破坏 · 持久化 · 换种子）；`LoadFromFile` 校验后**非法即抛**）、**地表生成参数与地貌分区**（`terrain_params.*`：`TerrainGenerationParams` / `TerrainLandformParams` / `ClassifyLandform` / `EvaluateLandformModulation` / `LoadFromFile`；W3）、**悬垂 / 洞穴 / 河流噪声**（`TerrainOverhangParams` / `TerrainCaveParams` / `TerrainRiverParams` + `TerrainNoiseGenerator::OverhangAt` / `CaveCarveAt` / `RiverJitterAt`；W4 / W5 / W6）、**气候**（`TerrainClimateParams` + `TerrainNoiseGenerator::TemperatureAt` / `HumidityAt`，各归一化 `[0,1]`；配置 `terrain.toml` 的 `[climate]`（可选段）；V0.6 C7 —— 只供内容放置判据，**与高度图解耦**） | 可依赖 `engine` | 生成必须是**纯函数**（种子 + 整数坐标）；预设编辑叠加在噪声之上，**同一文件必须得到同一世界**；禁止 `rand()` / 时间 / 线程顺序；`landform.enabled` / `caves.enabled` / `river.enabled` 关闭时**不进入该路径**（与引入前逐位一致）；**清单只声明族与策略、不复制地形字段**（地形字段的唯一事实来源 = 它引用的 `MapPreset`）。**V0.5 E1**：清单增 **`objects_edit_file`**（可选；显式给出却缺失 ⇒ **抛**、缺省**派生**为 `<objects_file 主名>.edit.toml` 却缺失 ⇒ **跳过**）—— 手工摆放的**独立可编辑层**，叠加在发布清单之上、**不写回**（ADR 0028「A 不入库」） |
| `world/dig/` | 地形笔刷：平整填平 / 削平（`Level`）、平滑爆破（`Crater`）、球笔刷挖 / 堆，与脏 tile 收集；**可挖区域标记表**（`dig_region`：ADR 0006 的数据文件部分 + 层间交接过滤器）与**可挖体积**（`dig_volume` + `volume_mesher`：33³ `int8` 密度、Surface Nets 等值面）；**弹丸规格表**（`projectile_table`：弹道 + 爆炸破坏 + 自发光，`[[projectile]]` 数组留多类型扩展）；**破坏表**（`destruction_table`：伤害预算的换算系数，T31 / ADR 0013）与**倒塌规则表**（`collapse_table`，T29 / ADR 0015 / 0018） | 可依赖 `engine` | 只标脏**受影响**的 tile / 体积块；重网格与 GPU 上传不得阻塞主线程；爆破 / 平整剖面**边界一阶连续**；**体积只在标记区域内存在**（ADR 0004 硬约束 2）；体积块与地表网格的交接口径见 ADR 0011 |
| `world/object/` | **物件层**（[ADR 0004](adr/0004-hybrid-layered-world-representation.md) **层③「物件/建造」**；V0.5 的 V0 + V0b）：`object_layer.*` = ① **`ObjectTable`**（类型表 `[[type]]`：`id` / `ObjectAssetKind`（`DirtPile` / `Stone` / `Crate` / **`Portal`**）/ 半尺寸 / `destructible` + 放置清单 `[[placement]]`（含 **`target_world`（V3：门的目标世界 id；仅 `Portal` 必填非空、非 `Portal` 必须为空）**），`LoadFromFile` 校验后**非法即抛**，ADR 0005）；② **`ObjectLayer`**（**EnTT 注册表**，ADR 0003）：`Place` 返回从 1 递增的稳定 id、`Remove` / `Get` / `ForEach`（**遍历 = 放置顺序，确定性**）/ `Count` / `Clear`。`object_mesh.hpp` = **程序化代形**（盒 / 椭球 / 锥 / **立起来的门环**（`Portal`，V3：环面，门洞在环心 ⇒ 可从中间走过），底面中心为原点、法线朝外、无退化三角形）+ `RotateMeshAboutY`（把朝向烘进顶点，供静态碰撞体用）；`object_support.hpp` = **支撑探测**（底面中心 + 四角 × 绕 Y 旋转的探测点 + "任一实心即有支撑"）。配置 `assets/config/objects.toml`（`schema_version = 1`；含**可破坏总开关** `destructible_enabled`） | 可依赖 `engine` 与第三方（EnTT，**只在 `.cpp` 内出现**，PIMPL 隔离） | **几何绝不写进地形场**（ADR 0004 硬约束 1）；公共头**不泄漏 entt 类型**；类型表 / 放置清单**数据驱动、非法即抛**（不静默回退）；渲染与碰撞**共用同一份 `MeshData`**。**未做**：glTF / kit 资产（当前程序化代形）、实例化 / HLOD（当前每实例一份 GPU 网格）、破坏**持久化**（存档未开始）、凹形物件多凸包。**V3**：物件清单**按世界指定**（`LevelManifest.objects_file`；每世界一份 `assets/maps/world_{a,b,c}_objects.toml`），`game/main.cpp` 走近门出 **HUD 提示**、按 `E` 触发切换。**V8**（[ADR 0029](adr/0029-a-world-asset-enrichment-model-and-scatter.md)）：新增形态 **`Model`**（外部 GLB：`ObjectType::modelFile` / `materialSlot`，**仅 `model` 可给、非法即抛**）+ `object_mesh.hpp` 的 **`BuildObjectMeshFromModel`**（等比装进 `2*half_extent` 的盒、底面贴地、水平居中 ⇒ 与程序化形态**共用同一 `MeshData`**）+ **新增** `object_scatter.hpp` 的 **`PlanObjectScatter`**（**程序化散布**：分块抖动网格、有界抖动、确定性洗牌子集 ⇒ 同种子逐位可复现；配置 `[[scatter]]`）。**V0.5 E1/E3**：**新增** `object_edit_save.hpp`（**可编辑层保存器**：写 `schema_version` + 该层全部增量（`[[type]]` / `[[placement]]` / `[[remove]]` / `[[scatter]]`）+ **临时文件 + `rename` 原子替换**，覆盖式 ⇒ 不保留手写注释）+ `ObjectType::category`（选择器分组 / 值域校验）+ `[[remove]]`（删除项）+ `LoadOverlayFromFile` / `MergeObjectTables`（**发布清单 → 应用 `[[remove]]` → 追加 `[[placement]]` → 散布**）/ `RemovePlacementsByRemoval`（纯函数）；**发布清单只读**（编辑结果写独立可编辑层）。**未做**：**逐模型贴图 / UV**（按地表材质槽着色）、实例化 / HLOD。**V0.6（[ADR 0033](adr/0033-world-content-placement-and-streaming.md)）**：新增 **`[[scatter_tiled]]`**（**流式 / 地形感知散布**：`ObjectScatterTiled` = 类型 + 种子 + **互斥网格步长 `cellBlocks`** + 坡度 / 高度带 + `landforms` + **气候区间**；**全部校验、非法即抛**，**传送门类型即抛**）+ **新增** `object_placement_rule.hpp`（`tech-plan §3.3` 四项判据 + 气候：`PlacementRule` / `PlacementSample` / `IsPlacementAllowed` / **`PlanTileCandidates`**（分块确定性候选点））+ **新增** `terrain_sampling.hpp`（`SamplePlacement`：高度 / 地貌 / 温度 / 湿度四回调 + 中心差分坡度）。**V0.11 I1（[ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)）**：**新增** `placement_snap.hpp`（**放置吸附与对齐纯函数** `SnapToStep` / `SnapYawDegrees` + `PlacementFootprint` / `SnapPlacementToNeighbor`（**邻居优先吸附**）+ `PlacementSnapSettings`（`step <= 0` = 关闭即逐位返回））+ **新增** `placement_validation.hpp`（**重叠合法性** `FootprintsOverlap2D`：2D 有向矩形 SAT；**边界接触不算重叠**） |
| `world/streaming/` | **常驻调度（流式）**：① **可挖体积**（V0.2 起，[ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md)）：`dig_volume_residency.*` = 玩家窗口（tile ± K）→ **纯函数集合差**（要建 / 要卸 / 脏块留驻 / 超限淘汰）→ **分帧推进**的建块与卸块（`DigVolumeWorld::CreateBlock` / `UnloadBlock`）；② **地表 tile 常驻策略与调度**（W7-S1 / S2，[ADR 0024](adr/0024-terrain-streaming-and-lod.md)）：`terrain_tile_residency.*` = `TerrainTileWindow` / `TerrainTileRange`（活动 / 常驻含预取环）+ **滞回**推进中心 tile + `PlanTerrainTileResidency`（建 / 卸 / **编辑块留驻** / 超限淘汰；**纯函数**、确定序、范围版 O(窗口)）+ `TerrainTileScheduler`（幂等 `Update` + **分帧 `Step`** 先加载后卸载） | 可依赖 `engine` 与 `world/terrain`、`world/dig` | **不做**主线程同步重活（建 / 卸必须分帧或下沉 worker）；**已改动的（脏 / 编辑）块不得卸载**；地表 tile 的**游戏层接入与 LOD 分环**属 W7-S3（尚未接入），存档 IO 不在此阶段 |
| `world/premade/` | **预制地图**（[ADR 0026](adr/0026-premade-map-format-and-bake-tool.md)）：`premade_map.*` = 容器格式（魔数 + `schema_version` + 世界范围 + 定长索引 + **逐块 zstd** + 按块**随机访问**）；`premade_bake.*` = 离线烘焙库函数（把地图预设的宏地形高度场写成预制文件，**与运行时同源**、逐字节可复现）。**zstd 只在本目录的 `.cpp` 内出现**（公共头不泄漏）。**V4**：新增 `premade_terrain_source.*`（`PremadeTerrainTileSource` = `ITerrainTileSource` 的预制实现：运行期按块读宏高度喂给 `TerrainWorld` / 建块池；缺块 ⇒ false + 一次性 WARN；线程安全） | 可依赖 `engine` 与 `world/generation`、`world/terrain` | 生成 / 烘焙必须**确定性**（红线 7）；非法 / 版本不符 / 块缺失**即抛**（不静默回退） |
| `world/shell/` | **地表体积壳**（世界表示 v2 层②，[ADR 0023](adr/0023-world-representation-v2-hybrid-shell.md)，W4 / W5 / W6）：`surface_shell.*` = 贴着地表的**有界 SDF**（`(y − 宏地表高度) + 悬垂 3D 噪声 × 幅度 × 边界淡出 + 洞穴隧道雕刻量 × 边界/深度淡出 + 河道下切量 × 边界淡出`）+ `SurfaceShellSampler`（`IVolumeSampler`）+ `BuildShellBlockMesh`；复用**与可挖体积同一套** Surface Nets（`world/dig/volume_mesher.*`）。**W7-S3b① 增** `surface_shell_residency.*` = Ring 0 壳区域随玩家流式的**常驻策略层**（纯函数：`MakeSurfaceShellRegion`（tile/block 对齐 + 世界范围钳制）+ `PlanSurfaceShellResidency`（目标集合 / `toLoad` / `toUnload` / **边界淡出圈 `toRebuild`**）+ `SurfaceShellRebuildRingThickness`） | 可依赖 `engine` 与 `world/generation`、`world/dig`、`world/water`（**只前置声明** `RiverCarveField`） | 与运行时**同源**（同一份 `terrain.toml`）；跨块共享面顶点**逐位一致**（无裂缝）、**退化三角形 = 0**；渲染与 `Jolt MeshShape` **共用同一份 `MeshData`**。`caves.enabled == false` / 不传河道场 ⇒ 各自**逐位一致**地退化。**W4~W6 只在近场有界区域铺开**；**W7-S3b① 起策略层与 worker 构建已就绪，游戏层接入（动态区域 / 建卸 / 接管翻转）待做** |
| `world/water/` | **水体（河流）**（[ADR 0027](adr/0027-water-representation.md)，W6）：`river.*` = `RiverPath` / `RiverNode` + `GenerateRiverPath`（多候选源取最长、沿**最陡下降 + 确定性抖动**行进、**水位单调不升**）+ `RiverCarveField`（1 格分辨率下切场 + 双线性查询、河岸线性衰减）+ `BuildRiverWaterMesh`（水面 ribbon） | 可依赖 `engine` 与 `world/generation` | 纯函数、确定性（红线 7）；**不做**流体模拟 / 游泳 / 动态水位（ADR 0027）；河道**只刻进地表壳**（不写宏高度场）⇒ 碰撞随壳自动承担；水面渲染是**独立的** `water.frag` 管线（引擎层不认识"河"） |
| `world/CMakeLists.txt` | 世界层构建目标 | — | 新增源文件 / 子目录须在此登记 |

---

## 游戏逻辑层

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `game/` | 玩法、数值、关卡、UI、AI | 可依赖 `world` / `engine` | 不放通用能力；不被下层引用 |
| `game/main.cpp` | 程序入口：初始化、主循环、组装各层 | — | — |
| `game/world_manager.hpp` | **世界切换管理器**（V2a / [ADR 0028](adr/0028-world-families-and-static-asset-first.md) 决策四）：**清单注册表**（`id → LevelManifest`，重复 id / 非法清单即抛）+ **当前世界**记账 + **切换请求状态机**（`Idle` / `Requested`；`RequestSwitch` 拒绝未知 id / 同世界 / 重复请求并给原因）+ `TakePendingSwitch`（只取出一次，**当前世界不变**）/ `CommitActive`（装载成功后才改名）。**V5 增秘境实例账本**：`EnsureInstance`（首次建 / 之后**复用** ⇒ "反复进入同一世界"）/ `ResetInstance`（销毁换种子 + `generation+1`）/ `FindInstance`（种子由 game 层 roll ⇒ 本类不碰随机数）。**纯逻辑、header-only ⇒ 可单测** | 可依赖 `world`（`LevelManifest`） | **不碰 GPU / 物理 / 世界数据**（真正的卸载 / 装载在 `game/main.cpp` 的装载器，V2b）；拒绝一律**返回 false + 原因**，不静默 |
| `game/portal_interaction.hpp` | **传送门交互**（V3）：`PortalEntry`（门的世界坐标 + 目标世界 id）+ **最近门纯函数** `FindNearestPortal`（半径内取最近；等距取**先出现者** ⇒ 确定性）+ `kPortalPromptRadius`（提示半径）。数据源 = `game/main.cpp` 放置物件时从 `ObjectPlacement.targetWorldId` 与落点收集 | 只依赖 `glm`（**纯函数、header-only ⇒ 可单测**） | 不读全局状态、不分配内存（可热路径调用）；门的**命名 / 为何能传送**属世界观设定（所有者 2026-10-06 已给定：门后 = **秘境**、门 = 「某某传送门」/「神秘传送门」；落地见 V3c） |
| `game/world_fingerprint.hpp` | **世界指纹**（V2c / [ADR 0028](adr/0028-world-families-and-static-asset-first.md) 决策四的验收判据）：`FingerprintBegin` / `FingerprintMixBytes` / `FingerprintMixU64` / `FingerprintTileView` / `ComputeTerrainFingerprint` —— 「**种子 + 升序 tile 的全部 `int16` 高度**」经 **FNV-1a 64 位**混成一个摘要，用于证明"**同种子切回同一世界 ⇒ 逐位相同**"（红线 7） | 无依赖（**纯函数、header-only ⇒ 可单测**） | **顺序敏感**（调用方须传确定的 tile 顺序 = `TerrainWorld::ResidentTiles()` 升序）；**与 LOD 无关**（用全分辨率高度）；不改世界数据 |
| `game/CMakeLists.txt` | 游戏可执行目标 + Shader 构建钩子 | — | 新增 Shader 须在此 `add_shader(...)` |

---

## 测试

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `tests/` | 单元测试（GoogleTest + CTest） | 可依赖所有被测模块 | 只放测试，不放产品代码 |
| `tests/CMakeLists.txt` | 测试目标与 `add_test` 注册 | — | 新增测试文件须在此登记 |
| `tests/fixtures/` | **进仓库的测试夹具**（不放运行时资源）：`skinned_triangle.gltf`（T68 用的最小蒙皮模型：3 顶点 / 1 三角面 / 2 关节 / 1 条旋转动画）+ 其生成脚本 `generate_skinned_triangle.ps1`（纯 ASCII） | — | 与 `assets/models/` 不同：**夹具必须入库**（`.gitignore` 只排除 `assets/models/`）；改动夹具须重跑生成脚本并保持单测绿 |

---

## 资源

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `assets/` | 运行时资源源文件 | — | 生成物放 `assets/generated/`（已忽略）；**美术资源（`assets/textures/`、`assets/models/`）不入库**，由 `tools/fetch_assets.ps1` 取回（见下方两行） |
| `assets/config/` | 配置表：`materials.toml`（地表材质槽与权重规则）、`lighting.toml`（太阳 / 天空光 / 雾 / 阴影 / **环境贴图 `[environment]`（T67，可选段）**）、`brush.toml`（平整 / 削平 / 爆破笔刷参数；**T27 起未绑定按键**）、`dig_regions.toml`（可挖区域标记，ADR 0006）、`projectiles.toml`（弹丸：弹道 + 爆炸破坏 + 自发光，T27）、**`terrain.toml`（地表生成参数 + 地貌分区：山川 / 平原 / 丘陵 + 悬垂 `[overhang]` + 洞穴 `[caves]` + 河流 `[river]`，W3~W6）** 等 | — | 带 `schema_version`；由 toml++ 在**启动期**加载，失败即明确报错（ADR 0005）。**唯一例外**：`dig_regions.toml` 缺失按 ADR 0006 返回空表（不报错） |
| `assets/maps/` | **预设固定地图（地形 / `MapPreset`）** + **世界清单（`LevelManifest`）**：① 地形预设 = 种子 / 覆盖范围 / 出生点 / 地形编辑区（flatten · raise · carve）：`test_range.toml`（1×1 km 手工测试场）、**`world_10km.toml`**（10×10 km 大世界；`tile_radius` 上限已由 W7-S3a 从 8 放到 **78**）、**`world_b_terrain.toml` / `world_c_terrain.toml`**（各 1×1 km，V1）；② **世界清单**（ADR 0028 §一；V1）= `world_a.toml`（10 km 大世界）/ `world_b.toml`（预制 1 km）/ `world_c.toml`（随机 1 km 肉鸽）—— 声明 `family` / `source`(+`premade_file`) / **引用 `terrain_preset`** / **`objects_file`（V3）** / 策略位（破坏 · 持久化 · 换种子）；③ **每世界的物件 / 传送门放置清单**（V3/V8）= `world_a_objects.toml`（**2 门 → B / C** + 1 土堆 + **V8：12 个模型类型 + 10 条 `[[scatter]]` 散布 + 一个小营地**）/ `world_b_objects.toml` / `world_c_objects.toml`（各 **1 门 → A** + 1 土堆）；④ **可编辑层**（V0.5 E1，游戏生成 / 可手工维护）= `world_{a,b,c}.objects.edit.toml` —— 由清单的 **`objects_edit_file`** 指定（缺省派生 `<objects_file 主名>.edit.toml`）；只放**该世界相对发布清单的增量**（`[[type]]` / `[[placement]]` / `[[remove]]` / `[[scatter]]`），加载时**叠加在发布清单之上** | — | 带 `schema_version`；**非法文件必须显式报错，不得静默回退**；同一文件必须得到同一世界；清单的 `terrain_preset` / `premade_file` / **`objects_file`** / **`objects_edit_file`** 按**清单所在目录**解析；**发布清单只读**（游戏只写可编辑层）；**地图不入库的是烘焙产物**（见下方 `assets/maps/**` 行），TOML 定义本身入库 |
| `assets/shaders/` | GLSL 源（`.vert` / `.frag` / `.comp`）：`mesh.*`（地表 / 体积 / 光球）、`mesh_skinned.vert` / `shadow_skinned.vert`（蒙皮）、**`mesh_instanced.vert` / `shadow_instanced.vert`（GPU 实例化；V0.7 H1）**、`shadow.*`、`tonemap.*`、`sky.frag` 与 `ibl_*.frag`（环境 / IBL）、**`water.frag`（水面：flow 滚动波 + 半透明；W6）** | — | 只放源；`.spv` / `.dxil` 由构建生成到 `<build>/assets/shaders/`；新增须在 `game/CMakeLists.txt` 里 `add_shader` |
| `assets/textures/` | **CC0 美术贴图**（**不入库**；T65 起由脚本取回）：`terrain/<材质>/{albedo,normal,roughness,ao}.jpg`（草 / 土 / 岩 / 沙，2048²）、`env/*.hdr`（环境贴图，等距柱状；**T67 起被天空通道与 IBL 烘焙消费**） | — | **被 `.gitignore` 排除** ⇒ 干净克隆后需执行 `tools/fetch_assets.ps1`；来源 / 许可 / SHA-256 逐项登记在 `NOTICE.md`「美术资源台账」；校验和清单 = `tools/assets.sha256`（**该文件进仓库**）。**缺文件时上层必须 WARN + 回落**（贴图 → 程序生成；HDRI → 半球天空光），不得崩、不得静默 |
| `assets/models/` | 3D 模型（**不入库**）：主角与**自然物**。**T69** = `character/Casual_Female.glb`（Quaternius，CC0 占位主角）；**V8** = `nature/*.glb`（**Kenney《Nature Kit》选抽 12 个**低模自然物：树 / 灌木 / 草花 / 岩石 / 营火 / 木栅；CC0） | — | 与 `assets/textures/` 同口径：由 `tools/fetch_assets.ps1` 取回并登记台账；**消费者 = `engine/render/model_loader.*`（T68 落地：glTF/.glb 静态与蒙皮网格 + 骨骼动画采样；T69 接渲染）+ `world/object/object_mesh.hpp` 的 `BuildObjectMeshFromModel`（V8：物件层把 GLB 变成 `MeshData`）** |

---

## 构建辅助 / CI / 文档 / 规范

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `cmake/` | 自写构建辅助模块 | — | 不放业务逻辑；工具缺失时降级为**警告**，不阻断配置 |
| `tools/` | 仓库级工具：**脚本**（不入构建）：**`fetch_assets.ps1`**（取回 CC0 美术资源，幂等 + SHA-256 校验；资源不入库见 `assets/textures/` 行；**V8 增 `modelzip` 模式**：整包 ZIP → 只抽选定 GLB ⇒ `assets/models/nature/`）、**`vx_perf_input.ps1`**（性能冒烟：自动操控键鼠跑固定档 —— `stand`/`rot`/`fly`/`flyfwd`/`walk`/`walkback`/`flybound`/`settle`/`shot`，产 `build/perf/input_<mode>.{out,err}.log` 与一行 `RESULT`；证据口径见 `docs/plans/v0.2.md` §4 与 `docs/plans/v0.3.md` §3。**T83 起入库**，由 `$PSScriptRoot` 推导仓库根）、`assets.sha256`（**进仓库**的校验和清单）、**`bake_premade_maps.ps1`**（V4：按世界清单发现 `source = "premade"` 的世界并**确定性烘焙** `.vxmap`；产物不入库，见 `NOTICE.md`「预制地图烘焙产物台账」）；**`baker/`（`voxel_bake`，阶段 W2-S2b 起**进入构建**）** = 离线烘焙 CLI（读地图预设 TOML → 调 `world/premade/premade_bake.*` → 写预制地图文件） | — | 脚本须**纯 ASCII 或带 BOM 的 UTF-8**（Windows PowerShell 5.1 按系统代码页读 `.ps1`）；**资源脚本**不得写入 `assets/` 之外的目录；重复运行必须幂等；**C++ 工具**（`baker/`）须纳入门禁扫描（`tools` 已在门禁默认 `IncludeDir` 内） |
| `cmake/Shaders.cmake` | 两段式 Shader 编译：GLSL →(glslc) SPIR-V →(shadercross) DXIL | — | 两种格式**都必须产出**：Vulkan 用 SPIR-V，D3D12 用 DXIL |
| `.github/workflows/` | CI：门禁 → 构建 → 测试 | — | 文件与 CI 脚本保持**纯 ASCII**（原因见 `ci.yml` 顶部注释）；新增步骤须本地可复现 |
| `docs/` | 方案文档、ADR、文件索引、开发记录、学习笔记、阶段计划、**内容基线文档** | — | 与代码同步 |
| `docs/adr/` | 架构决策记录（`NNNN-<主题>.md`）**+ `README.md` 决策索引** | — | 正文一经写入不回改；被取代时新增一条并互相链接；**每次决策变动须在同一次提交内更新索引** |
| `docs/plans/` | 阶段计划：当前阶段的任务分解、顺序、进度、下一步 | — | 只写计划与进度，不写技术结论（选型一律指向方案 / ADR）；阶段闭环后冻结不回改 |
| `docs/engine-capabilities.md` | **内容基线（引擎层）**：引擎能做什么（平台 / 渲染 / 世界表示 / 物理 / 数据 / 质量）及各项状态 | — | **只写引擎能力，与具体游戏无关**；必须区分"已引入未使用"与"已实现"；新增能力须先登记再实现 |
| `docs/game-design.md` | **内容基线（游戏层）**：本游戏要什么——世界生成要求、玩家能力、主角、当前阶段范围 | — | **只写需求，不复述引擎实现**；每行须可判定；依赖的缺失能力须与 `engine-capabilities.md` 互相链接 |
| `docs/npc-behavior.md` | **内容基线（游戏层）**：NPC 种类、感知与记忆、决策、寻路、日程、交互与战斗 | — | 技术约束写在此；**决策模型与寻路表示的选型写在 ADR**（候选不落此文件） |
| `docs/world-setting.md` | **内容基线（游戏层）**：题材与基调、地理与生态、种族与文明、历史、命名规范 | — | **内容必须来自项目所有者，AI 不得代拟**；影响生成的设定须同时落到配置才算生效 |
| `docs/ui-inventory.md` | **内容基线（游戏层）**：界面清单（位置 / 打开方式 / 关闭方式 / 与鼠标捕获的关系）+ 控件清单（类型 / **名义承诺** / 状态 / 当前限制）+ 设置与落盘路径 | — | **控件标签即行为契约**，禁止无效控件；依赖缺失时**显式标注**而非做假；新增界面须先登记再实现 |
| `.trae/skills/` | AI 开发规范技能（正文 + references + 门禁脚本） | — | 规范变更时同步更新 |

---

## 当前模块入口

| 入口 | 说明 |
| --- | --- |
| `engine/core/fixed_step.hpp` | 固定步长累加器（单帧补步封顶、渲染插值 alpha） |
| `engine/core/frame_limiter.hpp` | 帧率上限：纯函数决定呈现模式 + **睡眠式**限帧（**禁止忙等**） |
| `engine/core/log.hpp` | 统一日志接口（`VX_LOG_*`） |
| `engine/core/task_scheduler.hpp` | **任务调度薄封装**（T81 / [ADR 0003](adr/0003-task-scheduler-and-ecs.md) 的"启用形态"，见 [ADR 0022](adr/0022-volume-build-worker-pipeline.md)）：`ParallelTask`（可分片并行、**生命周期须覆盖到完成**）+ `TaskScheduler`（`Submit` 非阻塞 / `WaitForAll` 停机）。**本工程唯一引用 enkits 的地方**（enkits 头只在 `.cpp` 内出现）；线程池不可用时 `Submit` **在当前线程同步执行**（可观测、不静默） |
| `engine/input/input_map.hpp` | 输入动作状态层（上层只消费动作；鼠标按键与键盘对称） |
| `engine/platform/window.hpp` | 窗口与事件循环；**唯一**把 SDL 事件翻译进 `InputMap` 的地方；相对鼠标模式（捕获 / 释放）在此封装 |
| `engine/render/triangle_renderer.hpp` | PoC 冒烟测试路径（保留可编译，未接线） |
| `engine/render/mesh_renderer.hpp` | 通用网格渲染路径（顶点/索引缓冲、相机 UBO、索引绘制、纹理数组、HDR 目标 + 色调映射通道、渲染开销记账、**自发光网格**：片元 uniform 槽 3 逐网格推送、**变长几何就地更新**：`UpdateMeshGeometry` 只上传用到的顶点 / 索引前缀 + 每网格 `usedIndexCount`（T42）、**天空管线 + IBL 烘焙 + 环境纹理绑定**（T67：`BakeEnvironment` / `EnvironmentReady` / 全屏通道辅助 `DrawFullscreenPass`）、**蒙皮网格路径（T69）**：`SkinnedVertex` / `SkinnedMeshData` / `UploadSkinnedMesh` / `SetSkinningMatrices` —— 骨骼矩阵走**顶点只读 storage buffer**，每网格每帧**一次整块**上传；另有蒙皮阴影管线） |
| `engine/render/model_loader.hpp` | **模型导入**（T68）：glTF / `.glb` 的静态与蒙皮网格 + 骨骼动画（TRS 通道）**加载与采样**。公共头**不含 Assimp 类型**（Assimp 只在 `.cpp` 内出现）；`LoadModel` 为纯 CPU 路径（可从工作线程调用）、失败即抛不静默回退；`SampleJointLocalTransforms` / `ComputeSkinningMatrices` 为**确定性纯函数**。**只到"加载 + 采样"，不接线渲染**（蒙皮着色器与主角替换属 T69） |
| `engine/render/environment.hpp` | **环境贴图口径**（T67 / [ADR 0021](adr/0021-environment-ibl.md)）：天空 / irradiance / 预过滤 / BRDF LUT 的尺寸与级数（**唯一事实来源**）+ 纯函数（`PrefilterRoughnessForMip` / `EstimateTextureMipChainBytes` / `HalfFromFloat`）。不含 GPU 与 SDL 类型 |
| `engine/render/lighting_table.hpp` | `assets/config/lighting.toml` 的加载与校验（含**可选** `[environment]` 段，T67）；**光照 → GPU 的唯一投影入口**（`LightingUniform` / `BuildLightingUniform`，后者按"实际烘焙成的预过滤 mip 级数"给出 IBL 启用位） |
| `engine/render/shadow_cascade.hpp` | CSM **纯函数**：级联分割、texel 对齐的光空间矩阵、`ShadowUniform`（无世界 / 游戏专有类型） |
| `engine/render/instance_batch.hpp` | **GPU 实例化口径**（V0.7 H1 / [ADR 0034](adr/0034-object-instancing-and-hlod.md)）：`InstancePose`（世界位姿，原点用 `double`；**V0.8 追加围合体代理字段**，[ADR 0035](adr/0035-modular-building-kit-and-enterable-spaces.md) 决策四）+ 纯函数 `BuildInstanceModelToRender`（局部 → **渲染相对** `mat4`，红线 6/7）与 `PackInstanceTransforms`（批量打包 + **超容量截断**；每实例 = **`kInstancePoseBytes` = 96 B**：`mat4`(64) + 围合体 `vec4`×2(32)，与两条实例化 vert 的 std430 结构体逐字节一致）。无 GPU / SDL 类型；由 `MeshRenderer::UploadInstances` 消费 |
| `engine/render/texture_loader.hpp` | **纹理资源加载**（T57）：从文件读图 → LDR `RGBA8` / HDR（`.hdr`）线性 `RGB32F`；**解码前**尺寸守卫（`kMaxImageDimension`）、失败**即抛不静默回退**；**无 GPU 触碰**（可从工作线程调用，上传仍留在渲染线程）。`stb_image` 只在 `.cpp` 内出现（公共头不泄漏）。**消费者**：T66 地表 PBR 贴图、T67 环境贴图（HDRI 由 `game/` 解码后喂给 `MeshRenderer::BakeEnvironment`） |
| `engine/render/camera.hpp` | 第三人称相机 + 避障；`ITerrainQuery` 查询契约（由 `world/` 实现） |
| `world/terrain/terrain_world.hpp` | 地表世界入口：tile 容器、网格、脏重网格，并实现 `ITerrainQuery`。**P6-B（2026-10-06）**：增 `PruneStagedTiles(keep)` —— 窗口中心变化时**只失效**"已出常驻窗口 / 暂存 LOD 与目标不符"的预取条目（原整批 `ClearStagedTiles` 会把 worker 刚算好的一整圈丢掉 ⇒ 飞行时表现为可见 pop-in）；`ClearStagedTiles` 保留给"确实要全清"的场合 |
| `world/terrain/material_table.hpp` | `assets/config/materials.toml` 的加载与校验；**CPU→GPU 材质参数唯一投影入口**（`MaterialUniform` / `BuildMaterialUniform`）。**T43 起每层另有四个物理字段**（`density` / `friction` / `restitution` / `indestructible`，见 [ADR 0016](adr/0016-collapse-realism-impulse-material-debris.md)）与 **T46 的落地口径字段 `rigid_debris`**（刚性碎块落地后保留几何体，见 [ADR 0017](adr/0017-landing-by-material-rigid-vs-granular.md)）：**都不参与地表着色、因此不进 GPU uniform**，只决定倒塌整体的质量 / 摩擦 / 弹性、小碎片清除的守卫与落地后的表示；五个字段**可选**、缺省值等价于引入前的口径 ⇒ `schema_version` 保持 4 |
| `world/terrain/material_textures.hpp` | 程序生成占位材质贴图（albedo + 法线，确定性、可平铺；ADR 0009） |
| `world/terrain/world_bounds.hpp` | 世界边界盒、四周**空气墙**与**顶盖**的放置（**纯函数**，由 tile 范围推导；对任意地图尺寸生效）。**T84 起边界为"六面封闭"**：`ComputeBoundaryWalls`（4 堵）+ `ComputeBoundaryCeiling`（1 块顶盖） |
| `world/dig/dig_region.hpp` | 可挖区域标记表（`assets/config/dig_regions.toml`，ADR 0006 的**数据文件**部分；含包围盒**向外吸附**、优先级 / sealed 合并、块数上限校验），并实现**层间交接过滤器** `ITerrainQuadFilter`（ADR 0011） |
| `world/dig/dig_volume.hpp` | 可挖体积世界（ADR 0004 层 ②）：33³ `int8` 密度块（由高度场初始化）、球体挖除、脏块重网格、区域外密度回退；**只在标记区域内存在**；**体素材质持久化**（T42 / ADR 0014 修订：`VolumeBlock::material` 33³ **懒分配**、`0xFF` = 未写入回落列派生、`ReadMaterialRegion` / `SetMaterialSlot` / `MaterialBytes`）。**T81 增**（[ADR 0022](adr/0022-volume-build-worker-pipeline.md)）：`BlockBuildInput` / `BlockBuildResult` + **纯函数** `BuildBlockFromInput`（填密度 + Surface Nets + 分类，可在任意线程跑）、`CaptureBlockBuildInput`（主线程采快照）、`InstallBuiltBlock` / `PollBlockBuildsAndInstall`（主线程收包安装）、`SetBuildPipeline`（装了任务池后 `CreateBlock` 变为**异步提交**）；`CreateBlock` 未装任务池时仍是同步路径（单测 / 工具 / 降级）。**V0.10 S3 增**（[ADR 0037](adr/0037-world-state-save-v2-and-terrain-persistence.md)）：`ExportBlockSave` / `ApplyBlockSave` —— 只导出该块**拥有的** 32³ 采样（33³ 数组相邻块**重叠一格** ⇒ 边界层由拥有它的邻块给出，读回后按 `SampleDensity` 重算 ⇒ 与恢复顺序无关） |
| `world/dig/volume_mesher.hpp` | Surface Nets 等值面网格化（ADR 0007）：块内局部顶点 + 密度梯度法线 + 块间共享边界采样；纯函数（只依赖采样器接口）。**T42 增 `BuildRegionMesh(sampler, sizeX, sizeY, sizeZ)`**：同一套数学用于**任意尺寸区域**（倒塌整体的外观与地形同源的口径）。**T47 增 `CountBoundaryEdges(MeshData)`**：水密自检的**唯一实现**（"不是恰被 2 个三角形共用"的无向边条数；闭合曲面恒 0）—— 生产代码与单测共用，见 [ADR 0018](adr/0018-structural-support-and-representation-preserving-destruction.md) 决策五。**T55 增**（[ADR 0019](adr/0019-ambiguous-cell-vertex-splitting.md)）：**歧义 cell / 歧义面**按**实体侧连通分量拆顶点**（`cellEdgeVertex` + `kQuadCellLocalEdge`）⇒ 外观网格重新成为**流形**；**单分量 cell 的输出逐位不变** |
| `world/dig/volume_collision.hpp` | 可挖体积 → 物理层的**三角网静态碰撞体**提供者（T28 / ADR 0012）：每个有网格的块一个 Jolt `MeshShape`，挖除 / 塌落后按脏块重建 |
| `world/dig/volume_collapse.hpp` | 破坏后的**倒塌**（T29 起 / **T33 改为整体刚体化**，ADR 0015）：按「载荷通路（纵向接地）+ 悬挑跨度」判支撑 → 失支撑实心体按 **6 邻域连通分量**分组（每分量 = 一个整体）→ 抽出体素 + 生成凸包点集。**T42 增**：抽出前抓**体素补丁**（密度 + 有效材质，包围盒 ±1 格）与 `BuildCollapseUnitMesh`（用同一份 Surface Nets 把补丁网格化成"它原本那一片表面"，局部坐标与凸包同源）；回写数学 `WritebackCollapseUnit`（按落定位姿体素化回写 + **把材质搬到落点**）。**T43 增**（[ADR 0016](adr/0016-collapse-realism-impulse-material-debris.md)）：`CollapseSeed` 带**爆心 / 半径**、`ComputeUnitPhysics`（逐体素冲量 ⇒ 整体 `V` / `ω`；质量 = Σ 密度、摩擦 / 弹性 = 多数材质；**无 GPU / Jolt 依赖 ⇒ 可单测**）、**小碎片清除 + `indestructible` 守卫**。**T46 增**（[ADR 0017](adr/0017-landing-by-material-rigid-vs-granular.md)）：`CollapseUnit::rigidDebris`、`surfaceMaterialCounts`（T50 起**只作诊断**）、补丁**归属掩码** `patchIsUnit`、`hullPoints` 的**局部 AABB**、纯函数 `UnitWorldAabb`（唤醒判据）、**散体接地沉降**（回写后把"下方为空"的体素沿本列下落；刚性不沉降 ⇒ 形状不变），`CollapseWriteback` 增 `settledVoxels` / `stuckVoxels`。**同日缺陷修复**：`UnitPatchSampler` 索引改 1:1 + 非本整体实心当空气、`BuildCollapseUnitMesh` 多覆盖一格 cell ⇒ **碎块外观网格自闭合**（修"部分面透明、碰撞却在"）。**T48**：手工 OBB 判据 `LocalAabbContainsPoint` **已下线**（命中改由 `PhysicsWorld::RayCastDynamic` 回答，见 [ADR 0018](adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）。**T49 增**（ADR 0018 决策一）：支撑求解的 scope 改为「**以被改动采样为起点的实心连通域**」——`AnalyzeWindow`（① 纵向接地 → ② 连通域洪泛 → ③ 同层悬挑传播）+ 窗口**只向触界的侧**翻倍扩张 + 累计采样超限 ⇒ 告警 + 保守回退固定窗口；`CollapsePlan` 增 `domainVoxels` / `domainNarrowed`。**T50 增**（ADR 0018 决策二 / 四）：`CarveCollapseUnitPatch`（在**自身补丁**上做与地形同口径的球体挖除）、`RefreshCollapseUnitFromPatch`（由补丁重算体素 / 凸包 / 质量，**质心不变**）、`SplitUnitByMaterial`（按材质一致性拆子块，落地口径 = 子块自己的材质）。单次判定、不迭代 |
| `world/dig/collapse_table.hpp` | 倒塌规则表（`assets/config/collapse.toml`，`schema_version = 3`）：`enabled` / `max_cantilever_blocks` / `neighborhood_margin_blocks` / `settle_linear_speed` / `settle_angular_speed` / `settle_steps` / `initial_tilt_speed` / `impulse_speed` / `debris_delete_max_voxels` / `max_active_units`，逐项校验、非法即抛（**v3 删除** `mass_per_voxel` / `friction`：质量 / 摩擦 / 弹性改由材质表驱动，见 T43） |
| `world/dig/projectile_table.hpp` | 弹丸规格表（`assets/config/projectiles.toml`）：弹道 / 爆炸破坏 / 自发光；`[[projectile]]` 数组留出多类型扩展 |
| `world/dig/destruction_table.hpp` | **破坏表**（`assets/config/destruction.toml`，`schema_version = 1`，T31 / [ADR 0013](adr/0013-destructible-elements.md)）：`points_per_cubic_block`（伤害预算的**唯一手感旋钮**）+ `prop_damage_threshold` / `prop_broken_tint`（**器物参数先落表**，待 ADR 0004 层 ③ 消费）；缺失 / 越界 / 版本不符一律抛异常（不静默回退） |
| `world/object/object_layer.hpp` | **物件层门面**（ADR 0004 层③）：`ObjectTable`（类型表 `[[type]]` + 放置清单 `[[placement]]` + **程序化散布 `[[scatter]]`**，V8 + **流式散布 `[[scatter_tiled]]`**，V0.6 C3 + **删除项 `[[remove]]`**，E3）/ `ObjectLayer`（EnTT 注册表，PIMPL）/ `ObjectType`（含 **`category`** 仓库分组，E3）/ `ObjectPlacement` / `ObjectScatter` / **`ObjectScatterTiled`** / **`ObjectRemoval`** / `ObjectAssetKind`。（V0.6 C3：`[[scatter_tiled]]` 的 `cell_blocks` / 坡度 / 高度带 / `landforms` **全部校验、非法即抛**，缺省语义 = 不写即空表）**V0.5 E1/E3 追加**：`LoadOverlayFromFile`（可编辑层：`[[type]]` 可选、引用须在"发布清单 ∪ 本层"内）+ `MergeObjectTables`（**发布清单 → 应用 `[[remove]]` → 追加 `[[placement]]` → 散布**，ADR 0032 决策五）+ `RemovePlacementsByRemoval`（纯函数）+ `IsValidObjectCategory`。**V0.8 追加**（[ADR 0035](adr/0035-modular-building-kit-and-enterable-spaces.md)）：`ObjectAssetKind::Kit` + **`ObjectKitRole`**（floor / wall / wall_door / roof）+ `ObjectType::kitRole` / `moduleBlocks` + **`ObjectBuilding` / `ObjectBuildingPiece`** + `ObjectTable::buildings`（配置 `[[type]] kind="kit"` 与 `[[building]]`）+ **`ObjectEnclosure` / `ComputeBuildingEnclosure`**（纯函数：屋顶并集 → 围合体代理；室内变暗用）。**非法即抛**（ADR 0005） |
| `world/object/object_edit_save.hpp` | **可编辑层保存器**（V0.5 E3 / [ADR 0032](adr/0032-object-palette-and-placement-mode.md) 决策六）：`SaveObjectEditLayer(path, editLayer)` —— 写 `schema_version` + `destructible_enabled` + `[[type]]` / `[[placement]]` / `[[remove]]` / `[[scatter]]`（**该层全部增量**）；**临时文件 + `rename` 原子替换**；**覆盖式 ⇒ 不保留手写注释**；读回走 `LoadOverlayFromFile`（往返逐字段一致）。**只写增量、不碰发布清单**（ADR 0028 的"A 不入库"口径） |
| `world/object/object_mesh.hpp` | 物件**局部网格**：**程序化代形**（盒 / 椭球 / 锥 / 门环）+ **`BuildObjectMeshFromModel`**（V8：外部 GLB → `MeshData`，等比装进 `2*half_extent` 的盒、底面贴地）+ **`BuildKitPieceMesh`**（V0.8 / [ADR 0035](adr/0035-modular-building-kit-and-enterable-spaces.md)：kit 构件代理体 —— 地板 / 屋顶 = 有厚度的板、墙 = 中间薄板、**门洞墙 = 墙垛 ×2 + 门楣，洞口区域无几何**；配套 `AppendBoxCentered`）+ `RotateMeshAboutY`。**纯函数、header-only** |
| `world/object/object_scatter.hpp` | **程序化散布**（V8，**纯函数、header-only**）：`PlanObjectScatter`（分块抖动网格 + 有界抖动 + 确定性洗牌子集 ⇒ 同种子逐位可复现）+ `ScatterPoint` |
| `world/object/object_placement_rule.hpp` | **地形感知的元素放置规则**（V0.6 C1/C2 / [ADR 0033](adr/0033-world-content-placement-and-streaming.md)，**纯函数、header-only**）：`PlacementRule`（`tech-plan §3.3` 四项判据：坡度区间 / 高度带 / 地貌允许集 / 互斥间距 `cellBlocks`）+ `PlacementSample` + `IsPlacementAllowed`（闭区间、`NaN` 拒绝）+ **`PlanTileCandidates`**（**分块确定性**候选点：tile 局部抖动网格 ⇒ 点全在本 tile 内、相邻 tile 不重复、同种子逐位可复现、最小间距 > `0.5 × cell`）。**只做判据与候选点，不碰地形 / 不建实体** |
| `world/object/terrain_sampling.hpp` | **地形采样**（V0.6 C4 / [ADR 0033](adr/0033-world-content-placement-and-streaming.md)，**纯函数、header-only**）：`SlopeDegreesFromGradient`（坡度 = `atan|∇h|`）、`SlopeDegreesFromNeighbors`（四邻**中心差分**）、`SamplePlacement`（把"读高度 / 读地貌"抽象成回调 ⇒ 不依赖 `TerrainWorld` / 噪声生成器，可单测）。产物 = `PlacementSample`，直供 `IsPlacementAllowed` |
| `world/object/object_support.hpp` | 物件**支撑探测**（纯函数）：底面中心 + 四角 × 绕 Y 旋转 + "任一实心即有支撑" |
| `world/object/placement_snap.hpp` | **放置吸附与对齐**（V0.11 I1 / I1c / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)，**纯函数、header-only**）：`PlacementSnapSettings`（世界网格步长 `translateBlocks` 缺省 0.25 / 朝向 `yawDegrees` 缺省 1° / 邻居半径 `neighborRadiusBlocks` 缺省 2.0；`<= 0` = 关闭）+ `SnapToStep`（平移就取整）+ `SnapYawDegrees`（朝向吸附 + 归一化）+ `PlacementFootprint` + **`SnapPlacementToNeighbor`**（**邻居优先吸附**：半径内取最近邻居 ⇒ 落点贴其最近的面；无邻居 ⇒ 返回 false，由调用方退回世界网格）。**只改位置、不改朝向**（登记为后续可选） |
| `world/object/placement_validation.hpp` | **放置合法性：2D 足迹重叠检测**（V0.11 I2 / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)，**纯函数、header-only**）：`FootprintsOverlap2D(a, b, epsilon=1e-6)` —— 两个**有向矩形**（中心 + 半尺寸 + 绕 Y 朝向）的**分离轴测试（SAT）**，4 条候选轴（各自 2 条局部轴）；**边界接触（间隙 0）不算重叠**。供预览侧与提交侧**共用同一判据**（不会"看着能放、点了不放"） |
| `world/object/edit_history.hpp` | **编辑操作的撤销 / 重做栈**（V0.11 I3 / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)，**纯逻辑、header-only**）：`EditOpKind`（放置单件 / 删除单件 / 放置成套建筑 / 删除成套建筑 / 改室内变暗）+ `EditDarkeningTarget` + `EditCommand`（**纯数据**的可逆记录：落点 / 原下标 / 改前改后值 / 被一并清掉的变暗覆盖 / **落点 ①③ 的地形逐列改前改后高度**）+ `ApplyEditCommand` / `RevertEditCommand`（严格互逆的**数据**操作）+ `EditHistory`（**命令栈**：新操作清空重做栈、容量 64、`Record` / `UndoCommand` / `RedoCommand` / `MutableRedoCommand` / `CommitUndo` / `CommitRedo` / `Clear`） | 只依赖 `world/object/object_layer.hpp` + `world/terrain/terrain_types.hpp`（`TerrainColumnEdit`）；**不认识 GPU / Jolt**（运行期动作由 `game/main.cpp` 按同一条命令执行）；**可单测** |
| `world/object/gizmo.hpp` | **gizmo 手柄**（V0.11 I4 / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)，**纯逻辑、header-only**）：`GizmoHandle`（`None` / `TranslateX` / `TranslateZ` / `RotateY`）+ `GizmoLayout`（原点 / 轴长 / 拾取半径 / 环半径）+ `GizmoRay` + **`PickGizmoHandle`**（射线 × 轴杆 AABB、射线 × 水平面与**环带半径**判定，取**最近**者）+ `GizmoAngleDegrees` + **`GizmoYawDeltaDegrees`**（跨 ±180° 归一化，不跳变）+ `BuildGizmoAxisMesh` / `BuildGizmoRingMesh`（手柄几何，复用 `object_mesh_detail::AppendBoxCentered`） | 只依赖 `world/object/object_mesh.hpp` + `render/mesh_renderer.hpp`；**不认识 GPU 句柄 / 交互状态**（绘制与拖动在 `game/main.cpp`）；**可单测** |
| `world/terrain/terrain_tile_source.hpp` | **地表数据来源抽象**（V4）：`ITerrainTileSource::FillTileHeights` —— 返回 true 即 tile **完整可用**（`heights` 同口径 + `maxSurfaceBlocks` 已刷新）；实现须**线程安全**（worker 并发） |
| `world/premade/premade_terrain_source.hpp` | **预制地图作为地表数据源**（V4）：`PremadeTerrainTileSource`（按 `(tileX,0,tileZ)` 读宏高度块；缺块 ⇒ false + 一次性 WARN + 计数；线程安全） |
| `world/save/world_instance_save.hpp` | **秘境存档槽**（V10 / [`plans/v0.5.md`](plans/v0.5.md) §1.13.1）：`SavedWorldInstance`（`worldId` / `seed` / `generation`）+ `WorldInstanceSave` + `LoadWorldInstanceSave` / `SaveWorldInstanceSave`。**只存元数据**（世界种子 / 已重置次数；**不含世界状态**）⇒ TOML + `schema_version`（红线 8：**未知版本 ⇒ 拒绝**，不静默误读）；写 = **临时文件 + `rename` 原子替换**；`seed` 写**十进制字符串**（TOML 整数是有符号 64 位、种子是 u64 ⇒ 才能无损）。**纯序列化、不认游戏层类型**（`game` 侧映射到 `WorldInstance`） |
| `world/save/world_save.hpp` | **世界状态存档 v2（`.voxr`）容器 + 载荷编解码**（V0.10 / [ADR 0037](adr/0037-world-state-save-v2-and-terrain-persistence.md)，**布局已冻结**）：`WorldSaveChunkKind`（`HeightDirtyTile` / `VolumeDirtyBlock`）+ `WorldSaveChunkKey`（**升序 = 写盘确定性**）+ `WorldSaveHeader`（`version` / `flags` / `generatorVersion`=地形参数内容哈希 / 世界范围与种子 / 区域表 `schema_version` + 内容哈希）+ `WorldSaveWriter`（**只存脏块** + 逐块 zstd + **临时文件 + `rename` 原子替换** + 同输入**逐字节相同**）+ `WorldSaveReader`（`Open` 校验魔数 / 版本 / **头部 CRC32** / 索引区间 / `flags` 与索引一致，按块**随机读**并解压）+ **载荷编解码纯函数**（`Encode/DecodeHeightDirtyTile`（**只存脏列**：列号 + i16 差值）/ `Encode/DecodeVolumeDirtyBlock`（`int8 density[32³]` + 可选 `u8 material[32³]`））+ `Fnv1a64`（世界定义一致性哈希）。**zstd 只在本文件与 `premade_map.cpp` 内出现**（同类容器形态） | 可依赖 `engine` 与第三方（**zstd，只在 `.cpp` 内出现**） | 冻结格式 = **改字段必须升 `version` + 迁移函数 + 迁移测试**（红线 8）；只写脏块（红线 4）；未知版本 / 损坏 / 长度不符**即抛**（不静默回退）；**公共头不泄漏 zstd** |
| `world/save/world_state_save.hpp` | **世界状态差量的运行期会话 + 异步写盘器**（V0.10 S4 / [ADR 0037](adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策四、五）：`WorldStateSave`（**内存块表**：`WorldSaveChunkKey` → 逐块**原始未压缩载荷**；`SetChunk` / `EraseChunk` / `FindChunk` / `WriteToFile` / `LoadFromFile` —— **不依赖地形 / 体积类型** ⇒ 可脱离 SDL / GPU 单测，且"采集"与"落盘"解耦）+ `WorldSaveFlusher`（把**快照**交给 worker 的**单飞**写盘器；`Submit` 非阻塞、`Poll` / `WaitForIdle` 收包；自持 **1 个 worker**，不借构建池）。**卸载不阻塞磁盘**：脏单元**卸载前**先把差量采集进会话（改动已离开该单元、不会丢），写盘仍走 worker | 可依赖 `engine`（`core/task_scheduler`）与 `world/save/world_save.hpp` | 同一时刻**只允许一个在飞写盘**（不排队堆积）；写盘失败**必须上报**（不静默）；**主线程绝不阻塞在磁盘**（SKILL §四） |
| `world/streaming/dig_volume_residency.hpp` | **可挖体积的常驻调度**（T60 / [ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md)）：`TileOfBlockIndex`（块→tile **精确**映射）/ `DigVolumeWindow` + `WindowForPlayerBlocks`（玩家窗口）/ `ResidencyWindowForPlayerBlocks`（**预取窗口** = 活动半径 + `kResidencyPrefetchTiles`，T80 修订）/ `PlanDigVolumeResidency`（**纯函数**：要建 / 要卸 / **脏块留驻** / 超限淘汰最远者）/ `DigVolumeScheduler`（`Update` 幂等 + `Step` 先建后卸、确定序、分帧；**活动窗口 `Window()` 与常驻窗口 `ResidencyWindow()` 分离**） |
| `world/streaming/terrain_tile_residency.hpp` | **地表 tile 的常驻策略与调度**（W7-S1 / S2 / S3b，[ADR 0024](adr/0024-terrain-streaming-and-lod.md)）：`TerrainTileWindow`（中心 tile ± R；`TileCount()` = `(2R+1)²`）/ `TerrainTileRange`（世界内存在的 tile 矩形范围）/ `TerrainWindowForPlayerBlocks`（活动窗口）/ `TerrainResidencyWindowForPlayerBlocks`（**常驻窗口** = 活动半径 + `kTerrainResidencyPrefetchTiles`）/ `TerrainHysteresisCenterTile`（**滞回**推进中心 tile）/ `PlanTerrainTileResidency`（**纯函数**：要加载 / 要卸载 / **编辑块留驻** / 超限淘汰最远者；**范围版成本 O(窗口)**）/ `TerrainTileScheduler`（**幂等 `Update`** 重算计划 + **分帧 `Step`** 先加载后卸载、确定序、编辑块超限 **WARN**；**W7-S3b 增分环构造**：`TerrainLodRings`（8/16/32 tile → LOD 0/1/2）、`TerrainLodLevelForTileDistance`、`TerrainLodMorphRangeForLevel`（morph 恰在环边界取 1）、`LodLevelForTile`、`PendingRelodCount` / `StepRelod`（relod **只改网格、不碰世界数据**）、`CollectPendingLoadTiles`（供预取）。**单半径构造下 relod 恒为 0 ⇒ 与从前逐位一致**）。**P6（2026-10-06）**：「更新收包」拆成计划 / 收包 / 预取三段子计时（定位结论：`Update` 仅 ~1 ms，**不是**瓶颈）；`Update` 只物化一次 `ResidentTiles()`、`PlanFromDesired` 加 `is_sorted` 快路径（已升序时省一次整窗拷贝 + 排序）、`desired.reserve(window.TileCount())`。**只含策略与调度，不做网格 / 物理**（接入在 `game/`） |
| `world/streaming/terrain_tile_build_pipeline.hpp` | **地表 tile 构建任务池**（W7-S3b，照 T81 / [ADR 0022](adr/0022-volume-build-worker-pipeline.md) 形态）：worker 跑**纯函数** `GenerateTerrainTileData`（噪声 + 预设编辑）+ `BuildTerrainMesh`（**不带四边形过滤**）⇒ 主线程只做「收包 → `ApplyQuadFilterToMesh`（按**当前**常驻集合过滤）→ 装进 `TerrainWorld` → GPU 上传 → 碰撞体同步」。**为什么过滤不在 worker**：过滤器依赖"当前常驻集合"这一可变状态，放 worker 就要做快照、会引入"结果随快照陈旧"的歧义；两条路径共用同一份过滤实现 ⇒ **逐位一致**（单测钉死）。worker 不碰世界数据 / 图形 API / Jolt；线程池不可用 ⇒ WARN 一次并回退同步路径（结果不变）。**P6 收尾（2026-10-06）**：完成队列用 `std::deque` + `pop_front`（O(1)；原 `erase(begin())` 要搬移整队，元素含 8 KB `TerrainTile`）；在飞任务的回收改为「**本轮收包弹空时一次**」（原每弹一个都 O(在飞) 重扫）。 |
| `world/streaming/volume_build_pipeline.hpp` | **块构建任务池**（T81 / [ADR 0022](adr/0022-volume-build-worker-pipeline.md)）：`Submit(BlockBuildInput)`（非阻塞）+ `TakeCompleted(BlockBuildResult&)`（主线程收包）+ worker 观测统计。worker 跑**纯函数** `BuildBlockFromInput`；**唯一互斥量只保护完成队列**（不保护世界数据）；worker 不碰图形 API |
| `world/streaming/shell_block_build_pipeline.hpp` | **地表壳块构建任务池**（W7-S3b①，照 T81 / [ADR 0022](adr/0022-volume-build-worker-pipeline.md) 形态）：worker 跑**纯函数** `BuildShellBlockMesh`（请求携带**区域快照** ⇒ 边界淡出可复现），主线程只做「收包 + GPU 上传 + 加碰撞体」。河道下切场以 `const RiverCarveField*` 只读传入（`CarveAt` 无状态 ⇒ 多 worker 并发只读安全）。线程池不可用 ⇒ WARN 一次并回退同步路径 |
| `game/orb.hpp` | 光球（T27）：弹道推进与命中检测（**纯函数**，只依赖 `IOrbWorldQuery`）、程序化球网格、固定容量弹丸池 |
| `game/rigid_collapse.hpp` | 倒塌整体的运行时（T33 / ADR 0015）：网格池（**只在启动时**建 GPU 资源）、`Spawn` 用 `UpdateMeshGeometry` 写一次**与地形同源的等值面**（T42；只上传用到的前缀，超容量按整个四边形截断）、每步读刚体位姿 + 落定检测、每帧只推 `mat4`、落定后 `Writeback` + 清空槽位（索引数 0 = 不可见）。**T46 / [ADR 0017](adr/0017-landing-by-material-rigid-vs-granular.md)**：刚性整体落定后**保留几何体**（`retained`，不回写 ⇒ 形状不变）；池 **16 槽**；`RetireOldestRetained`（池满腾位 ⇒ 惰性回写）/ `AwakenIntersecting`（块碰撞体重建后唤醒相交残骸）。**T47 增**：`BuildUnitMesh` 在**截断前 / 截断后**各做一次**外观网格闭合自检**（`vx::CountBoundaryEdges`），非 0 即 WARN 出**体素数 / patch 尺寸 / 两处边数 / 是否被容量截断**并把成因指名到候选 ①②③（`Spawn` 记入 `ActiveCollapseUnit`，`RetireRetained` 回报）；容量截断 WARN 补体素数与原始规模。**T48 / [ADR 0018](adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三**：命中判定改由 `PhysicsWorld::RayCastDynamic` 回答（句柄 + 真实凸包表面）⇒ `ContainsRetainedPoint` / `RetireRetainedAt` **下线**。**T50 / ADR 0018 决策二**：命中动态刚体改走 **`CarveBody`**（在碎块**自身补丁**上雕刻 → 按剩余体素在**当前姿态**下原地重建刚体 → 复用同一网格槽位；剩余过少 / 凸包不足 / 重建失败 ⇒ `RetireCarved` 删除整体）⇒ 命中路径的"惰性体素化"**下线**；`CollapseCarveResult` / `TotalCarved` / `TotalCarvedVoxels` |
| `game/out_of_bounds.hpp` | 出界判定（**纯函数**）+ 救援余量；越界/坠落时送回出生点 |
| `game/portal_interaction.hpp` | **最近传送门查询**（V3，**纯函数**：`FindNearestPortal` 半径内取最近、等距取先出现者）+ `PortalEntry`（门坐标 + 目标世界 id + 门显示名 `name`，V3c） |
| `game/ground_pick.hpp` | **准星 → 地表拾取**（V0.5 E2，**纯函数、header-only**）：`RaycastGround`（沿射线按固定步长求与高度场的交点、**跳过无地表数据的列**）+ `GroundPick`（平面坐标 + 地表高度 + 命中距离）。供 `F2` 摆放 / 预览定位（成本为常数级） |
| `game/object_palette.hpp` | **物件选择器的纯逻辑**（V0.5 E3 / [ADR 0032](adr/0032-object-palette-and-placement-mode.md)，**纯函数、header-only**）：`PaletteCategory` + `BuildPalette`（按 `ObjectType.category` 分组；**类别顺序 = 配置首次出现顺序**、类别内 = 类型表顺序 ⇒ 确定性，红线 7）+ `PaletteState` + `ClampPaletteState` + `SelectedPaletteType`。不含 UI 与 GPU |
| `game/object_preview.hpp` | **选择器预览小图的纯逻辑**（V0.5 E4 / ADR 0032 决策八，**纯函数、header-only**）：`PreviewTriangle`（`[0,1]²` 屏幕空间 + `shade`）+ `BuildObjectPreview(mesh, yaw, pitch)` —— 正交投影 → **顶点法线平均的背面剔除**（**不依赖三角形绕序**）→ 固定光源**朗伯明暗** → **深度升序**（画家算法）→ 归一化（基准 = **未旋转包围球** ⇒ 转动不"呼吸"）。**零 GPU 资源**（面板用 `ImDrawList` 直接画） |
| `game/placement_rotate.hpp` | **摆放模式的旋转手感**（V0.11 I1b / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)，**纯函数、header-only**）：`PlacementRotateSettings`（点按步长 `stepDegrees` 缺省 1° / 长按速率 `rateDegPerSec` 缺省 90°/秒 / **长按启动延迟 `holdDelaySeconds` 缺省 0.4 s** / 单帧上限 `maxFrameSeconds`）+ `PlacementHoldActive(held, heldSeconds, s)`（**按住 ≥ 延迟才"生效"**）+ `PlacementRotationDeltaDegrees(...)`（**点按 = 恰好一步、长按已生效 = 按速率连续转**；速率 `<= 0` = 只点按；点按优先于按住）—— 编辑器与运行时"游戏内建造"**共用** |
| `game/portal_menu.hpp` | **传送门交互菜单的数据模型**（V9，**纯函数**：`BuildPortalMenuModel` —— 由门 / 目标清单 / 秘境实例组装「门名 → 秘境名」+ 可用动作 `canReset`（**仅肉鸽秘境**）+ 生成次数）与 `PortalAction` / `PortalMenuRequest`；UI 层只按此绘制，故"该显示什么 / 给哪些动作"可由 `tests/portal_menu_test.cpp` 钉死 |
| `game/world_fingerprint.hpp` | **世界指纹**（V2c，**纯函数**：`ComputeTerrainFingerprint` —— 种子 + 升序 tile 全分辨率 `int16` 高度 → **64 位 FNV-1a**）—— 用于证明"**同种子切回同一世界逐位相同**"（红线 7） |
| `game/character_movement.hpp` | 主角移动基向量（**纯函数**：由相机 yaw 得前向 / 右向；方向语义有单测钉死） |
| `game/character_mesh.hpp` | 主角**程序化胶囊代理网格**（T69 起是**缺模型时的回落**占位体，尺寸同碰撞胶囊） |
| `game/character_model.hpp` | **纯函数**（T69）：`BuildSkinnedMeshFromModel`（把 `vx::Model` 的所有网格拼成一个 `SkinnedMeshData`，并给出绑定姿态的"脚底中心"`localPivot`）+ `FindAnimationClip`（按名字查 clip，区分大小写）。可直接单测（用 `tests/fixtures/skinned_triangle.gltf`） |
| `game/character_animation.hpp` | 主角**动画状态机**（T69，**纯函数**）：`SelectCharacterAnimState`（由"是否着地 / 水平速度 / 竖直速度"选 `Idle`/`Run`/`Jump`/`Fall`）+ `ClipNameForCharacterState`（状态 → clip 名；`Fall` **复用 `Jump`**，属已登记取舍） |
| `game/mouse_capture.hpp` | 鼠标捕获状态机（**纯函数**：`Esc` 释放 / 点击重捕获；**重捕获点击先于笔刷判定被消费**，避免误挖）。**物理层**（光标是否锁定），与 `input_context.hpp` 的**逻辑层**正交、不合并 |
| `game/input_context.hpp` | **输入上下文栈 + 动作归属表**（V0.11 / A6，**纯函数**、header-only）：`InputContext`（自由活动 / 建造 / 修改 / 修改-拖动 / 模态）+ `InputContextState` + **`OwnerOf(state, action)`（「按键 ↔ 谁生效」的单一权威）** + **`EscPopTarget`（`Esc` 栈顶弹出）**；未被子上下文 claim 的动作**穿透到 base** ⇒ 移动/飞行三模式同源（[ADR 0040](adr/0040-input-context-stack-and-action-ownership-table.md)） |
| `game/gameplay_input.hpp` | 游戏输入**抑制**决策（**纯函数**：由 `InputContext` 派生 —— `Modal` ⇒ 三类全量抑制，其余 ⇒ 全放行）。**不采信** ImGui 的 `WantCapture*`（只读叠加层不抑制，V0.9 回归口径） |
| `game/screen_ray.hpp` | **屏幕像素 → 世界射线**（V0.11 / A8，**纯函数**：`ScreenPointToRay` —— 由视图投影矩阵 + 视口尺寸 + 光标像素反投影；**自由光标拾取**的前提，见 [ADR 0041](adr/0041-immersive-modify-mode-and-editor-camera.md)） |
| `game/system_panel.hpp` | ESC 系统面板（显示模式 / 分辨率 / 音量 / 退出）；**不碰 SDL、不写文件**，只回报"用户做了什么" |
| `game/test_mode.hpp` | **测试模式解析**（T85 / G13，**纯函数**）：`ParseTestModeFromArguments` 由 `--auto-test` / `--manual-test="项1;项2"`（按 `;` 切分、丢弃空项、可多次给出）解析出**自动 / 人工 + 待人工确认项列表**；`IsOptionArgument` 区分"启动开关"与位置参数。**只读展示**（由 F1 面板置顶横幅消费），不改玩法 |
| `game/ui_font.hpp` | UI 字体**三级解析**（仓库 `assets/fonts/` → 系统 CJK → 无）与加载 |
| `game/ui_text.hpp` | **标签缝**：所有 UI 标签的唯一取词处（命中 CJK 用中文，否则整表英文；**禁止绕过**，有单测与源码扫描防护） |
| `game/ui_theme.hpp` | UI 统一主题（**唯一**样式入口 `ApplyUiTheme`） |
| `engine/platform/settings.hpp` | 系统设置读写（TOML，落盘到 `SDL_GetPrefPath`）+ **唯一音频增益入口** `ApplyMasterVolumeGain` |
| `engine/platform/console.hpp` | **控制台 UTF-8 输出**（V0.10 / S10）：`EnableUtf8ConsoleOutput()` 把本进程所附控制台的代码页切到 **65001**（工程内面向人的字符串统一 UTF-8，否则中文日志在 Windows 终端乱码）；**返回实际生效的代码页**供调用方写日志（可判定证据）；**无附加控制台 / 非 Windows ⇒ 空操作**，不抛、不分配堆 |

---

## 规划中（尚未创建）

| 规划路径 | 用途 | 备注 |
| --- | --- | --- |
| `assets/maps/**`（预制数据目录） | 离线烘焙产物（**不入库**；`tools/` 生成 / 取回 + 校验和 + 台账） | 与 `assets/textures/`、`assets/models/` 同口径；可读定义（种子 / 范围 / 参数）入库，大数据不入库 |
| `editor/` | **场景 / 关卡编辑器**（**独立可执行**；编辑器与运行时**分离**，依赖只能向上 —— **`engine` / `world` / `game` 一律不得 include 它**） | **已批准启动（2026-10-07 / [ADR 0038](adr/0038-construction-editor-and-runtime-separation.md)）**：**不再是暂缓项**；代码落点 = 阶段 [V0.11](plans/v0.11.md) 的 E1（尚未创建）。建造核心的**纯逻辑**（吸附 / 合法性 / 撤销栈 / 蓝图）放 `world/object/`，编辑器外壳放本目录 |
