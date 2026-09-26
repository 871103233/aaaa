# 文件目录（File Index）

本文件是仓库结构的**职责索引**，回答"什么该放哪里"。
维护规则见 [开发规范 · 第六节](../.trae/skills/voxel-engine-dev-standards/SKILL.md)。

- **粒度**：目录 + 模块入口（公共头 / `CMakeLists.txt` / 脚本）。实现文件（`.cpp`）与测试用例不逐个登记。
- **更新时机**：任何目录或模块入口发生增删改时，与代码**同一次提交内**更新本文件。
- **最后核对**：2026-09-25（对照 `git ls-files`）

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
├── game/                      游戏逻辑层（含 ImGui 调试面板）
├── tests/                     单元测试
├── third_party/               vendored 单头文件库（FastNoiseLite）
├── world/                     世界层（分层混合，见 ADR 0004）
│   ├── dig/                   笔刷挖掘 / 堆建与脏 tile 收集
│   ├── generation/            确定性种子与噪声（FastNoiseLite 封装）
│   └── terrain/               地表高度场 tile / 网格化 / 材质混合 / ITerrainQuery 实现
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
| `.gitignore` | 忽略 `build/`、`vcpkg_installed/`、`*.spv`、IDE 产物 | — | 保留 `.trae/skills/`（CI 依赖其中的门禁脚本） |
| `LICENSE` / `NOTICE.md` | 许可与第三方组件清单 | — | 引入新第三方库须同步 `NOTICE.md` |

---

## 引擎核心层

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `engine/` | 引擎核心：可复用的通用能力 | 可依赖 `platform` 与第三方 | 不放世界 / 游戏专有类型（`TerrainTile`、`DigVolume`、`Biome` 等），不含游戏内容 |
| `engine/core/` | 主循环装配、固定步长累加器、单调计时、统一日志接口 | 可依赖 `engine/platform` 与第三方 | 不放渲染与世界逻辑 |
| `engine/input/` | 输入动作状态层：按键 / 鼠标 → 动作，每帧采样一次 | 可依赖 `engine/platform` | 上层只消费动作；**本层之外不得读 SDL 事件队列** |
| `engine/physics/` | Jolt 薄封装：生命周期、固定步长推进、通用高度场 / 角色胶囊（**公共头不含 Jolt 类型**） | 可依赖 `engine/core` 与第三方 | 不放地形专有类型；世界坐标进出须显式转换并注明精度（见待收敛项 7） |
| `engine/platform/` | 平台抽象：窗口、输入、计时、文件 IO | 可依赖第三方（SDL3） | 不放渲染与游戏逻辑 |
| `engine/render/` | 渲染封装（RHI 薄层） | 可依赖 `engine/platform` | 不把具体图形 API 语义泄漏到上层 |
| `engine/CMakeLists.txt` | 聚合 `engine/` 源文件为 `voxel_engine` 静态库 | — | 新增源文件须在此登记 |

---

## 世界层

> 分层混合世界（ADR 0004 / `tech-plan-v2.0.md` §2）：地表高度场 + 可挖标记区域内的有界 SDF 体积 + 物件/建造层。
> **旧 `voxel/` 层与 `chunk/chunk_types.hpp` 已随迁移任务 M2 删除**（方块世界表示作废），不得再引用 `voxel/` 路径。

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `world/` | 世界层：生成、地表网格化、材质、挖掘、流式加载、可挖体积、存档 | 可依赖 `engine` | 硬件访问一律经引擎核心 / 平台抽象，**不直接调用平台 API** |
| `world/terrain/` | 地表高度场 tile（64×64、`int16` 1/16 格）、网格化与梯度法线、材质混合、`ITerrainQuery` 实现、**碰撞体采样构建**（`terrain_collision`） | 可依赖 `engine` | tile 网格须多采样一行/列（65×65），保证相邻 tile 边界**逐位相等、无裂缝**；世界定位用整数 / `double` |
| `world/generation/` | 确定性种子派生与噪声（FastNoiseLite 封装，pimpl 隔离）、**预设固定地图加载**（`map_preset`：种子 / 范围 / 出生点 / 地形编辑区，TOML） | 可依赖 `engine` | 生成必须是**纯函数**（种子 + 整数坐标）；预设编辑叠加在噪声之上，**同一文件必须得到同一世界**；禁止 `rand()` / 时间 / 线程顺序 |
| `world/dig/` | 笔刷挖掘 / 堆建与脏 tile 收集 | 可依赖 `engine` | 只标脏**受影响**的 tile；重网格与 GPU 上传不得阻塞主线程 |
| `world/CMakeLists.txt` | 世界层构建目标 | — | 新增源文件 / 子目录须在此登记 |

---

## 游戏逻辑层

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `game/` | 玩法、数值、关卡、UI、AI | 可依赖 `voxel` / `engine` | 不放通用能力；不被下层引用 |
| `game/main.cpp` | 程序入口：初始化、主循环、组装各层 | — | — |
| `game/CMakeLists.txt` | 游戏可执行目标 + Shader 构建钩子 | — | 新增 Shader 须在此 `add_shader(...)` |

---

## 测试

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `tests/` | 单元测试（GoogleTest + CTest） | 可依赖所有被测模块 | 只放测试，不放产品代码 |
| `tests/CMakeLists.txt` | 测试目标与 `add_test` 注册 | — | 新增测试文件须在此登记 |

---

## 资源

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `assets/` | 运行时资源源文件 | — | 生成物放 `assets/generated/`（已忽略） |
| `assets/config/` | 配置表：`materials.toml`（地表材质槽与权重规则）、`lighting.toml`（太阳 / 天空光 / 雾）等 | — | 带 `schema_version`；由 toml++ 在**启动期**加载，失败即明确报错（ADR 0005） |
| `assets/maps/` | 预设固定地图（TOML）：种子 / 覆盖范围 / 出生点 / 地形编辑区（flatten · raise · carve） | — | 带 `schema_version`；**非法文件必须显式报错，不得静默回退**；同一文件必须得到同一世界 |
| `assets/shaders/` | GLSL 源（`.vert` / `.frag` / `.comp`） | — | 只放源；`.spv` / `.dxil` 由构建生成到 `<build>/assets/shaders/`；新增须在 `game/CMakeLists.txt` 里 `add_shader` |
| `assets/textures/` | 纹理源（供地表多纹理权重混合使用） | — | 现状含 `layers.toml`（旧纹理数组层号表）——**已随 ADR 0004 作废待删除**；材质配置见 `assets/config/` |
| `assets/blocks.toml` | ~~方块注册表数据源~~（**已随 ADR 0004 作废待删除**） | — | 方块世界专属；已由 `assets/config/` 下的材质 / 可挖区域 / 生成参数取代 |

---

## 构建辅助 / CI / 文档 / 规范

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `cmake/` | 自写构建辅助模块 | — | 不放业务逻辑；工具缺失时降级为**警告**，不阻断配置 |
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
| `engine/input/input_map.hpp` | 输入动作状态层（上层只消费动作；鼠标按键与键盘对称） |
| `engine/platform/window.hpp` | 窗口与事件循环；**唯一**把 SDL 事件翻译进 `InputMap` 的地方；相对鼠标模式（捕获 / 释放）在此封装 |
| `engine/render/triangle_renderer.hpp` | PoC 冒烟测试路径（保留可编译，未接线） |
| `engine/render/mesh_renderer.hpp` | 通用网格渲染路径（顶点/索引缓冲、相机 UBO、索引绘制、纹理数组、HDR 目标 + 色调映射通道、渲染开销记账） |
| `engine/render/lighting_table.hpp` | `assets/config/lighting.toml` 的加载与校验；**光照 → GPU 的唯一投影入口**（`LightingUniform` / `BuildLightingUniform`） |
| `engine/render/shadow_cascade.hpp` | CSM **纯函数**：级联分割、texel 对齐的光空间矩阵、`ShadowUniform`（无世界 / 游戏专有类型） |
| `engine/render/camera.hpp` | 第三人称相机 + 避障；`ITerrainQuery` 查询契约（由 `world/` 实现） |
| `world/terrain/terrain_world.hpp` | 地表世界入口：tile 容器、网格、脏重网格，并实现 `ITerrainQuery` |
| `world/terrain/material_table.hpp` | `assets/config/materials.toml` 的加载与校验；**CPU→GPU 材质参数唯一投影入口**（`MaterialUniform` / `BuildMaterialUniform`） |
| `world/terrain/material_textures.hpp` | 程序生成占位材质贴图（albedo + 法线，确定性、可平铺；ADR 0009） |
| `world/terrain/world_bounds.hpp` | 世界边界盒与四周**空气墙**放置（**纯函数**，由 tile 范围推导；对任意地图尺寸生效） |
| `game/out_of_bounds.hpp` | 出界判定（**纯函数**）+ 救援余量；越界/坠落时送回出生点 |
| `game/character_movement.hpp` | 主角移动基向量（**纯函数**：由相机 yaw 得前向 / 右向；方向语义有单测钉死） |
| `game/character_mesh.hpp` | 主角**程序化胶囊代理网格**（可见占位体，尺寸同碰撞胶囊） |
| `game/mouse_capture.hpp` | 鼠标捕获状态机（**纯函数**：`Esc` 释放 / 点击重捕获；**重捕获点击先于笔刷判定被消费**，避免误挖） |
| `game/gameplay_input.hpp` | 游戏输入抑制决策（**纯函数**：面板打开 / ImGui 要鼠标键盘 → 抑制视角 / 笔刷 / 键盘玩法） |
| `game/system_panel.hpp` | ESC 系统面板（显示模式 / 分辨率 / 音量 / 退出）；**不碰 SDL、不写文件**，只回报"用户做了什么" |
| `game/ui_font.hpp` | UI 字体**三级解析**（仓库 `assets/fonts/` → 系统 CJK → 无）与加载 |
| `game/ui_text.hpp` | **标签缝**：所有 UI 标签的唯一取词处（命中 CJK 用中文，否则整表英文；**禁止绕过**，有单测与源码扫描防护） |
| `game/ui_theme.hpp` | UI 统一主题（**唯一**样式入口 `ApplyUiTheme`） |
| `engine/platform/settings.hpp` | 系统设置读写（TOML，落盘到 `SDL_GetPrefPath`）+ **唯一音频增益入口** `ApplyMasterVolumeGain` |

---

## 规划中（尚未创建）

| 规划路径 | 用途 | 备注 |
| --- | --- | --- |
| `tools/` | 离线工具：纹理打包、可挖区域标记生成、资源生成 | 目录结构待 `tech-plan-v2.0.md` 定稿；创建时须在此登记 |
| `editor/` | 场景编辑器 | POST-V0.5 暂缓项，默认不创建 |
