# 第三方组件许可清单（Third-Party Notices）

本文件列出本项目使用或计划使用的第三方组件及其许可。
发布任何二进制产物前，**必须逐项核对上游仓库的 LICENSE 文件**，并把完整许可原文一并分发。

> ⚠️ 下表许可类型为记录用途。**标 \* 者尚未逐字核对**，发布前必须确认。

| 组件 | 用途 | 许可 | 引入阶段 |
| --- | --- | --- | --- |
| SDL3 | 窗口 / 输入 / 音频 / GPU 抽象 | zlib | V0.1 |
| GLM | 数学库 | MIT（另有 Happy Bunny License 双许可） | V0.1 |
| FastNoiseLite | 噪声生成 | MIT | V0.1 |
| stb_image（`stb`） | 纹理加载 | Public Domain / MIT 双许可 | V0.1（**T57 起已接入**：`engine/render/texture_loader.*`） |
| Dear ImGui（`imgui`，含 SDL3 与 SDL3_gpu 绑定） | 调试 UI | MIT | V0.1 |
| toml++（`tomlplusplus`） | 配置解析 | MIT | V0.1 |
| GoogleTest | 单元测试 | BSD-3-Clause | V0.1 |
| enkits（enkiTS）\* | 任务调度 | zlib | V0.3 |
| Taskflow \* | 任务调度（备选） | MIT | V0.3（备选） |
| EnTT | ECS | MIT | V0.4 |
| Jolt Physics（`joltphysics`） | 刚体物理 / 角色碰撞与地形碰撞 | MIT | V0.1 |
| zstd | 存档压缩 | BSD-3-Clause（另有 GPLv2 双许可） | V0.3 |
| Assimp | 3D 模型加载 | BSD-3-Clause | **V0.3（ⓒ T68）** —— 所有者 2026-09-29 指示由 V0.5 **前移**；**尚未引入** |
| Tracy | 性能分析 | BSD-3-Clause | V0.2 |
| RenderDoc | 图形调试（外部工具，不随产物分发） | MIT | V0.1 |
| Lua 5.4 \* | 脚本 | MIT | V1.0+ |
| sol2 \* | Lua 绑定 | MIT | V1.0+ |
| GameNetworkingSockets | 网络联机 | BSD-3-Clause | V1.0+ |
| shaderc（`glslc`）\* | Shader 编译：GLSL → SPIR-V（构建期工具） | Apache-2.0 | V0.1 |
| SDL_shadercross \* | Shader 交叉编译：SPIR-V → DXIL（构建期工具） | 以官方仓库为准 | V0.1 |

## 美术资源台账（CC0；**资源文件不入库**）

> **入库口径**（所有者 2026-09-29 裁定，见 `docs/plans/v0.3.md` §1.1 第 2 条）：**仓库只放下载脚本 + 校验和 + 本台账**，
> 资源文件由 `tools/fetch_assets.ps1` 取回到 `assets/textures/`（该目录被 `.gitignore` 排除）。
> 校验和清单 = **`tools/assets.sha256`**（进仓库）；重复运行脚本会**逐项校验**，上游文件变化即**报错**（不静默接受）。
>
> **许可口径**：ambientCG 全部内容与 Poly Haven 全部内容均为 **CC0 1.0 Universal**（可商用、可修改、**无需署名**）；
> 本项目仍**逐项登记**来源与作者，便于追溯与发布核对。
> **Quixel Megascans 不可用**（UE-Only Content，与自研引擎不兼容）—— 这也是本项目只从 CC0 源取材的原因。

### 表 1：来源与许可

| 来源 | 资产 | 作者 | 许可 | 来源页 | 采集日期 |
| --- | --- | --- | --- | --- | --- |
| ambientCG | Grass 001（地表-草） | ambientCG（Lennart Demes 及贡献者） | CC0 1.0 Universal | <https://ambientcg.com/view?id=Grass001> | 2026-09-29 |
| ambientCG | Ground 037（地表-土） | 同上 | CC0 1.0 Universal | <https://ambientcg.com/view?id=Ground037> | 2026-09-29 |
| ambientCG | Rock 030（地表-岩） | 同上 | CC0 1.0 Universal | <https://ambientcg.com/view?id=Rock030> | 2026-09-29 |
| ambientCG | Ground 093 C（地表-沙 / 沙漠沙丘） | 同上 | CC0 1.0 Universal | <https://ambientcg.com/view?id=Ground093C> | 2026-09-29 |
| Poly Haven | Kloofendal 48d Partly Cloudy（环境 HDRI，户外晴天） | Greg Zaal | CC0 1.0 Universal | <https://polyhaven.com/a/kloofendal_48d_partly_cloudy> | 2026-09-29 |

### 表 2：文件与校验和（`SHA-256`，逐文件）

| 文件（相对仓库根） | 来源资产 | SHA-256 |
| --- | --- | --- |
| `assets/textures/terrain/grass/albedo.jpg` | Grass 001 | `8a8bfcffd087134caad2a7def216f3dd2d307fbe02340ca47e3c5c71f49d50b4` |
| `assets/textures/terrain/grass/normal.jpg` | Grass 001 | `7ccaf9e28ea7a533e590c2e07a39e141017a5a5db8b7241730a3c287c540e881` |
| `assets/textures/terrain/grass/roughness.jpg` | Grass 001 | `2a2a3f9220a351f14858075d4c6ba1ff7fde8bedd7009c94cb432bc2b9a362e4` |
| `assets/textures/terrain/grass/ao.jpg` | Grass 001 | `7aa2c5ac4db77005b92bed77399189c9219300ada2f3d86c3c0fea51bba5a3f1` |
| `assets/textures/terrain/dirt/albedo.jpg` | Ground 037 | `86af35a54593548543a291a54bddfd3568fd90268f8f65d0f8e743b37d714782` |
| `assets/textures/terrain/dirt/normal.jpg` | Ground 037 | `99743f03f5f818d4a0ee3dca289d5c6f125b1e05f1e624a541455e2f7fb29eec` |
| `assets/textures/terrain/dirt/roughness.jpg` | Ground 037 | `5484c117706e5937afa103c82795cbacb6c3b76d1a056b47942ca91d00efbbb9` |
| `assets/textures/terrain/dirt/ao.jpg` | Ground 037 | `f8529644aff137ccc4566b7304b3e55b97bc4a8a1ada3aa92d4f5f42dac6e687` |
| `assets/textures/terrain/rock/albedo.jpg` | Rock 030 | `c7169827afa47148833a3d778bd2827f12b124852efcabf3364910b1cb6d4beb` |
| `assets/textures/terrain/rock/normal.jpg` | Rock 030 | `bf4b029b98fc771887e80807e28f97488c3a8936369152bd1020b2535415a5af` |
| `assets/textures/terrain/rock/roughness.jpg` | Rock 030 | `a057169aa693438170a969249eb7e1113078f04c38b3fc2766fdccf21d5a918f` |
| `assets/textures/terrain/rock/ao.jpg` | Rock 030 | `f430913d66ff94e55c384d006e0365808b304dee513db4c27eeab49385c1c265` |
| `assets/textures/terrain/sand/albedo.jpg` | Ground 093 C | `7a814c8d4fe2fc894dcc5426f1b8919f960ce844a3dd5cd98d6ba1a12fb6193f` |
| `assets/textures/terrain/sand/normal.jpg` | Ground 093 C | `254828003997c04de657fe9109bb5dae7eeeff61abb6315c4c8a4dfdf79f65fc` |
| `assets/textures/terrain/sand/roughness.jpg` | Ground 093 C | `ab402c7d1c773b26a860d8b0e976407a0663ea3b509d45ba7a26ea9baaeca707` |
| `assets/textures/terrain/sand/ao.jpg` | Ground 093 C | `b327c1c436e1cdf40e85c71ea0dacac4beb647c9e54a1962a95887ba044298a6` |
| `assets/textures/env/kloofendal_48d_partly_cloudy_2k.hdr` | Kloofendal 48d Partly Cloudy | `3fbd33f279f29bd64925c1dd3214fd46c627493d21f1100248b6b1098da4d06e` |

**取回命令**（资源不入库 ⇒ 干净克隆后需执行一次）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1 -Record   # 首次：下载并记录校验和
powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1            # 之后：逐项校验
```

**规格**（T66 消费者需要）：地表贴图 = **2048×2048 JPG**（albedo / normal(GL) / roughness / AO 四件套，共 4 套 ≈ 79 MB）；
环境贴图 = **`.hdr`（2K 等距柱状）**；文件名统一为 `albedo` / `normal` / `roughness` / `ao`（扩展名保留上游 `.jpg`）。

## 待办

- [ ] 填入 `LICENSE` 中的 `<COPYRIGHT HOLDER>`（当前为占位符）
- [ ] 逐项核对上表标 \* 的许可与版本
- [ ] 首次发布前生成完整的许可原文归档（如 `licenses/` 目录）
- [x] 首套美术资源（地表 PBR 4 套 + 1 张 HDRI，全部 CC0）来源 / 许可 / 作者 / 采集日期 / SHA-256 已登记（2026-09-29）
