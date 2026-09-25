# ADR 0002：Shader 采用 SPIR-V + DXIL 双格式并存

- **状态**：已采纳
- **日期**：2026-09-25
- **相关**：`docs/tech-plan-v1.3.md` §3.2 图形渲染 / §9.1 环境清单；`cmake/Shaders.cmake`；`engine/render/triangle_renderer.cpp`
- **取代**：ADR 0001 中「Shader 在构建期离线编译为 **SPIR-V**」这一子决策；ADR 0001 的「渲染后端采用 SDL3_gpu」主体决策**仍然有效**

## 背景

ADR 0001 定下"构建期离线编译为 SPIR-V"，并据此推论"新开发机必须安装 Vulkan SDK 以获得 `glslc`"。
PoC #2 实测推翻了该假设：

- 运行即断言失败：`SDL_CreateGPUShader_REAL: '!"Incompatible shader format for GPU backend"'`。
- 指定 `SDL_GPU_DRIVER=vulkan` 得到 `SDL_HINT_GPU_DRIVER vulkan unsupported!`，
  说明该开发机（RTX 3080）**没有可用 Vulkan**，SDL3_gpu 落在 **D3D12** 后端。
- **D3D12 只接受 DXIL，Vulkan 只接受 SPIR-V** —— 着色器字节码格式由后端决定，不由我们的偏好决定。

因此"只产 SPIR-V"在任何没有 Vulkan 的 Windows 机器上**必然崩**，而这类机器数量不少。

## 决策

**构建期同时产出 SPIR-V 与 DXIL 两种字节码，运行时按设备能力选择加载哪一种。**

- 链路为**两段式**（`shadercross` 不接受 GLSL 输入，无法一步到位）：

  ```
  triangle.vert (GLSL) --glslc--> .spv (SPIR-V) --shadercross--> .dxil (DXIL)
  ```

- 运行时用 `SDL_GetGPUShaderFormats()` 询问设备需要哪种格式，再决定加载 `.spv` 还是 `.dxil`
  （见 `engine/render/triangle_renderer.cpp`）。
- 工具链由 **vcpkg** 提供：`shaderc` 端口给 `glslc`、`sdl3-shadercross` 端口给 `shadercross`。
  两者以 **host 依赖**形式写入 `vcpkg.json`（`{ "name": "...", "host": true }`），
  使工具自动进入 CMake 的 `find_program` 搜索路径，CI 与开发机无需手工配 PATH。
- **不再需要 Vulkan SDK**：ADR 0001 中"必须安装 Vulkan SDK"的推论作废。
- 缺 `glslc` 时 `cmake/Shaders.cmake` 只警告并跳过，不阻断配置 —— 代价是**运行期才失败**，
  故 CI 必须把两个 host 依赖装齐（否则会编过但跑不起来）。

## 备选方案与取舍

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| **双格式并存**（采纳） | 有 Vulkan 的机器走 Vulkan、没有的走 D3D12，**两边都能跑** | 构建期多一次转换；产物多一份 | **采纳** |
| 只产 SPIR-V（ADR 0001 原决策） | 工具链最短 | 无 Vulkan 的机器**直接断言失败**，PoC 已实测 | 已被推翻 |
| 只产 DXIL | Windows 上够用 | Vulkan / Metal 机器全废；与"跨平台"目标冲突 | 不可行 |
| 改用 HLSL 单一源 | 可只产 DXIL | 放弃 GLSL；且 Vulkan 仍需 SPIR-V | 不划算 |

## 后果

### 正面

- Windows 上有无 Vulkan 均可运行，不再依赖开发机的驱动栈是否完整。
- 工具链全部来自 vcpkg，与其它依赖同一套 baseline，环境可复现。
- 上层代码只依赖"选格式 + 加载"两步，与后端解耦。

### 负面 / 必须承受的代价

- 构建期多一次 `shadercross` 转换，产物体积翻倍。
- `shaderc` / `sdl3-shadercross` 作为 host 依赖会拉长首次 vcpkg 安装时间（实测 `shaderc` 编译约 4.6 分钟）。
- 每新增一种后端（如 macOS 的 Metal/MSL）都要在 `cmake/Shaders.cmake` 里补一段转换。

## 何时需要重新审视

1. SDL3_gpu 支持"运行时从单一中间表示转译"且成本可接受时（可回到单格式）。
2. 需要支持 Metal 后端时：须补 SPIR-V → MSL 一段（同一两段式链路，只是多一个 dest）。
3. `shadercross` 的上游输入格式发生变化（例如直接接受 GLSL）时，可简化两段式为一阶段。
