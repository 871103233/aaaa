# 第三方组件许可清单（Third-Party Notices）

本文件列出本项目使用或计划使用的第三方组件及其许可。
发布任何二进制产物前，**必须逐项核对上游仓库的 LICENSE 文件**，并把完整许可原文一并分发。

> ⚠️ 下表许可类型为记录用途。**标 \* 者尚未逐字核对**，发布前必须确认。

| 组件 | 用途 | 许可 | 引入阶段 |
| --- | --- | --- | --- |
| SDL3 | 窗口 / 输入 / 音频 / GPU 抽象 | zlib | V0.1 |
| GLM | 数学库 | MIT（另有 Happy Bunny License 双许可） | V0.1 |
| FastNoiseLite | 噪声生成 | MIT | V0.2 |
| stb_image | 纹理加载 | Public Domain / MIT 双许可 | V0.1 |
| Dear ImGui | 调试 UI | MIT | V0.1 |
| GoogleTest | 单元测试 | BSD-3-Clause | V0.1 |
| enkits（enkiTS）\* | 任务调度 | zlib | V0.3 |
| Taskflow \* | 任务调度（备选） | MIT | V0.3（备选） |
| EnTT | ECS | MIT | V0.4 |
| Jolt Physics | 动态刚体物理 | MIT | V0.4 |
| zstd | 存档压缩 | BSD-3-Clause（另有 GPLv2 双许可） | V0.3 |
| Assimp | 3D 模型加载 | BSD-3-Clause | V0.5 |
| Tracy | 性能分析 | BSD-3-Clause | V0.2 |
| RenderDoc | 图形调试（外部工具，不随产物分发） | MIT | V0.1 |
| Lua 5.4 \* | 脚本 | MIT | V1.0+ |
| sol2 \* | Lua 绑定 | MIT | V1.0+ |
| GameNetworkingSockets | 网络联机 | BSD-3-Clause | V1.0+ |
| shaderc（`glslc`）\* | Shader 编译：GLSL → SPIR-V（构建期工具） | Apache-2.0 | V0.1 |
| SDL_shadercross \* | Shader 交叉编译：SPIR-V → DXIL（构建期工具） | 以官方仓库为准 | V0.1 |

## 待办

- [ ] 填入 `LICENSE` 中的 `<COPYRIGHT HOLDER>`（当前为占位符）
- [ ] 逐项核对上表标 \* 的许可与版本
- [ ] 首次发布前生成完整的许可原文归档（如 `licenses/` 目录）
