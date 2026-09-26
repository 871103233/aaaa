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

### 1.2 渲染

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| GPU 抽象（SDL3_gpu 薄封装） | **已实现** | `engine/render/` |
| 双格式 Shader 管线（GLSL → SPIR-V **与** DXIL，两者都必须产出） | **已实现** | `cmake/Shaders.cmake`（ADR 0002） |
| 通用网格渲染路径（顶点/索引缓冲、相机 UBO、索引绘制） | **已实现** | `engine/render/mesh_renderer.hpp` |
| 纹理数组（多层 `SDL_GPUTexture`，每层独立 mipmap；`sampler2DArray` 采样） | **已实现** | 同上；地表材质按 ADR 0009 使用（**禁止**改用纹理图集，见 `references/meshing-and-render.md` §3） |
| 片元 uniform 块（材质参数，std140；CPU→GPU **唯一投影入口**） | **已实现** | `world/terrain/material_table.hpp`（`MaterialUniform` / `BuildMaterialUniform`，`static_assert` 钉死布局）+ `engine/render/mesh_renderer.hpp`（上传） |
| 程序生成占位材质贴图（albedo + 法线，确定性、可平铺） | **已实现** | `world/terrain/material_textures.*`（ADR 0009）；4 层 × 256² × `R8G8B8A8_UNORM`，albedo + 法线各一张，含 mip 约 **2.67 MB** 显存（第 0 级 1.00 MB/张） |
| 叠加层接口（`IRenderOverlay`，用于调试 UI） | **已实现** | 同上 |
| 动态网格顶点刷新（就地更新定长网格顶点；稳态**零堆分配**、不建 GPU 资源） | **已实现** | `engine/render/mesh_renderer.hpp`（`UpdateMeshVertices`）；供每帧移动的网格（角色代理体）使用 |
| 第三人称相机（跟随 + 沿视线避障 + **最小跟随距离托底防退化视图矩阵**） | **已实现** | `engine/render/camera.hpp`；避障经 `ITerrainQuery` 契约（由世界层实现）；`kCameraMinDistance` 保证 `eye≠target`，避免 `lookAt` 归一化得 NaN |
| 相机相对渲染（浮点原点重定基） | **已实现** | 世界定位保持整数 / `double`，上传 GPU 前转相机相对 `float` |
| 视锥体裁剪 | **未开始** | 目前全部网格随手提交 |
| 阴影 / 级联阴影（CSM） | **未开始** | 方案见 `tech-plan-v2.0.md` §4 |
| 天空 / 雾 / 大气 | **未开始** | — |
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
| 材质权重混合（按高度 + 坡度算 splat 权重，4 槽位） | **部分实现** | 权重已改为在片元着色器**逐像素**重算（窄带 `smoothstep`，ADR 0009），不再是逐顶点插值；外观为**程序生成占位贴图**（**无真实美术 PBR 资源**，故仍为"部分实现"） |
| 球笔刷挖 / 堆 + 脏 tile 局部重网格 | **已实现** | `world/dig/terrain_brush.*` |
| 可挖标记区域（程序化规则 + 数据文件叠加） | **未开始** | 规则已定（ADR 0006）；代码未实现 |
| 可挖体积（局部 SDF + 等值面网格化，洞穴） | **未开始** | 方案见 ADR 0007 |
| 物件层（地表元素 / 建筑 / 建造） | **未开始** | 分层定义见 ADR 0004 |
| 流式加载 / 卸载（按距离） | **未开始** | 当前固定 3×3 tile |
| LOD 与接缝缝合 | **未开始** | 待收敛项 4 |

### 1.4 物理与角色

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| 物理引擎薄封装（Jolt，固定步长推进） | **已实现** | `engine/physics/`；公共头不含 Jolt 类型 |
| 地表高度场碰撞体（逐 tile，挖掘后按脏 tile 重建） | **已实现** | `world/terrain/terrain_collision.*` |
| 通用静态盒体（供世界边界等使用；公共头不含 Jolt 类型） | **已实现** | `engine/physics/physics_world.hpp`（`AddStaticBox`）；由上层按地图范围推导放置 |
| 角色胶囊控制器（走 / 冲刺 / 跳 / 上坡 / 自动上台阶） | **已实现** | `CharacterVirtual`；重力 24 / **跳跃初速 7.20（由身高推导，最高点 = 身高 60% = 1.08 格）** / 最大坡度 50° / 上台阶 1.0 格 |
| 角色位置纠正（被地形埋住时顶回地表并清零速度） | **已实现** | `engine/physics/physics_world.hpp`（`SetCharacterPosition`）；静态高度场不会把角色顶出，须由上层在改地形后纠正 |
| 飞行模式（角色可控竖直移动，碰撞仍生效） | **已实现** | 当前作为测试设施（`FreeFly` 开关） |
| 刚体 / 碰撞事件 / 可推物体 | **未开始** | — |
| 物理的世界坐标精度方案（大坐标） | **未开始** | **待收敛项 7**（`joltphysics` 为单精度） |

### 1.5 实体、任务与数据

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| ECS（EnTT） | **已引入未使用** | 依赖已在 `vcpkg.json`，代码尚未使用 |
| 任务系统 / 后台线程（enkits） | **已引入未使用** | 同上；当前生成与网格化仍在主线程 |
| 配置表加载（TOML + toml++，启动期校验、非法即报错） | **已实现** | `world/terrain/material_table.*`、`world/generation/map_preset.*`、`engine/platform/settings.*` |
| 音频（播放 / 混音 / 音源） | **未开始** | 仅保留**唯一增益入口** `engine/platform/settings.hpp::ApplyMasterVolumeGain`（设置值已接通，**当前无声源 ⇒ 听不到**）；要能听到还需音频流 + 混音器 + 音源 |
| 存档 / 读档（只存脏数据，自定义二进制 + zstd） | **未开始** | 内容模型见 ADR 0006 / `references/save-and-serialization.md` |
| 资源管理（纹理 / 模型加载） | **未开始** | `stb_image` 已引入未使用；模型导入（Assimp）未引入 |

### 1.6 调试与工程质量

| 能力 | 状态 | 说明 / 落点 |
| --- | --- | --- |
| UI 可交互（ImGui + SDL3/SDL3_gpu 后端，事件转发已接） | **已实现** | `game/debug_overlay.*`、`game/system_panel.*`；面板交互与游戏输入抑制分离（`game/gameplay_input.hpp`） |
| UI 字体解析与标签缝（命中 CJK 字体用中文，否则**整表英文、绝不缺字**） | **已实现** | `game/ui_font.*`（三级解析：仓库 `assets/fonts/` → 系统 CJK → 无）、`game/ui_text.hpp`（唯一取词缝；有单测 + 源码扫描防绕过） |
| UI 主题（统一暗色样式，单一样式入口） | **已实现** | `game/ui_theme.*`（`ApplyUiTheme`） |
| 单元测试 | **已实现** | `tests/`，**113 项**（`ctest --preset debug`） |
| 结构门禁（禁止标识符扫描） | **已实现** | `scripts/check-banned-identifiers.ps1`（扫 **81** 文件） |
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
