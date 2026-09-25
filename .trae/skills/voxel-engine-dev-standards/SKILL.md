---
name: voxel-engine-dev-standards
description: Enforces this voxel engine repo's architecture and coding standards - chunk states, task system, thread roles, precision, save versioning. Use when writing or editing engine and voxel code.
---

# 体素引擎开发规范

本文件是本仓库的**强制施工规则**。选型理由与备选方案对比见方案文档
`docs/tech-plan-v1.3.md`，本技能只保留**可检查的硬约束**。
两者冲突时以方案文档为准，并回来修正本技能。

## 阅读约定（先读这一节）

本文件**全部是规则**。凡标注「禁止 / 禁用 / 不使用 / 暂缓」的条目，都是**不得执行**的动作，**不是待办事项**；
每条禁止都对应一个必须执行的正向做法，**一律以正向做法为准**。
「备选方案」是默认不启用的兜底项，不是并行候选，仅在出现明确切换条件并经确认后采用。

## 技术白名单（默认采用，照此执行）

| 领域 | 一律使用 |
| --- | --- |
| 渲染后端 | **SDL3_gpu**（Vulkan / Direct3D 12 / Metal）+ SPIR-V（**SDL_shadercross** 离线编译） |
| 任务调度 | **enkiTS** 或 **Taskflow** |
| 存档 | **自定义二进制 + zstd** + `.voxr` 区域文件 |
| 程序化生成 | **FastNoiseLite** + **分块确定性网格抖动** |
| 网格 | 面剔除 + **贪婪合并**（作用域 = 单 Section） |
| 单元测试 | **GoogleTest + CTest** |
| 物理 | **自研 swept AABB**（子步进 + 上台阶）；Jolt 仅用于动态刚体 |
| 性能分析 | **Tracy**；图形调试 **RenderDoc** |

## 适用范围

**适用**：`engine/`、`voxel/`、`game/` 下的任何 C++ 改动；区块 / 生成 / 网格化 / 光照 / 流式加载 / 存档；
构建配置、目录结构、第三方库引入；测试与 CI；性能优化。

**不适用**：纯玩法数值调整；与引擎无关的独立脚本工具。

## 任务路由

先通读本文件（通用底线，始终生效），再按任务类型读取**对应那一份**引用：

| 当前任务涉及 | 必读引用 |
| --- | --- |
| 区块、程序化生成、流式加载、体素光照 | `references/chunk-and-streaming.md` |
| 网格化、贪婪合并、顶点格式、渲染、LOD、Draw Call | `references/meshing-and-render.md` |
| 存档、序列化、压缩、版本迁移 | `references/save-and-serialization.md` |
| 线程、任务系统、异步管线、GPU 上传 | `references/concurrency.md` |
| 构建、Sanitizer、单元测试、CI | `references/build-and-tests.md` |

只读与当前任务相关的那一份。

## 一、红线（违反即返工）

下表左列是**需识别并绕开的写法**，右列是**必须执行的做法**——执行时以右列为准。

| # | 需绕开的写法 | 必须执行 |
| --- | --- | --- |
| 1 | 把每个方块做成 ECS Entity | 区块是 Entity（甚至不进 ECS），方块是 Section 内数组元素；仅动态物体为 Entity |
| 2 | 主线程同步加载/生成/IO 区块 | 生成、光照、网格、IO 全走后台任务；仅 GPU 上传回渲染线程 |
| 3 | 每方块单独顶点缓冲 / 单独绑定纹理 | 区块级合并网格 + 纹理数组；透明面走独立通道 |
| 4 | 持久化所有区块 | 只存脏区块；未修改区块靠种子确定性重建 |
| 5 | 用通用物理引擎处理方块碰撞 | 自研 swept AABB + 子步进 + 上台阶；Jolt 仅用于动态刚体 |
| 6 | 用 `float` 表示世界坐标 | 位置用整数（BlockPos `int32` / WorldPos `double`）+ 相机相对渲染 |
| 7 | 生成逻辑依赖 `rand()` / 时间 / 线程顺序 | 纯确定性函数（种子 + 整数坐标） |
| 8 | 存档格式无版本号 | 带 `version` + 迁移函数 + 迁移测试 |
| 9 | 对区块加粗粒度锁读写 | 单写者构建 + 不可变快照 + 原子指针发布 |
| 10 | 热路径 `new/delete`、滥用 `shared_ptr` | 对象池 / 句柄 / 线性分配器；`shared_ptr` 仅限区块快照 |
| 11 | 用可变 `deltaTime` 直接驱动物理与角色逻辑 | 固定时间步长 + 渲染插值 |
| 12 | 区块未等邻居就建网格 | 邻居达 `Lit` 才可进入 `Meshed`（详情见 `references/chunk-and-streaming.md`） |
| 13 | 贪婪合并跨 Section，或缺 4 角光照/AO 判据 | 作用域 = 单 Section；判据含 4 角光照/AO（AO 先恒为 0 占位） |
| 14 | 用归一化 0~1 UV 直接采大四边形 | 存格内 UV + `quadSize`，Shader 端 `fract(uv * quadSize)` |
| 15 | 结构生成依赖全局状态，无法"只看当前区块"重建 | 分块确定性网格抖动（Chunked Jittered Grid） |
| 16 | 每帧全量重算光照 / 整 Chunk 重建 | BFS 增量传播；脏标记到 Section 粒度 |
| 17 | Debug 下裸跑 | Windows 开 ASAN；Linux/Clang 另跑 TSan（两者互斥，分开配置） |

## 二、分层与依赖方向

`platform → engine core → voxel world → game`

- 依赖只能向上：下层不 include 上层头文件（`engine/` 不引用 `voxel/`、`game/`）。
- `engine/` 内只放通用类型，不放体素 / 游戏专有类型（`Chunk`、`BlockId`、`Biome` 等）。
- `voxel/` 只经引擎核心层与平台抽象层访问硬件，不直接调用平台 API。
- 第三方库只在平台层或引擎核心的薄封装中直接引用。

## 三、编码约定

- 标准：**C++17**。只使用 C++17 及以下的语言与库设施，**不使用 C++20 特性（Modules、`std::jthread` 等）**。
- 命名：类型 / 枚举 `PascalCase`；函数与方法 `PascalCase`；局部变量与参数 `camelCase`；
  成员 `m_camelCase`；编译期常量 `kPascalCase`；宏 `UPPER_SNAKE`；文件 `snake_case.h` / `snake_case.cpp`。
- 头文件：一律 `#pragma once`；头文件内不使用 `using namespace`；优先前置声明。
- 所有权：`unique_ptr` / 句柄 / 资源池；不用裸 owning 指针与散落的 `new/delete`。
- 错误处理：热路径用返回值传递错误，不做运行时分配、不抛异常；启动期可用异常。
- 不使用 RTTI（`dynamic_cast` / `typeid`）；需要区分类型时用 tag 或 variant。
- 常量正确性：能 `const` 一律 `const`。
- 日志走统一日志宏 / 接口，不散落 `std::cout` / `printf`。
- 注释：公共接口用 Doxygen 说明意图与前置条件；实现内不复述代码。
- Shader：GLSL 源 + 自写 include 预处理，经 **SDL_shadercross** 离线编译为 SPIR-V（SDL3_gpu 必需）。

## 四、性能预算（超出即视为缺陷）

- 帧目标 60 FPS：V0.3 视距 8 区块；V0.5 视距 16~24 区块。
- Draw Call：**≤700/帧 @16 区块**；**≤1500/帧 @24 区块**。
  要压到"几百"须上 Multi-Draw Indirect + 4×4 区域合批（列为 V0.5 之后备选）。
- 内存（24 区块视距 = 2401 区块）：CPU ≈ 70~150 MB；**VRAM ≈ 150~550 MB**（网格常驻显存，纹理数组另计 32~64 MB）。中低端 GPU 会先于系统内存见底。
- 单区块光照数据未压缩为 **96 KB**（`16×16×384 × 1 B`），估算时按 96 KB 计。
- 单区块方块数据（Section 压缩 + palette）目标 8~20 KB。
- 主线程帧内只做渲染、输入、相机、碰撞查询；区块生成、网格化、文件 IO、等待 GPU 都放到后台或上传阶段。
- 碰撞查询范围：**3×3×3 个方块**（单位是方块，不是 Section）。
- 任何性能相关改动必须附 Tracy 对比数据，不接受"感觉更快了"。

## 五、范围控制

| POST-V0.5 暂缓项（默认不启动） | 当前阶段改用 |
| --- | --- |
| 场景编辑器 | Dear ImGui 调试面板 + 配置文件 |
| 脚本系统（Lua / sol2） | 数据驱动配置（TOML / JSON） |
| 网络联机（GameNetworkingSockets） | 单机 + 确定性生成 |
| 多渲染后端（手写 Vulkan） | 只用 SDL3_gpu |
| 自研 ECS | 直接用 EnTT |

上表为**暂缓**项，不是待办：默认不实现，仅在获得明确批准后启动。
原则：引擎不为"未来第二款游戏"提前抽象；每阶段只交付一个可运行、可观察的目标。

## 六、完成定义（DoD 自检）

- [ ] 编译零警告（MSVC `/W4`，警告视为错误）
- [ ] 结构性禁止门禁通过（`scripts/check-banned-identifiers.ps1` 退出码 0）
- [ ] 逐条对照第一节红线表的「必须执行」列，确认全部满足
- [ ] 分层依赖方向正确，无逆向 include
- [ ] 已按"任务路由"落实对应引用文件的要求
- [ ] 所有权、错误处理、RTTI 使用均符合第三节
- [ ] 命名与头文件约定符合第三节
- [ ] 性能敏感改动已附 Tracy 对比数据，且满足第四节各项预算
- [ ] 涉及生成 / 存档 / 网格化的改动，已新增或更新对应测试
- [ ] 方案文档与实际代码同步

## 七、发现冲突时

若现有代码与本规范不符：**先报告再改**。
列出冲突点、影响范围与建议方案，经确认后统一整改，并在提交信息中说明。
