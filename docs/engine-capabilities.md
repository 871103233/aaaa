# 引擎能力与产出范围

> **性质**：本仓库的**内容基线文档**之一（见 `.trae/skills/voxel-engine-dev-standards/SKILL.md` 第六节）。
> **层面**：**引擎层** —— 这里只写"**引擎本身能做什么**"，与"我们要做哪款游戏"无关。
> 游戏侧的需求、玩法与世界观见 [游戏设计](game-design.md) 与 [世界观设定](world-setting.md)。
> **维护义务**：任何一次改动**新增、修改或移除引擎能力**，必须**同一次提交内**更新本文件对应行。
> 未开发的能力**先占位**（状态 `未开始`），不得留白。

## 0. 状态图例

| 状态 | 含义 |
| --- | --- |
| **已实现** | 代码可运行，有测试或冒烟证据 |
| **部分实现** | 骨架可用但缺关键环节（如仅占位外观、仅近场、无真实资源） |
| **已引入未使用** | 依赖已在 `vcpkg.json` / `third_party/`，但代码尚未真正使用 |
| **未开始** | 尚无代码；本行只是占位 |

> 只描述**代码事实**，不描述愿望。

## 1. 引擎真实具备的能力

### 1.1 平台与运行时

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| 窗口与事件循环 | **已实现** | SDL3；`engine/platform/`（**唯一**读取 SDL 事件队列处） |
| 相对鼠标模式（光标捕获 / 隐藏，位移以相对增量持续输入） | **已实现** | `engine/platform/window.hpp`（`SetRelativeMouseMode`，幂等）；**失焦自动释放**由平台层处理，重捕获策略在上层 |
| 通用 SDL 事件转发回调（供 UI 层接入；每事件一次、**零分配**） | **已实现** | `engine/platform/window.hpp`（`SetEventCallback`）；**`engine/` 不依赖 ImGui**，由 `game/` 装入 `ImGui_ImplSDL3_ProcessEvent` |
| 显示模式与分辨率控制（窗口 / 桌面无边框全屏；档位取自 SDL） | **已实现** | `engine/platform/window.hpp`（`SetFullscreen` / `SetWindowSize` / `SupportedResolutions`） |
| 系统设置持久化（TOML：显示模式 / 分辨率 / 主音量 / 帧率上限） | **已实现** | `engine/platform/settings.hpp`；落盘 `SDL_GetPrefPath`；缺文件用默认、非法**明确报错**、越界钳制 |
| 显示器刷新率查询 | **已实现** | `engine/platform/window.hpp`（`DisplayRefreshRate`：`SDL_GetDisplayForWindow` → 桌面显示模式）；未知时回退并记日志 |
| 帧率上限（垂直同步 / 睡眠限帧，**零忙等**） | **已实现** | `engine/platform/window.hpp`（`SetVSync`，唯一呈现模式路径）+ `engine/core/frame_limiter.hpp`（纯函数决定模式；下限档用一次 `SDL_DelayNS`） |
| 输入 → 动作（键鼠 → 命名动作，每帧采样一次） | **已实现** | `engine/input/`；上层只消费动作 |
| 固定步长主循环（1/60，单帧补步 ≤ 5，插值 alpha） | **已实现** | `engine/core/fixed_step.hpp` |
| 单调计时 | **已实现** | `engine/core/clock.hpp`（`SDL_GetPerformanceCounter`） |
| 统一日志接口（`VX_LOG_*`） | **已实现** | `engine/core/log.hpp` |
| **长任务按帧分片（预算队列）+ 加载画面（进度可见）** | **已实现** | `game/main.cpp`（`LoadingScreen`：出帧 + 阶段文字 + 加权总进度；`kLoadWorkBudgetMs = 8 ms`）/ `game/debug_overlay.*`（`SetLoadingStatus` / `BuildLoadingUI`）+ `world/terrain/material_textures.*`（`MaterialTextureBuilder` 分步到像素行）+ `world/dig/dig_volume.*`（`BeginInitFromHeightField` / `StepInitFromHeightField`）。**硬规则见 SKILL「不冻结画面」**：任何一次性重计算都不得让当前视角停下等待；分片**只改变"何时可见"、不改变结果**（分步与一次性路径**逐位一致**，各有测试钉死） |

### 1.2 渲染

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| GPU 抽象（SDL3_gpu 薄封装） | **已实现** | `engine/render/` |
| 双格式 Shader 管线（GLSL → SPIR-V **与** DXIL，两者都必须产出） | **已实现** | `cmake/Shaders.cmake`（ADR 0002） |
| 通用网格渲染路径（顶点/索引缓冲、相机常量、**逐网格原点偏移**、索引绘制） | **已实现** | `engine/render/mesh_renderer.hpp`；顶点只承载**网格局部**坐标，绘制时按网格推送"网格原点 − 渲染原点"（顶点 uniform `set = 1` / slot 0，含推送去重）|
| 纹理数组（多层 `SDL_GPUTexture`，每层独立 mipmap；`sampler2DArray` 采样） | **已实现** | 同上；地表材质按 ADR 0009 使用（**禁止**改用纹理图集，见 `references/meshing-and-render.md` §3） |
| 片元 uniform 块（材质参数，std140；CPU→GPU **唯一投影入口**） | **已实现** | `world/terrain/material_table.hpp`（`MaterialUniform` / `BuildMaterialUniform`，`static_assert` 钉死布局）+ `engine/render/mesh_renderer.hpp`（上传） |
| 程序生成占位材质贴图（**材质四件套 albedo / normal / roughness / AO + 宏观变化**，确定性、可平铺） | **已实现** | `world/terrain/material_textures.*`（ADR 0009 / ADR 0010 P2）；5 张 `R8G8B8A8_UNORM` 纹理数组（四件套各 4 层 + macro **1 层**）× 256²，含 mip 约 **5.67 MB** 显存。多尺度（双频段）且**逐字节确定性** |
| 叠加层接口（`IRenderOverlay`，用于调试 UI） | **已实现** | 同上 |
| 动态网格顶点刷新（就地更新定长网格顶点；稳态**零堆分配**、不建 GPU 资源） | **已实现** | `engine/render/mesh_renderer.hpp`（`UpdateMeshVertices`，同时更新该网格登记的原点）；供每帧移动的网格（角色代理体 / 光球）使用 |
| **变长网格几何就地更新**（顶点 + 索引都可变；**只上传用到的前缀**；空网格 = 不可见 ⇒ 零上传零绘制） | **已实现** | `engine/render/mesh_renderer.hpp`（`UpdateMeshGeometry` + `MeshResources::usedIndexCount`）；供倒塌整体的网格池使用（T42：槽位按容量一次建好，之后只写用到的前缀） |
| **自发光网格**（片元 uniform **槽 3**：`rgb` = 自发光颜色、`a` = 强度；`0` = 普通地表网格） | **已实现** | `engine/render/mesh_renderer.hpp`（`UploadMesh(..., bool emissive)` + `SetEmissiveColor`，`DrawMeshes` **逐网格**推送）+ `assets/shaders/mesh.frag`（在**雾之后**叠加，使远处光球不被雾洗掉）。用途：光球弹丸。**踩坑**：SDL_gpu 的 `set = 3` uniform 绑定必须**从 0 连续编号**、且个数与创建 Shader 时声明的 `num_uniform_buffers` 一致（每阶段上限 4）；不一致时 `SDL_CreateGPUGraphicsPipeline` 直接以 E_INVALIDARG 失败 |
| 第三人称相机（跟随 + 沿视线避障 + **最小跟随距离托底防退化视图矩阵** + **"不得埋在实心内"安全网**） | **已实现** | `engine/render/camera.hpp`；避障经 `ITerrainQuery` 契约（`QueryObstruction` + **`IsSolid`**），由世界层实现；`kCameraMinDistance` 保证 `eye≠target`，避免 `lookAt` 归一化得 NaN。**`IsSolid` 必须包含可挖体积** ⇒ 游戏层用组合查询 `GameCameraQuery`（区域内以体积为准）；否则站在挖出的洞里的角色会把相机顶到旧地表之上 ⇒ 视角退化为俯视（人工实测第 7 轮 / T32） |
| 相机相对渲染（**每网格自带原点** + 浮点原点重定基） | **已实现** | 世界定位保持整数 / `double`；顶点只承载**网格局部**坐标，位置由逐网格顶点 uniform（网格原点 − 渲染原点）在顶点阶段补上 ⇒ **重定基只更新 uniform，零重传**（T41；此前重定基要重传全部网格，是"走动就卡"的根因） |
| 视锥体裁剪 | **部分实现** | [ADR 0008](adr/0008-sizes-precision-budget.md) 无关；落点 `engine/render/frustum.*`（Gribb–Hartmann 平面提取 + AABB 保守相交，7 项单测）+ `game/main.cpp` 每帧剔除。**注意（T39 实测）**：为**不丢阴影**，剔除判定用的是"物体 ∪ 其影子落点"（沿太阳方向扫掠）⇒ 低太阳仰角下余量很大，**本测试世界内只剔掉约 3%**；真正的提交侧大头是**阴影通道按级联全量重画**，按级联剔除见阶段计划 **T40（待开工）** |
| **HDR 离屏渲染 + 后处理通道**（曝光 / ACES 近似色调映射 / sRGB 编码） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md)；`engine/render/mesh_renderer.*`（主通道渲到 `R16G16B16A16_FLOAT` 离屏目标）+ `assets/shaders/tonemap.vert|.frag`（全屏三角形）；曝光经 `SetExposure` 来自 `engine/platform/settings.*`（`[0.1, 8.0]` 钳制）。**记账**：HDR 目标 8 B/px、深度 4 B/px，纹理总量计入 `RenderStats::textureBytes` 并**在启动日志按项打印**（实测 1280×720 全项：材质 5.67 + 深度 3.52 + HDR 7.03 + 阴影 48.00 + MSAA 38.67 = **102.89 MB**）；预算表见方案 §7.2.1 |
| **PBR 着色模型**（Cook-Torrance：GGX + Smith + Schlick，电介质 `F0 = 0.04`） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md) P2；`assets/shaders/mesh.frag`；粗糙度来自 roughness 贴图，**AO 只作用环境项**；参数经 `BuildMaterialUniform` 单入口投影 |
| 方向光 + **半球天空光**（参数全部来自配置，含颜色在 CPU 侧转线性） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md)；`engine/render/lighting_table.*`（`LightingUniform` 128 字节 + `BuildLightingUniform` 单入口投影）+ `assets/config/lighting.toml`；经**片元 uniform 槽 1** 上传（槽 0 为材质）。**暗部因此呈天空色而非死黑** |
| 阴影 / 级联阴影（CSM，3 级 2048² `D32_FLOAT` 深度数组 + **texel 2 的幂量化** + 3×3 PCF + **投射体扩展** + **级联过渡带混合**） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md)；纯函数 `engine/render/shadow_cascade.*`（分割 / 光空间矩阵 / **caster extension** / **`QuantizeTexelWorldSize`** / **`CascadeBlendWeight`** / `ShadowUniform` **320 B**）+ `mesh_renderer`（深度数组与深度通道）+ `assets/shaders/shadow.vert` / `shadow.frag`；参数来自 `lighting.toml [shadow]`（含 `caster_height_min`、`cascade_blend`）。**记账：48.00 MB**（已计入 `RenderStats::textureBytes`）。**投射体扩展**让高于视锥切片的高大投射体（塔）仍能投影；**texel 量化为 2 的幂 + 级联间平滑混合**消除"阴影随视角变化 / 边界突跳"（B6 / B8）。**注意**：SDL3_gpu 不允许 `fragment_shader == nullptr`，深度通道仍需一个空入口的片元着色器 |
| 指数高度雾（雾色默认取天空地平色） | **已实现** | `assets/shaders/mesh.frag`（光照之后、写 HDR 之前于**线性空间**施加；参数来自 `lighting.toml`，`enabled=false` 时整体跳过）。**大气散射 / 体积雾仍未开始** |
| 抗锯齿（MSAA 1× / 2× / 4× / 8×，档位可配；`R16G16B16A16_FLOAT` 多采样 + `SDL_GPU_STOREOP_RESOLVE`） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md) P3；`engine/render/mesh_renderer.*`（档位经 `SetMsaaSampleCount` 来自 `engine/platform/settings.*`，按硬件能力取不高于请求的受支持档；**档位 = 1 时不建多采样纹理，零额外开销**）+ `game/debug_overlay.hpp` 的多频细节法线（`mesh.frag`：第二频段 UV ×4、RNM、仅最高权重层）。**记账**：1080p 4× ≈ 95 MB（1280×720 实测 +38.67 MB） |
| 渲染开销统计（Draw Call / 三角形 / 纹理显存 / CPU 帧时间分解） | **已实现** | `engine/render/mesh_renderer.hpp`（通用 `RenderStats` + 纯函数 `EstimateTextureArrayBytes`，显存随纹理 / 目标创建释放增减，**含阴影深度数组**）+ `game/debug_overlay.*`（F1 面板展示；**Draw Call / 三角形已含阴影通道的绘制**）；**GPU pass 时间不可用**——SDL3_gpu 无时间戳查询 API，面板显式标注而非编造 |
| **帧尖峰打点（hitch 观测）** | **已实现** | `RenderStats::swapchainWaitMs`（`SDL_WaitAndAcquireGPUSwapchainTexture` 的等待**单独记账**，不再混进渲染提交耗时）+ `game/main.cpp`（帧 > **33 ms** 即 WARN：逻辑 / UI / 渲染提交 + draw call + 提交网格数 + 等交换链 + 主要受限在谁；节流 200 ms）+ `game/debug_overlay.*`（**P50 / P95 / P99** + 等交换链行）。判据与用法见 `references/performance-and-hitches.md` §2（阶段计划 T38） |
| 粒子 | **未开始** | — |
| 遮挡剔除 | **未开始** | 待收敛项 6 |

### 1.3 世界生成与表示

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| 确定性程序化生成（种子 + 整数坐标纯函数） | **已实现** | `world/generation/`；无 `rand()` / 时间 / 线程顺序 |
| 多层噪声（FastNoiseLite 封装，pimpl 隔离） | **已实现** | `world/generation/terrain_noise.*`（vendored，MIT） |
| **预设地图**（TOML：种子 / 范围 / 出生点 / 地形编辑区 flatten·raise·carve） | **已实现** | `world/generation/map_preset.*`；示例 `assets/maps/test_range.toml` |
| 高度场地表 tile（64×64 列、`int16` 1/16 格、65×65 采样） | **已实现** | `world/terrain/`；相邻 tile 边界**逐位相等无裂缝** |
| 地表网格化 + 梯度法线 | **已实现** | 同上 |
| 材质权重混合（按高度 + 坡度算 splat 权重，4 槽位）+ **四件套贴图**（albedo / normal / roughness / AO）+ **宏观变化** + **陡壁三平面投影** | **部分实现** | 权重**逐像素**重算（窄带 `smoothstep`，ADR 0009）；PBR 与四件套已落地（ADR 0010 P2），程序生成为**多尺度**（双频段）且**逐字节确定性**；**坡度驱动的三平面混合**（平坦处单次采样、陡面按 `|N|` 混合三轴投影）已落地，参数来自 `materials.toml [triplanar]`（`slope_min` / `slope_max` / `sharpness`），**随笔刷挖 / 堆自动跟随**；**仍无真实美术 PBR 资源**（程序生成占位），故为"部分实现"。材质显存 **5.67 MB**。**材质带的不变量已单测钉死**（任意 `(高度, 坡度)` 至少一层非零） |
| 地形笔刷：**平整填平 / 削平**（`Level`，向目标高度平滑收敛）+ **平滑爆破**（`Crater`，坑体 + 外环隆起，边界一阶连续）+ 球笔刷挖 / 堆；脏 tile 局部重网格 | **已实现** | `world/dig/terrain_brush.*`（`ApplyTerrainLevel` / `ApplyTerrainCrater` / `BrushFalloff` 纯函数）；参数表 `assets/config/brush.toml`（T26）。**T27 起不再绑定鼠标按键**（地形破坏改由光球爆炸触发，见下） |
| 可挖标记区域 | **部分实现** | **数据文件部分已实现**：`world/dig/dig_region.*`（`DigRegionTable`，含**包围盒向外吸附到 32 的整数倍**与包含判定）+ `assets/config/dig_regions.toml`（`mode` / `priority` / `min` / `max`，同 ADR 0006 的字段规格）；**程序化规则部分未开始**（ADR 0006 的噪声阈值 / 连通性约束） |
| 可挖体积（局部 SDF + 等值面网格化，洞穴） | **部分实现** | `world/dig/dig_volume.*`（33³ `int8` 密度块，**由高度场初始化**，球体平滑挖除，脏块重网格）+ `world/dig/volume_mesher.*`（**Naive Surface Nets**，顶点位置随密度连续变化、法线由密度梯度给出，ADR 0007/0008）。**物理碰撞已落地**（三角网静态体，[ADR 0012](adr/0012-collision-takeover-by-volumes.md)）；**仍缺**：流式加载与存档；`int8` 精度下的陡壁台阶感见 ADR 0008 的重审条件 |
| **破坏后的倒塌：结构整体刚体化**（支撑缺失 ⇒ 失支撑的**连通分量**整体变动态刚体，倾斜 / 旋转 / 碰撞由物理求解，落定后体素化回写为地形） | **已实现（阶段 1）** | `world/dig/volume_collapse.*`（支撑判定 → **6 邻域连通分量** → 抽出 + **体素补丁**）+ `engine/physics/physics_world.*`（动态凸包刚体）+ `game/rigid_collapse.*`（网格池 / 落定检测 / 回写）+ `assets/config/collapse.toml`（`schema_version = 3`）；决策见 [ADR 0015](adr/0015-structure-units-and-rigid-collapse.md)。**逐列下落模型已删除**（实测：地标塔基座被挖断后作为 1835~2072 体素的整体倾倒、落定倾角 68~88°）。**外观口径（T42）**：几何 = **与地形同一份 Surface Nets**（`BuildCollapseUnitMesh` → 抽出前后同源）、材质**随残骸搬走**（持久化）。**真实感（T43 / [ADR 0016](adr/0016-collapse-realism-impulse-material-debris.md)）**：**爆心冲量**（逐体素 `dir · impulse_speed · (1 − d/R)` ⇒ 整体初线速度 / 角速度，"被炸飞 + 翻滚"同源）、**材质化物理参数**（质量 = Σ 逐体素 `density`、摩擦 / 弹性 = 多数材质）、**小碎片清除 + `indestructible` 守卫**（"当炸没了"，不占刚体名额、不触发重网格）；**无冲量来源时自动退回人工倾斜**（配置 `initial_tilt_speed`）。**落地后的表示按材质分流（T46 / [ADR 0017](adr/0017-landing-by-material-rigid-vs-granular.md)，修订"落定后一律回写"）**：材质的 `rigid_debris`（**岩 = true**，草 / 土 / 沙 = false；按碎块**露出来的表面**材质判定，与渲染网格**同源**）决定 —— **刚性**碎块落定后**保留自身网格与姿态**（不体素化 ⇒ 形状与掉落中一致；**光球命中它时才惰性体素化** ⇒ 仍可挖）、**散体**碎块落定后体素化回写**与地面融合**并**接地沉降**（不允许"下方为空"的悬空体素）；配套：网格池 **16 槽**（岩石残骸长期占用）、**块碰撞体重建后唤醒相交残骸**（`PhysicsWorld::ActivateBody`）、池满时**最旧优先**腾位。**未做**：凹形分量的多凸包分解（凸包会**填平凹形** ⇒ "塔 + 平台板"连成一体时易被卡住、倾角偏小；T46 起该后果对保留中的岩石残骸**永久化**）、`structureId` 强制合并（阶段 2）、**落地摔断（二次碎裂）**（前置 = ADR 0013 的 `toughness`，登记为 T44） |
| **可破坏性判定**（材质坚固度 × 伤害预算的**逐格结算**；固定器物的破坏状态） | **未开始**（规范已落盘） | [ADR 0013](adr/0013-destructible-elements.md)：地形体量按「材质 `toughness` × 弹丸 `damage` × 换算系数」**自爆心向外逐格³ 扣减**（⇒ 混合材质时软的先被挖掉；不可破坏材质零改动）；固定器物 = **几何不可变** + 状态 `Intact → Broken`（本阶段仅"变黑"占位表现）。配置落点：`materials.toml`(+`toughness`) / `projectiles.toml`(+`damage`) / `destruction.toml`(新)；落地硬约束见 `references/destructible-elements.md`。**数值（换算系数、器物阈值）待确认** |
| **体积内表面材质**（挖出的洞按"被切开的是什么材质"着色） | **已实现**（2026-09-27；**同日 T42 增持久化**） | [ADR 0014](adr/0014-voxel-material-index.md)：材质 = 该列地表 splat 主槽位经「表层 → 次表层」映射（`subsurface`）后的结果 —— **默认纯函数派生、零额外存储**；`MeshVertex::material` 携带**槽位覆盖**（顶点属性 `location 2`、片元 `flat in`），片元遇覆盖时直接令该槽位权重为 1 ⇒ 挖开草地看到**土**、挖开山体（陡坡 ⇒ 岩）看到**岩**。**T42 起另加"懒分配"的体素材质持久化**（`VolumeBlock::material`，33³ / 块，`0xFF` = 未写入 ⇒ 回落列派生）：塌落残骸落地后**保留它原本的材质**（否则岩体落地变泥土）；未发生倒塌时 `MaterialBytes() == 0`。**未做**：深度分层（浅土深岩）、矿脉等可编辑体素材质（**数据通道已就绪**）—— 见 ADR 0014 的切换条件与修订记录 |
| 物件层（地表元素 / 建筑 / 建造） | **未开始** | 分层定义见 ADR 0004；**可破坏器物的状态模型已由 [ADR 0013](adr/0013-destructible-elements.md) 定义**（几何不可变 + `Intact` / `Broken`），实现未开始 |
| 流式加载 / 卸载（按距离） | **未开始** | 当前固定 3×3 tile |
| LOD 与接缝缝合 | **未开始** | 待收敛项 4 |

### 1.4 物理与角色

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| 物理引擎薄封装（Jolt，固定步长推进） | **已实现** | `engine/physics/`；公共头不含 Jolt 类型 |
| 地表高度场碰撞体（逐 tile，挖掘后按脏 tile 重建） | **已实现** | `world/terrain/terrain_collision.*`。**被体积接管的 tile 不再建此碰撞体**（[ADR 0012](adr/0012-collision-takeover-by-volumes.md)）：否则隐形高度场会把角色挡在洞口外 |
| **通用三角网静态碰撞体**（任意顶点 / 索引，用于可挖体积的等值面网格） | **已实现** | `engine/physics/physics_world.hpp`（`MeshDesc` / `AddMesh` / `UpdateMesh`，Jolt `MeshShape`；公共头不含 Jolt 类型） |
| **碰撞接管**（体积绘制的地表由体积提供碰撞；挖除后按脏块重建） | **已实现** | [ADR 0012](adr/0012-collision-takeover-by-volumes.md)；`world/dig/volume_collision.*` + `game/main.cpp`。判据与 ADR 0011 同源（同一份 `DigRegionTable`），粒度 = tile。**已知限制**：部分覆盖的 tile 仍保留高度场（该 tile 内的洞进不去）；体积无存档 ⇒ 重进游戏洞与碰撞体一起消失 |
| 通用静态盒体（供世界边界等使用；公共头不含 Jolt 类型） | **已实现** | `engine/physics/physics_world.hpp`（`AddStaticBox`）；由上层按地图范围推导放置 |
| 角色胶囊控制器（走 / 冲刺 / 跳 / 上坡 / 自动上台阶） | **已实现** | `CharacterVirtual`；重力 24 / **跳跃初速 7.20（由身高推导，最高点 = 身高 60% = 1.08 格）** / 最大坡度 50° / 上台阶 1.0 格 |
| 角色位置纠正（被地形埋住时顶回地表并清零速度） | **已实现** | `engine/physics/physics_world.hpp`（`SetCharacterPosition`）；静态高度场不会把角色顶出，须由上层在改地形后纠正 |
| 飞行模式（角色可控竖直移动，碰撞仍生效） | **已实现** | 当前作为测试设施（`FreeFly` 开关） |
| **动态凸包刚体**（点云 → 凸包形状；质量 / 摩擦 / 初始角速度；位姿与线 / 角速度读写；重力可设） | **已实现** | `engine/physics/physics_world.hpp`（`ConvexHullDesc` / `AddDynamicConvexHull` / `GetRigidBodyState` / `SetGravity`，Jolt `ConvexHullShape`；**位置以刚体局部原点为准**，内部折算 Jolt 的质心）。已知限制：凸包会**填平凹形** |
| 碰撞事件 / 可推物体 / 玩家可交互刚体 | **未开始** | — |
| 物理的世界坐标精度方案（大坐标） | **未开始** | **待收敛项 7**（`joltphysics` 为单精度） |

### 1.5 实体、任务与数据

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| ECS（EnTT） | **已引入未使用** | 依赖已在 `vcpkg.json`，代码尚未使用 |
| 任务系统 / 后台线程（enkits） | **已引入未使用** | 同上；当前生成与网格化仍在主线程 |
| 配置表加载（TOML + toml++，启动期校验、非法即报错） | **已实现** | `world/terrain/material_table.*`、`world/generation/map_preset.*`、**`engine/render/lighting_table.*`**、**`world/dig/terrain_brush.*`（`brush.toml`）**、`engine/platform/settings.*` |
| 音频（播放 / 混音 / 音源） | **未开始** | 仅保留**唯一增益入口** `engine/platform/settings.hpp::ApplyMasterVolumeGain`（设置值已接通，**当前无声源 ⇒ 听不到**）；要能听到还需音频流 + 混音器 + 音源 |
| 存档 / 读档（只存脏数据，自定义二进制 + zstd） | **未开始** | 内容模型见 ADR 0006 / `references/save-and-serialization.md` |
| 资源管理（纹理 / 模型加载） | **未开始** | `stb_image` 已引入未使用；模型导入（Assimp）未引入 |

### 1.6 调试与工程质量

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| UI 可交互（ImGui + SDL3/SDL3_gpu 后端，事件转发已接） | **已实现** | `game/debug_overlay.*`、`game/system_panel.*`；面板交互与游戏输入抑制分离（`game/gameplay_input.hpp`） |
| UI 字体解析与标签缝（命中 CJK 字体用中文，否则**整表英文、绝不缺字**） | **已实现** | `game/ui_font.*`（三级解析：仓库 `assets/fonts/` → 系统 CJK → 无）、`game/ui_text.hpp`（唯一取词缝；有单测 + 源码扫描防绕过） |
| UI 主题（统一暗色样式，单一样式入口） | **已实现** | `game/ui_theme.*`（`ApplyUiTheme`） |
| 单元测试 | **已实现** | `tests/`，**281 项**（`ctest --preset debug`，**281/281 全绿**；含 T36~T39 的分步初始化等价性 / 分步贴图逐字节一致 / 破坏队列确定性 / 视锥剔除，T33 的连通分量分组 / 整体倾倒 / 落定回写 / 动态凸包刚体，T42 的区域网格化 ≡ 整块 / 整体网格 = 原地形子集 / 材质持久化与搬运，T43 的爆心冲量方向与衰减 / 确定性 / 无冲量兜底 / 材质化质量与摩擦 / 多数票 / 小碎片清除阈值边界 / 不可破坏守卫，以及 **T46 的表面材质分流（含同票与"体积土占多数、皮上有岩 ⇒ 仍刚性"回归）/ 散体接地沉降不悬空 / 刚性不回写不沉降 / OBB 命中判据随姿态旋转 / `ActivateBody` 容忍非法句柄 / 独立碎块与"连着未塌实心"的碎块网格均闭合**等） |
| **卡顿消除（hitch-free）施工标准** | **已实现**（规范） | `.trae/skills/voxel-engine-dev-standards/references/performance-and-hitches.md`（三类卡顿判据 / 四层手段与业界参照 / 观测义务 / 自查清单 / 伪优化反例）+ `SKILL.md` 第四节「卡顿消除」与 DoD 勾选。**这是规范、不是引擎能力**：它约束今后所有性能改动（见阶段计划 M8） |
| 结构门禁（禁止标识符扫描） | **已实现** | `.trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1`（当前扫 **115** 文件，0 违规） |
| CI（Windows debug/release 全绿） | **部分实现** | Linux 作业受 runner 系统依赖影响，见阶段计划 I1 |

## 2. 引擎**不**包含什么（分层边界）

| 不属于引擎层 | 归属 |
| --- | --- |
| 玩法规则、世界生成的具体内容配方、必备元素清单 | [游戏设计](game-design.md) |
| 主角身份、成长、收集与交互规则 | [游戏设计](game-design.md) |
| NPC 种类与行为约定 | [NPC 行为逻辑](npc-behavior.md) |
| 题材、地名、种族、历史、命名 | [世界观设定](world-setting.md) |
| 技术选型与理由 | `docs/adr/` |
| 任务顺序与验收 | `docs/plans/<阶段>.md` |

## 3. 新增引擎能力的登记规则

1. **先登记后动手**：先在 §1 找到所属小节**补一行**（状态 `未开始`），再实现，完成后回填状态与落点。
2. **一行四件事**：**是什么** / **状态** / **落点模块** / （必要时）**依据的 ADR**。
3. **能力与玩法分开**：引擎能力只写"能做什么"，不写"我们的游戏用它做什么"——后者写游戏设计文档。
4. **涉及选型**（如新的网格化算法、导航表示）必须先有 **ADR**，并登记到 SKILL「待收敛项」表。
5. **落地才算数**：`已引入未使用` 与 `已实现` 必须区分，不得把"依赖装了"写成"能力有了"。
