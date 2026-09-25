# ADR 0001：渲染后端采用 SDL3_gpu

- **状态**：已采纳
- **日期**：2026-09-25
- **相关**：`docs/tech-plan-v1.3.md` §3.2 图形渲染；技能规范 `references/meshing-and-render.md`
- **取代**：v1.1 方案中「OpenGL 3.3 Core 为首选渲染 API」的决定

## 背景

渲染后端是本项目改动代价最高的决策之一：它决定了 Shader 工具链、资源上传路径、
调试工具链，以及 Draw Call 与多线程录制命令的可行上限。若在 V0.3 之后才发现选错，
代价是重写整个渲染层。

约束条件：

1. 项目以「低成本跑通 + 优先采用现有高性能库」为原则（见方案 v1.2 修订原则）。
2. 目标是超大世界体素场景，需要纹理数组、间接绘制、多线程录制命令缓冲等能力。
3. 跨平台（Windows 主目标，macOS 可选）。

## 决策

**渲染后端统一采用 SDL3_gpu**，Shader 在**构建期**离线编译为 SPIR-V。

- Windows / Linux / Android 走 **Vulkan** 后端；Windows 亦可回退 D3D12；macOS / iOS 走 **Metal**。
- Shader 源为 GLSL，经 `glslc`（或 SDL 官方 `SDL_shadercross`）交叉编译；
  未安装工具链时构建期给出警告并跳过（见 `cmake/Shaders.cmake`）。
- 引擎侧只包一层薄接口（`Renderer` / `MeshBuffer` / `Material`），
  不对外暴露 SDL3_gpu 类型，便于日后替换实现。

## 备选方案与取舍

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| **SDL3_gpu**（采纳） | 复用现有库；免手写交换链/渲染通道/同步的数百行样板；天然跨 Vulkan/D3D12/Metal；内置间接绘制 | 需离线 Shader 工具链；抽象粒度不足时（bindless、mesh shader）会碰壁；**不包含 OpenGL 后端** | **采纳** |
| 手写 OpenGL 3.3 Core | 运行时编译 GLSL、无需离线工具链；教程最丰富；能亲手管理 VBO/VAO/FBO | 属维护型 API；多线程录制不友好；代码量远高；macOS 上限 4.1 | 备选（仅当"亲手理解渲染管线"成为第一诉求时） |
| 提前手写 Vulkan | 性能上限最高、参考最多 | 仅初始化（交换链 + 渲染通道）即数百行，对第一版引擎是工期杀手 | 推迟到 V1.0 之后，且仅在 SDL3_gpu 粒度不足时 |

## 后果

### 正面

- V0.1 即可跑通现代后端，不需要自研 RHI，也不需要手写数百行初始化代码。
- 后续若要下沉到自研 Vulkan 后端，只需替换薄接口下的实现，上层（`voxel/`、`game/`）不动。

### 负面 / 必须承受的代价

- **构建期依赖 Shader 工具链**：新开发机必须安装 Vulkan SDK（`glslc`）或 `SDL_shadercross`，
  否则运行期会因缺少 `.spv` 而失败。此项已写入方案 §9.1 环境初始化清单第 5 条。
- **调试工具受限**：RenderDoc 不支持 Metal 后端，macOS 需改用 Xcode Metal Debugger。
- **放弃"手写渲染管线"的学习收益**：这是本次决策最主要的取舍。

## 何时需要重新审视

满足以下任一条时，应新开 ADR 重新评估：

1. 遇到 SDL3_gpu 抽象粒度不足的真实瓶颈（bindless texture、mesh shader、GPU-driven 管线）。
2. 需要 macOS 上使用 RenderDoc 等不兼容 Metal 的工具链。
3. SDL3_gpu 的接口在 SDL 大版本中发生不兼容变更且无法适配。
