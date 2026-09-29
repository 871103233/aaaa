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
| 通用网格渲染路径（顶点/索引缓冲、相机常量、**逐网格原点偏移**、索引绘制） | **已实现** | `engine/render/mesh_renderer.hpp`；顶点只承载**网格局部**坐标，绘制时按网格推送"网格原点 − 渲染原点"（顶点 uniform `set = 1` / slot 0，含推送去重）。**T78（2026-09-29）起支持按网格的"深度偏移变体"**：`UploadMesh(..., depthBiased=true)` 的网格走一条**带光栅化 depth bias 的主通道管线变体**（用于**层间共面重叠**，即地表 tile 与可挖体积网格在接管边界环上重合的 z-fighting），`DrawMeshes` 按标记聚成连续区间绘制、绑定次数只随标记切换增长（≤ 2，绝不逐网格绑定）；**阴影通道不使用**该变体 |
| 纹理数组（多层 `SDL_GPUTexture`，每层独立 mipmap；`sampler2DArray` 采样） | **已实现** | 同上；地表材质按 ADR 0009 使用（**禁止**改用纹理图集，见 `references/meshing-and-render.md` §3） |
| 片元 uniform 块（材质参数，std140；CPU→GPU **唯一投影入口**） | **已实现** | `world/terrain/material_table.hpp`（`MaterialUniform` / `BuildMaterialUniform`，`static_assert` 钉死布局）+ `engine/render/mesh_renderer.hpp`（上传） |
| 程序生成占位材质贴图（**材质四件套 albedo / normal / roughness / AO + 宏观变化**，确定性、可平铺） | **已实现** | `world/terrain/material_textures.*`（ADR 0009 / ADR 0010 P2）；5 张 `R8G8B8A8_UNORM` 纹理数组（四件套各 4 层 + macro **1 层**）× 256²，含 mip 约 **5.67 MB** 显存。多尺度（双频段）且**逐字节确定性** |
| 叠加层接口（`IRenderOverlay`，用于调试 UI） | **已实现** | 同上 |
| 动态网格顶点刷新（就地更新定长网格顶点；稳态**零堆分配**、不建 GPU 资源） | **已实现** | `engine/render/mesh_renderer.hpp`（`UpdateMeshVertices`，同时更新该网格登记的原点）；供每帧移动的网格（角色代理体 / 光球）使用 |
| **变长网格几何就地更新**（顶点 + 索引都可变；**只上传用到的前缀**；空网格 = 不可见 ⇒ 零上传零绘制） | **已实现** | `engine/render/mesh_renderer.hpp`（`UpdateMeshGeometry` + `MeshResources::usedIndexCount`）；供倒塌整体的网格池使用（T42：槽位按容量一次建好，之后只写用到的前缀）。**T76（2026-09-29）起它同时是"网格上传"的标准路径**：`UploadMesh` 改为"**按容量建缓冲（可预留）→ 复用常驻 staging → 提交即走**"，**不再做 `SDL_WaitForGPUFences` 同步等待**；地表 tile / 可挖体积块的**重网格**走该快路径（容量不足时调用方重建并 **WARN**，不静默）。**T82（2026-09-30）**：口径澄清 —— `reserveVertexCount` / `reserveIndexCount` 是**总容量**（实现取 `max(本次数量, reserve)`），**不是"额外预留"**（传等量值 = 不预留）；地表 tile **只有索引数**会随层间接管升降（顶点数是常数 65×65）⇒ tile 上传时**只要当前是"部分地表"就按满地表上界 `kTerrainTileIndexCount` 预留**（全量 289 tile 不预留：那样约 +28 MB 未记账几何显存；而"部分地表"只可能落在可挖区内 ⇒ **至多 16 个**、约 1.6 MB）⇒ 实测**跨界往返兜底重建 82 → 0 次** |
| **自发光网格**（片元 uniform **槽 3**：`rgb` = 自发光颜色、`a` = 强度；`0` = 普通地表网格） | **已实现** | `engine/render/mesh_renderer.hpp`（`UploadMesh(..., bool emissive)` + `SetEmissiveColor`，`DrawMeshes` **逐网格**推送）+ `assets/shaders/mesh.frag`（在**雾之后**叠加，使远处光球不被雾洗掉）。用途：光球弹丸。**踩坑**：SDL_gpu 的 `set = 3` uniform 绑定必须**从 0 连续编号**、且个数与创建 Shader 时声明的 `num_uniform_buffers` 一致（每阶段上限 4）；不一致时 `SDL_CreateGPUGraphicsPipeline` 直接以 E_INVALIDARG 失败 |
| 第三人称相机（跟随 + 沿视线避障 + **最小跟随距离托底防退化视图矩阵** + **"不得埋在实心内"安全网**） | **已实现** | `engine/render/camera.hpp`；避障经 `ITerrainQuery` 契约（`QueryObstruction` + **`IsSolid`**），由世界层实现；`kCameraMinDistance` 保证 `eye≠target`，避免 `lookAt` 归一化得 NaN。**`IsSolid` 必须包含可挖体积** ⇒ 游戏层用组合查询 `GameCameraQuery`（区域内以体积为准）；否则站在挖出的洞里的角色会把相机顶到旧地表之上 ⇒ 视角退化为俯视（人工实测第 7 轮 / T32） |
| 相机相对渲染（**每网格自带原点** + 浮点原点重定基） | **已实现** | 世界定位保持整数 / `double`；顶点只承载**网格局部**坐标，位置由逐网格顶点 uniform（网格原点 − 渲染原点）在顶点阶段补上 ⇒ **重定基只更新 uniform，零重传**（T41；此前重定基要重传全部网格，是"走动就卡"的根因） |
| 视锥体裁剪 | **部分实现** | [ADR 0008](adr/0008-sizes-precision-budget.md) 无关；落点 `engine/render/frustum.*`（Gribb–Hartmann 平面提取 + AABB 保守相交，7 项单测）+ `game/main.cpp` 每帧剔除。**注意（T39 实测）**：为**不丢阴影**，剔除判定用的是"物体 ∪ 其影子落点"（沿太阳方向扫掠）⇒ 低太阳仰角下余量很大，**本测试世界内只剔掉约 3%**；真正的提交侧大头是**阴影通道按级联全量重画**，按级联剔除见阶段计划 **T40（待开工）** |
| **HDR 离屏渲染 + 后处理通道**（曝光 / ACES 近似色调映射 / sRGB 编码） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md)；`engine/render/mesh_renderer.*`（主通道渲到 `R16G16B16A16_FLOAT` 离屏目标）+ `assets/shaders/tonemap.vert|.frag`（全屏三角形）；曝光经 `SetExposure` 来自 `engine/platform/settings.*`（`[0.1, 8.0]` 钳制）。**记账**：HDR 目标 8 B/px、深度 4 B/px，纹理总量计入 `RenderStats::textureBytes` 并**在启动日志按项打印**（实测 1280×720 全项：材质 5.67 + 深度 3.52 + HDR 7.03 + 阴影 48.00 + MSAA 38.67 = **102.89 MB**）；预算表见方案 §7.2.1 |
| **PBR 着色模型**（Cook-Torrance：GGX + Smith + Schlick，电介质 `F0 = 0.04`） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md) P2；`assets/shaders/mesh.frag`；粗糙度来自 roughness 贴图，**AO 只作用环境项**；参数经 `BuildMaterialUniform` 单入口投影 |
| 方向光 + **半球天空光**（参数全部来自配置，含颜色在 CPU 侧转线性） | **已实现** | [ADR 0010](adr/0010-render-quality-pipeline.md)；`engine/render/lighting_table.*`（`LightingUniform` 128 字节 + `BuildLightingUniform` 单入口投影）+ `assets/config/lighting.toml`；经**片元 uniform 槽 1** 上传（槽 0 为材质）。**暗部因此呈天空色而非死黑**。**T67 起它同时是 IBL 的回落路径**（未配置 HDRI / 资源缺失 / 烘焙失败时按 `fogParams.z = 0` 走这条） |
| **环境贴图与 IBL**（天空 + 漫反射 irradiance + 预过滤高光 + BRDF LUT；**加载期烘焙、运行期只采样**） | **已实现** | [ADR 0021](adr/0021-environment-ibl.md)（**超出** ADR 0010 的"半球天空光"质量线）；`engine/render/environment.*`（尺寸 / 级数 / 粗糙度↔mip 映射 / `HalfFromFloat` 的纯函数，带单测）+ `engine/render/mesh_renderer.*`（`BakeEnvironment` 三条全屏烘焙 pass + 天空管线 + 环境纹理绑定；全屏通道抽成 `DrawFullscreenPass` 供色调映射共用）+ `assets/shaders/sky.frag` / `ibl_irradiance.frag` / `ibl_prefilter.frag` / `ibl_brdf_lut.frag`（顶点阶段复用 `tonemap.vert`）+ `assets/config/lighting.toml` 的**可选** `[environment]` 段。**记账**：2048×1024 HDRI + 32×16 irradiance + 6 级 128×64 预过滤 + 256² LUT ≈ **16.59 MB**；**烘焙实测 165 ms**（ADR 0021 后果 2 已回填）。**缺失 / 失败 ⇒ WARN + 回落半球天空光**（不静默、不崩） |
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
| 材质权重混合（按高度 + 坡度算 splat 权重，4 槽位）+ **四件套贴图**（albedo / normal / roughness / AO）+ **宏观变化** + **陡壁三平面投影** | **已实现** | 权重**逐像素**重算（窄带 `smoothstep`，ADR 0009）；PBR 与四件套已落地（ADR 0010 P2）；**T66（2026-09-29）起默认使用真实 CC0 美术贴图**：`materials.toml` 的 `[textures]` 段 + `MaterialTextureAssetLoader`（分步加载、整数倍 box 降采样到 1024²）+ 着色器 `textureMode` 分支（真实模式 = 贴图取绝对值、层色置 1、宏观变化改用该层 albedo 放大采样）；**缺资源时 WARN 回落程序生成占位贴图**（旧路径逐字节不变）。**坡度驱动的三平面混合**（平坦处单次采样、陡面按 `|N|` 混合三轴投影）已落地，参数来自 `materials.toml [triplanar]`，**随笔刷挖 / 堆自动跟随**。材质显存：真实贴图 **85.33 MB**（1024²×16 层含 mip）；程序生成占位 **5.67 MB**（256²）。**材质带的不变量已单测钉死**（任意 `(高度, 坡度)` 至少一层非零） |
| 地形笔刷：**平整填平 / 削平**（`Level`，向目标高度平滑收敛）+ **平滑爆破**（`Crater`，坑体 + 外环隆起，边界一阶连续）+ 球笔刷挖 / 堆；脏 tile 局部重网格 | **已实现** | `world/dig/terrain_brush.*`（`ApplyTerrainLevel` / `ApplyTerrainCrater` / `BrushFalloff` 纯函数）；参数表 `assets/config/brush.toml`（T26）。**T27 起不再绑定鼠标按键**（地形破坏改由光球爆炸触发，见下） |
| 可挖标记区域 | **部分实现** | **数据文件部分已实现**：`world/dig/dig_region.*`（`DigRegionTable`，含**包围盒向外吸附到 32 的整数倍**与包含判定）+ `assets/config/dig_regions.toml`（`mode` / `priority` / `min` / `max`，同 ADR 0006 的字段规格）；**程序化规则部分未开始**（ADR 0006 的噪声阈值 / 连通性约束）。**2026-09-29 起：标记同时决定"哪里可挖"与"是否常驻"** ⇒ 竖向带宽与玩家窗口见 [ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md)（该 ADR 触发 ADR 0006 的「何时重新审视」第 1 条） |
| 可挖体积（局部 SDF + 等值面网格化，洞穴） | **部分实现** | `world/dig/dig_volume.*`（33³ `int8` 密度块，**由高度场初始化**，球体平滑挖除，脏块重网格）+ `world/dig/volume_mesher.*`（**Naive Surface Nets**，顶点位置随密度连续变化、法线由密度梯度给出，ADR 0007/0008）。**物理碰撞已落地**（三角网静态体，[ADR 0012](adr/0012-collision-takeover-by-volumes.md)）；**仍缺**：存档（**内存内的流式建 / 卸已由 T61 落地**，见下表"流式加载 / 卸载"行）；`int8` 精度下的陡壁台阶感见 ADR 0008 的重审条件。**T55 / [ADR 0019](adr/0019-ambiguous-cell-vertex-splitting.md)**：**歧义 cell / 歧义面**（实体侧角分成 ≥2 个连通分量、网格面四角成棋盘格）按**实体侧连通分量拆顶点** ⇒ 外观网格重新成为**流形**（`CountBoundaryEdges == 0`）；**单分量 cell 的输出逐位不变** |
| **破坏后的倒塌：结构整体刚体化**（支撑缺失 ⇒ 失支撑的**连通分量**整体变动态刚体，倾斜 / 旋转 / 碰撞由物理求解，落定后按材质分流：散体回写 / 刚性保留） | **已实现（阶段 1）** | `world/dig/volume_collapse.*`（支撑判定 → **连通域洪泛** → **6 邻域连通分量** → **按材质拆子块** → 抽出 + **体素补丁**）+ `engine/physics/physics_world.*`（动态凸包刚体，可带**初始姿态**）+ `game/rigid_collapse.*`（网格池 / 落定检测 / 回写 / **就地雕刻**）+ `assets/config/collapse.toml`（`schema_version = 3`，**T49 只改 scope、数值未动**）；决策见 [ADR 0015](adr/0015-structure-units-and-rigid-collapse.md)。**逐列下落模型已删除**（实测：地标塔基座被挖断后作为 1835~2072 体素的整体倾倒、落定倾角 68~88°）。**求解 scope = 连通域（T49 / [ADR 0018](adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一）**：scope 由"被改动采样周围一个固定窗口"改为"**以被改动采样为起点的实心连通域**"（窗口只向**触界的那几侧**翻倍扩张；**累计**采样超 `kMaxRegionSamples` 8M ⇒ 告警 + 保守回退固定窗口）⇒ 长条两端支点都被炸断后**中段不再悬空**（实测：每次爆炸打 `连通域 N 体素（已按连通域收窄）`，N = 1808~6753）。**外观口径（T42）**：几何 = **与地形同一份 Surface Nets**（`BuildCollapseUnitMesh` → 抽出前后同源）、材质**随残骸搬走**（持久化）。**真实感（T43 / [ADR 0016](adr/0016-collapse-realism-impulse-material-debris.md)）**：**爆心冲量**（逐体素 `dir · impulse_speed · (1 − d/R)` ⇒ 整体初线速度 / 角速度）、**材质化物理参数**（质量 = Σ 逐体素 `density`、摩擦 / 弹性 = 多数材质）、**小碎片清除 + `indestructible` 守卫**；**无冲量来源时自动退回人工倾斜**（`initial_tilt_speed`）。**落地后的表示按材质分流（T46 / [ADR 0017](adr/0017-landing-by-material-rigid-vs-granular.md)；T50 起判据 = **子块自己的材质**）**：`rigid_debris`（**岩 = true**，草 / 土 / 沙 = false）决定 —— **刚性**碎块落定后**保留自身网格与姿态**（不体素化 ⇒ 形状与掉落中一致）、**散体**碎块落定后体素化回写**与地面融合**并**接地沉降**（**T51 起：降不到支撑的残留一律清除**（`removedFloatingVoxels` + WARN）⇒ 区域内**绝不允许**"下方为空"的悬空体素）；配套：网格池 **16 槽**、**块碰撞体重建后唤醒相交残骸**（`PhysicsWorld::ActivateBody`）、池满时**最旧优先**腾位。**破坏时不切换表示（T50 / ADR 0018 决策二）**：命中动态刚体 ⇒ 在碎块的**自身体素补丁**上做**与地形同口径**的球体挖除 + **同一份 Surface Nets** 重网格 + 按剩余体素**在当前姿态下原地重建刚体**（复用同一网格槽位）⇒ **未被挖到的区域顶点逐位不变**（可证伪的不变量）；剩余 ≤ `debris_delete_max_voxels` / 凸包点数 < 4 / 重建失败 ⇒ **删除整个整体**兜底；命中路径的"惰性体素化"**已下线**（腾位仍走回写 —— 它不是破坏事件）。**未做**：凹形分量的多凸包分解（凸包会**填平凹形** ⇒ "塔 + 平台板"连成一体时易被卡住；T46 起该后果对保留中的岩石残骸**永久化**）、`structureId` 强制合并（阶段 2）、**落地摔断（二次碎裂）**（前置 = ADR 0013 的 `toughness`，登记为 T44）、**持久结构图**（T49 的切换条件：洪泛成为实测瓶颈时上）。**上述长尾一律「冻结为已知限制」（2026-09-29 所有者确认）**：切换条件 = ① 战斗设计明确依赖破坏的真实感，或 ② 成为实测的玩家可见缺陷（见 [game-design](game-design.md) 的「结构塌陷」行）。**外观网格闭合（T47 自检 / T55 修复）**：该路径原先偶发 `整体外观网格缺面（T47 自检）… 边界边 N 条`（非流形捏合），现已由 [ADR 0019](adr/0019-ambiguous-cell-vertex-splitting.md) 的歧义处置消除 |
| **可破坏性判定**（材质坚固度 × 伤害预算的**逐格结算**；固定器物的破坏状态） | **部分实现**（2026-09-27：**地形体量已完成**；器物部分待层 ③） | [ADR 0013](adr/0013-destructible-elements.md)：地形体量按「材质 `toughness` × 弹丸 `damage` × 换算系数」**自爆心向外逐格³ 扣减**（⇒ 混合材质时软的先被挖掉；**不可破坏材质零改动、且不消耗预算**）。**已落地**：`DigVolumeWorld::CarveByDamage`（确定序 + 整数点 + 空 / 不可破坏格不消耗预算）+ `world/dig/destruction_table.*` + `materials.toml`（**v5**，必填 `toughness`：草 2 / 土 3 / 岩 5 / 沙 2）+ `projectiles.toml`（**v2**，`damage = 10`）+ `destruction.toml`（换算系数 **271**）⇒ 泥 r≈6（单测钉死）。**T52（2026-09-28，所有者指定）**：**岩层 = `indestructible = true` ⇒ 完全无法击毁 / 无法挖洞**（同一开关同时覆盖地形挖洞与 T50 的碎块补丁雕刻；含岩分量不被小碎片清除），但**密度 / 碰撞体 / 命中判定完全不变** ⇒ 光球仍被岩石挡住、岩石仍会失去支撑并作为刚体倒塌；**区域外**地表爆破改**整片判定**（整片皆不可破坏 ⇒ 整坑不挖）。**未做**：固定器物（几何不可变 + 状态 `Intact → Broken` + 变黑占位）待 [ADR 0004](adr/0004-hybrid-layered-world-representation.md) **层 ③（物件与建造层）**；`destruction.toml` 的器物字段**只解析 + 校验 + 打日志、尚未消费**。落地硬约束见 `references/destructible-elements.md`。**地表爆破（区域外）**：逐列区分岩 / 土无法在高度场剖面上表达 ⇒ 采"整片皆不可破坏则整坑不挖"（ADR 0013 §二.5 的收紧） |
| **体积内表面材质**（挖出的洞按"被切开的是什么材质"着色） | **已实现**（2026-09-27；**同日 T42 增持久化**） | [ADR 0014](adr/0014-voxel-material-index.md)：材质 = 该列地表 splat 主槽位经「表层 → 次表层」映射（`subsurface`）后的结果 —— **默认纯函数派生、零额外存储**；`MeshVertex::material` 携带**槽位覆盖**（顶点属性 `location 2`、片元 `flat in`），片元遇覆盖时直接令该槽位权重为 1 ⇒ 挖开草地看到**土**、挖开山体（陡坡 ⇒ 岩）看到**岩**。**T42 起另加"懒分配"的体素材质持久化**（`VolumeBlock::material`，33³ / 块，`0xFF` = 未写入 ⇒ 回落列派生）：塌落残骸落地后**保留它原本的材质**（否则岩体落地变泥土）；未发生倒塌时 `MaterialBytes() == 0`。**未做**：深度分层（浅土深岩）、矿脉等可编辑体素材质（**数据通道已就绪**）—— 见 ADR 0014 的切换条件与修订记录 |
| 物件层（地表元素 / 建筑 / 建造） | **未开始** | 分层定义见 ADR 0004；**可破坏器物的状态模型已由 [ADR 0013](adr/0013-destructible-elements.md) 定义**（几何不可变 + `Intact` / `Broken`），实现未开始 |
| 流式加载 / 卸载（按距离） | **已实现（按玩家窗口常驻；T61 起在游戏内生效；T80 起预取、T81 起建块下沉 worker）**（2026-09-29） | `world/streaming/dig_volume_residency.*`（[ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二 / 五）—— 玩家窗口（tile ± 2；**T73 起中心 tile 带 16 格滞回** ⇒ 站在 tile 边界上的亚格级抖动不再触发 63 块重建；**T80 起常驻半径 = 活动半径 + 1 tile 预取环**，见 [ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二第三次修订）→ **纯函数集合差**（要建 / 要卸 / **脏块留驻** / 超限淘汰最远者 + WARN）→ **分帧推进**的 `CreateBlock` / `UnloadBlock`；`DigVolumeWorld` 可 `BeginInitFromHeightField(子集)` ⇒ **启动只常驻窗口**。**T61 接线**：`game/main.cpp` 的常驻集合改为**按块坐标索引的动态表**（键存在 == 常驻）+ **接管判据随常驻集合**（ADR 0011 的输入不再是静态区域）+ 每帧 `Update` / `Step(1)` + 建卸引起的碰撞体与 tile 重网格走既有**延后队列**（分帧）。**T81 接线**：建块改为**提交给 worker 任务池**（主线程只采快照 + 收包 + 上传），单块 22 ms 的计算不再占用渲染帧（[ADR 0022](adr/0022-volume-build-worker-pipeline.md)）。**实测（debug）**：启动只初始化窗口（本图窗口 ⊇ 区域 ⇒ 441 块）；**卸载几乎免费**（108 次卸载期间零帧尖峰、逻辑 ≈ 0.3 ms/帧）。**未做**：地形 tile 的流式（当前 tile 全量常驻，`tile_radius = 8` ⇒ 289 tile）；**存档**（脏块因此采取"常驻不卸载"的过渡口径）；LOD / 遮挡剔除 / 大视距仍延后（待收敛项 4 / 6） |
| **块构建任务池**（可挖体积块：填密度 + Surface Nets 在 worker 上跑） | **已实现**（2026-09-29 / T81） | `world/streaming/volume_build_pipeline.*` + `engine/core/task_scheduler.*`（enkits）；**快照式纯函数**：`DigVolumeWorld::CaptureBlockBuildInput`（主线程采齐全部输入）→ `BuildBlockFromInput`（worker，纯函数）→ `InstallBuiltBlock`（主线程单写者安装）。**确定性由单测逐位钉住**（`DigVolumeWorker.PipelineBuildMatchesMainThreadBuildBitForBit`：密度 / 网格 / 分类与同步路径逐值一致，覆盖"邻块存在 / 不存在"两种壳层来源）。**已知限制**：仅运行期常驻建块走 worker（加载期的分步初始化仍同步，见 ADR 0022 决策一） |
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
| **任务调度 / 后台线程**（enkits 1.12） | **已实现**（2026-09-29 T81 起真正启用） | `engine/core/task_scheduler.*`（`ParallelTask` + `TaskScheduler`；**本工程唯一引用 enkits 的地方**，头只在 `.cpp` 内出现）+ `world/streaming/volume_build_pipeline.*`（块的构建任务池）。首个消费者 = **可挖体积块构建**（`FillBlockDensity + MeshBlock` 下沉 worker，快照式纯函数，见 [ADR 0022](adr/0022-volume-build-worker-pipeline.md)）。**线程池不可用时自动回落同步执行**（WARN 一次，不静默）。**已知限制**：生成 / 光照 / 存档 IO 仍在主线程（各自的"下沉 worker"未开始） |
| 配置表加载（TOML + toml++，启动期校验、非法即报错） | **已实现** | `world/terrain/material_table.*`、`world/generation/map_preset.*`、**`engine/render/lighting_table.*`**、**`world/dig/terrain_brush.*`（`brush.toml`）**、`engine/platform/settings.*` |
| 音频（播放 / 混音 / 音源） | **未开始** | 仅保留**唯一增益入口** `engine/platform/settings.hpp::ApplyMasterVolumeGain`（设置值已接通，**当前无声源 ⇒ 听不到**）；要能听到还需音频流 + 混音器 + 音源 |
| 存档 / 读档（只存脏数据，自定义二进制 + zstd） | **未开始** | 内容模型见 ADR 0006 / `references/save-and-serialization.md` |
| 资源管理（纹理 / 模型加载） | **部分实现**（2026-09-29 T66：**纹理加载已有消费者**；模型导入未开始） | **纹理**：`engine/render/texture_loader.*`（LDR → RGBA8 / HDR `.hdr` → **线性 RGB32F**；**解码前**尺寸守卫；失败即抛、不静默回退；接 `stb_image`）。**消费者（T66）**：`world/terrain/material_textures.*` 的 `MaterialTextureAssetLoader`（分步：每步一张贴图 ⇒ 不冻结画面）+ `ResolveMapFile`（固定扩展名顺序）+ `DownscaleBoxRgba8`（**整数倍** box 降采样，确定性）⇒ 地表四件套换真实 CC0 贴图（资源不入库，见 `assets/textures/` 行与 `NOTICE.md` 台账）。**模型导入**（Assimp）未引入（T68） |

### 1.6 调试与工程质量

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| UI 可交互（ImGui + SDL3/SDL3_gpu 后端，事件转发已接） | **已实现** | `game/debug_overlay.*`、`game/system_panel.*`；面板交互与游戏输入抑制分离（`game/gameplay_input.hpp`） |
| UI 字体解析与标签缝（命中 CJK 字体用中文，否则**整表英文、绝不缺字**） | **已实现** | `game/ui_font.*`（三级解析：仓库 `assets/fonts/` → 系统 CJK → 无）、`game/ui_text.hpp`（唯一取词缝；有单测 + 源码扫描防绕过） |
| UI 主题（统一暗色样式，单一样式入口） | **已实现** | `game/ui_theme.*`（`ApplyUiTheme`） |
| 单元测试 | **已实现** | `tests/`，**360 项**（`ctest --preset debug -j`，**360/360 全绿**；**T79~T81 新增 5 项**：`TerrainQuery.ColumnLookupIsDirectAndCoversSharedBoundaryColumns` / `TerrainQuery.ColumnLookupFloorsNegativeColumns`（列查询 O(1) 且共享边界列不漏）、`DigVolumeResidency.ResidencyWindowIsASupersetOfTheActivityWindow` / `DigVolumeResidency.PrefetchRemovesTheCreateBurstWhenCrossingATileBoundary`（T80 预取：跨界零建块，含"环宽 0 ⇒ 建 4 块"的对照）、`DigVolumeWorker.PipelineBuildMatchesMainThreadBuildBitForBit`（T81：worker 构建与主线程构建**逐位一致**，含邻块存在 / 不存在两条壳层路径）；另含 T60 的常驻调度、T59 竖向带宽、T57 纹理加载、T55 歧义面拆顶点、T36~T39、T33、T42、T43、T46 等既有用例） |
| **卡顿消除（hitch-free）施工标准** | **已实现**（规范） | `.trae/skills/voxel-engine-dev-standards/references/performance-and-hitches.md`（三类卡顿判据 / 四层手段与业界参照 / 观测义务 / 自查清单 / 伪优化反例）+ `SKILL.md` 第四节「卡顿消除」与 DoD 勾选。**这是规范、不是引擎能力**：它约束今后所有性能改动（见阶段计划 M8） |
| 结构门禁（禁止标识符扫描） | **已实现** | `.trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1`（2026-09-29 T81：当前扫 **130** 文件，0 违规） |
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
