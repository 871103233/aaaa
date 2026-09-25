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
│   ├── shaders/               GLSL 源
│   └── textures/              纹理源 + 层号分配表
├── cmake/                     自写构建辅助模块
├── docs/                      方案文档 / ADR / 本索引 / 开发记录 / 阶段计划
│   ├── adr/                   架构决策记录
│   └── plans/                 阶段计划（接手者每次开工先读）
├── engine/                    引擎核心层
│   ├── core/                  主循环 / 固定步长 / 计时 / 日志
│   ├── input/                 输入动作状态层（上层只消费动作，不读 SDL 事件）
│   ├── platform/              平台抽象
│   └── render/                渲染封装
├── game/                      游戏逻辑层
├── tests/                     单元测试
├── voxel/                     体素世界层
│   └── chunk/                 区块数据与状态机
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
| `engine/platform/` | 平台抽象：窗口、输入、计时、文件 IO | 可依赖第三方（SDL3） | 不放渲染与游戏逻辑 |
| `engine/render/` | 渲染封装（RHI 薄层） | 可依赖 `engine/platform` | 不把具体图形 API 语义泄漏到上层 |
| `engine/CMakeLists.txt` | 聚合 `engine/` 源文件为 `voxel_engine` 静态库 | — | 新增源文件须在此登记 |

---

## 体素世界层（迁移中：见 ADR 0004）

> ⚠ 该层已被 **ADR 0004** 取代为"分层混合世界"：**地表高度场 + 可挖标记区域内的有界 SDF 体积 + 物件/建造层**。
> 下表为**现状**（旧方块体素实现），重写前不得据此扩展；新目录结构与职责待 `tech-plan-v2.0.md` 定稿后回填。

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `voxel/` | ~~体素世界~~ → **待改为世界层**：地表高度场、可挖体积、物件层、生成、网格化、流式加载、存档 | 可依赖 `engine` | 硬件访问一律经引擎核心 / 平台抽象，不直接调用平台 API |
| `voxel/chunk/` | ~~区块数据与状态机~~ → **待重写**为地表 tile / 可挖体积块的数据与状态 | 可依赖 `engine` | 现状只有 `chunk_types.hpp`；其尺寸常量与状态判据**已随 ADR 0004 作废**，迁移期不修不删 |
| `voxel/CMakeLists.txt` | 世界层构建目标 | — | 新增子目录须在此登记 |

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
| `assets/shaders/` | GLSL 源（`.vert` / `.frag` / `.comp`） | — | 只放源；`.spv` / `.dxil` 由构建生成到 `<build>/assets/shaders/` |
| `assets/textures/` | 纹理源（供地表多纹理权重混合使用） | — | 现状含 `layers.toml`（纹理数组层号表）——**已随 ADR 0004 作废待替换**为材质 / 生物群系配置 |
| `assets/blocks.toml` | ~~方块注册表数据源~~（**已随 ADR 0004 作废待删除**） | — | 方块世界专属；替换为材质表 / 可挖区域表 / 生成参数（格式待收敛） |

---

## 构建辅助 / CI / 文档 / 规范

| 条目 | 职责 | 依赖方向 | 约束 |
| --- | --- | --- | --- |
| `cmake/` | 自写构建辅助模块 | — | 不放业务逻辑；工具缺失时降级为**警告**，不阻断配置 |
| `cmake/Shaders.cmake` | 两段式 Shader 编译：GLSL →(glslc) SPIR-V →(shadercross) DXIL | — | 两种格式**都必须产出**：Vulkan 用 SPIR-V，D3D12 用 DXIL |
| `.github/workflows/` | CI：门禁 → 构建 → 测试 | — | 文件与 CI 脚本保持**纯 ASCII**（原因见 `ci.yml` 顶部注释）；新增步骤须本地可复现 |
| `docs/` | 方案文档、ADR、文件索引、开发记录、学习笔记、阶段计划 | — | 与代码同步 |
| `docs/adr/` | 架构决策记录（`NNNN-<主题>.md`）**+ `README.md` 决策索引** | — | 正文一经写入不回改；被取代时新增一条并互相链接；**每次决策变动须在同一次提交内更新索引** |
| `docs/plans/` | 阶段计划：当前阶段的任务分解、顺序、进度、下一步 | — | 只写计划与进度，不写技术结论（选型一律指向方案 / ADR）；阶段闭环后冻结不回改 |
| `.trae/skills/` | AI 开发规范技能（正文 + references + 门禁脚本） | — | 规范变更时同步更新 |

---

## 当前模块入口

| 入口 | 说明 |
| --- | --- |
| `engine/core/fixed_step.hpp` | 固定步长累加器（单帧补步封顶、渲染插值 alpha） |
| `engine/core/log.hpp` | 统一日志接口（`VX_LOG_*`） |
| `engine/input/input_map.hpp` | 输入动作状态层（上层只消费动作） |
| `engine/platform/window.hpp` | 窗口与事件循环 |
| `engine/render/triangle_renderer.hpp` | 渲染入口（当前为 PoC 冒烟测试用，将由网格渲染路径取代） |
| `engine/render/mesh_renderer.hpp` | 通用网格渲染路径（顶点/索引缓冲、相机 UBO、索引绘制） |
| `engine/render/camera.hpp` | 第三人称相机 + 避障；`ITerrainQuery` 查询契约（由世界层实现） |
| `voxel/chunk/chunk_types.hpp` | ~~区块状态机与 `can_build_mesh` 唯一判据~~ —— **已随 ADR 0004 作废**，迁移期不修不删 |

---

## 规划中（尚未创建）

| 规划路径 | 用途 | 备注 |
| --- | --- | --- |
| `tools/` | 离线工具：纹理打包、可挖区域标记生成、资源生成 | 目录结构待 `tech-plan-v2.0.md` 定稿；创建时须在此登记 |
| `editor/` | 场景编辑器 | POST-V0.5 暂缓项，默认不创建 |
