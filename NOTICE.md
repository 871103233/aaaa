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
| enkits（enkiTS）\* | 任务调度 | zlib | **V0.3（T81 起已真正启用）**：`vcpkg.json` 依赖 `enkits`（实测 **1.12**）；唯一引用点 = `engine/core/task_scheduler.*`（头目录 `include/enkiTS/`，只在 `.cpp` 内出现），首个消费者 = `world/streaming/volume_build_pipeline.*`（可挖体积块构建下沉 worker，见 [ADR 0022](docs/adr/0022-volume-build-worker-pipeline.md)） |
| Taskflow \* | 任务调度（备选） | MIT | V0.3（备选） |
| EnTT | ECS | MIT | **V0.4（V0.5 的 V0 起已真正启用）**：`vcpkg.json` 依赖 `entt`（实测 **3.16.0**）；引用点 = `world/object/object_layer.cpp`（entt 头只在 `.cpp` 内出现，公共头不含；PIMPL 隔离），首个消费者 = **物件层**（[ADR 0004](docs/adr/0004-hybrid-layered-world-representation.md) 层③ / [ADR 0003](docs/adr/0003-ecs-and-third-party-libs.md)） |
| Jolt Physics（`joltphysics`） | 刚体物理 / 角色碰撞与地形碰撞 | MIT | V0.1 |
| zstd | 预制地图块压缩（并计划用于存档压缩） | BSD-3-Clause（另有 GPLv2 双许可） | **阶段 W（W2）已引入**：`vcpkg.json` 依赖 `zstd`（实测 **1.5.7**）；唯一引用点 = `world/premade/premade_map.cpp`（`zstd.h` 只在该 `.cpp` 内出现，公共头不含）；消费者 = 预制地图容器（[ADR 0026](docs/adr/0026-premade-map-format-and-bake-tool.md)） |
| Assimp | 3D 模型加载 | BSD-3-Clause | **V0.3（ⓒ T68）** —— 所有者 2026-09-29 指示由 V0.5 **前移**；**T68 已引入**：`vcpkg.json` 依赖 `assimp`；唯一引用点 = `engine/render/model_loader.cpp`（Assimp 头只在该 `.cpp` 内出现，公共头不含） |
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
> **角色模型**（T69 起）：Quaternius 的模型亦为 **CC0 1.0 Universal**（由 Cinevva 分发，其逐文件元数据标注 `license: CC0`）。
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
| Quaternius（经 Cinevva 分发） | Casual Female（**T69 占位主角**；含 `Idle`/`Walk`/`Run`/`Jump`） | Quaternius | CC0 1.0 Universal | <https://quaternius.com/>（分发页 <https://app.cinevva.com/game-assets/free-3d-character-models>） | 2026-10-05 |
| Kenney | Nature Kit（**自然物整包**；本项目**选抽 12 个低模**：3 树 / 灌木 / 草 / 花 / 蘑菇 / 高草 / 2 岩石 / 营火 / 木栅） | Kenney | CC0 1.0 Universal | <https://kenney.nl/assets/nature-kit> | 2026-10-06 |

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
| `assets/models/character/Casual_Female.glb` | Casual Female（Quaternius；经 Cinevva 分发） | `3b4f39d27dc8a5f3b42d023f88a928679e7b1b0ea49ddb4857a07399bc4d8332` |
| `assets/models/nature/tree_default.glb` | Nature Kit（Kenney） | `562d29638c902de3c7bee465d3a53bb77117efbc392ae04ed894faf6b5dc691d` |
| `assets/models/nature/tree_pineTallB.glb` | Nature Kit（Kenney） | `49b54146351e1e009e97ee592f997a90a16b10c5467baf734d352df1cddec5d1` |
| `assets/models/nature/tree_oak.glb` | Nature Kit（Kenney） | `d7fd8773674928c50c11b66d12c636d49bdcc15a8b1c7fbb98e6f63a3439a3f3` |
| `assets/models/nature/plant_bush.glb` | Nature Kit（Kenney） | `ae7b1beb39e242b13f5f29e3ec23ef21034b814f82297b8aa00a9bf4e1b09590` |
| `assets/models/nature/grass.glb` | Nature Kit（Kenney） | `260e41d3e5f2472492ed7b475c5b92a30b13ce2bad408535b5ff50574d4575e7` |
| `assets/models/nature/flower_redA.glb` | Nature Kit（Kenney） | `171930b7789ddbf735af3745576b5393750beb9133b9ff4a444bb6bb2986c020` |
| `assets/models/nature/mushroom_red.glb` | Nature Kit（Kenney） | `843e2de43dce78920a5675a67bac4b72f500e6220315e713fe08381642cfc571` |
| `assets/models/nature/plant_flatTall.glb` | Nature Kit（Kenney） | `b192523736f32754788de4dc7a5dcf30a58322b260ec8969120c91432a10b633` |
| `assets/models/nature/rock_largeA.glb` | Nature Kit（Kenney） | `6dd15390fd96501dcd1454765a17ba61dbbd8d47705dfe5149c8dd92b353ce25` |
| `assets/models/nature/rock_smallB.glb` | Nature Kit（Kenney） | `11b26bc0a12e971a3b69df1401e530e24432d528c262df94b99c412e7f372763` |
| `assets/models/nature/campfire_logs.glb` | Nature Kit（Kenney） | `98d87cdcf9095b94fd348d0946e911dd4ca6c0956bac3318cfd2b0030b86055a` |
| `assets/models/nature/fence_simple.glb` | Nature Kit（Kenney） | `ecaf6c29532aa9fd305a8ef71df769d60748bd797f2eb1c63bdefc4edb8062a7` |

**取回命令**（资源不入库 ⇒ 干净克隆后需执行一次）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1 -Record   # 首次：下载并记录校验和
powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1            # 之后：逐项校验
```

**规格**（T66 消费者需要）：地表贴图 = **2048×2048 JPG**（albedo / normal(GL) / roughness / AO 四件套，共 4 套 ≈ 79 MB）；
环境贴图 = **`.hdr`（2K 等距柱状）**；文件名统一为 `albedo` / `normal` / `roughness` / `ao`（扩展名保留上游 `.jpg`）。

**规格**（T69 消费者需要）：角色模型 = **单文件 GLB** —— `Casual_Female.glb`（**23 关节 / 6,624 三角面 / 17 条动画**，含 `Idle` / `Walk` / `Run` / `Jump`；**无外部贴图依赖**）。

**规格**（V8 消费者需要）：**自然物**模型 = **单文件 GLB**（Kenney《Nature Kit》**选抽 12 个**：`tree_default` / `tree_pineTallB` / `tree_oak` / `plant_bush` / `grass` / `flower_redA` / `mushroom_red` / `plant_flatTall` / `rock_largeA` / `rock_smallB` / `campfire_logs` / `fence_simple`），落到 `assets/models/nature/`。
**注意**：这些 GLB 自带贴图 / UV，但**本项目当前不渲染模型自带贴图**（渲染器只有地表 4 槽材质）⇒ 按**地表材质槽**着色（见 [`docs/plans/v0.5.md`](docs/plans/v0.5.md) §1.9 的已确认降级）；**逐模型贴图**登记为后续能力。

## 预制地图烘焙产物台账（**按尺寸分流：小世界入库 / 大世界不入库**）

> **入库口径**（[ADR 0026](docs/adr/0026-premade-map-format-and-bake-tool.md) §四，**2026-10-06 修订：由"一律不入库"改为按尺寸分流** + [`docs/plans/v0.5.md`](docs/plans/v0.5.md) §1.12）：
> `.vxmap` 是**离线烘焙产物**，由脚本**确定性**生成（同一输入 ⇒ 逐字节相同）。
> - **小世界预制（≈1 km，如 B）⇒ 产物入库**：干净克隆即可玩，满足"**写在程序里、何时访问都一样**"；
> - **大世界预制（10 km 级，≈200 MB）⇒ 不入库**，由脚本按需生成。
>
> **生成命令**：`powershell -ExecutionPolicy Bypass -File tools\bake_premade_maps.ps1`
> （脚本按世界清单自动发现 `source = "premade"` 的世界并调用 `voxel_bake`）。
> `.gitignore` 为**按需白名单**：`assets/maps/*.vxmap` 被排除、`!assets/maps/world_b.vxmap` 例外放行。

| 产物 | 是否入库 | 来源（清单 → 地形预设） | 生成命令 | 大小 | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `assets/maps/world_b.vxmap` | **入库**（小世界，1 km） | `assets/maps/world_b.toml` → `assets/maps/world_b_terrain.toml`（种子 20261006，tile 半径 [8, 8]，289 块） | `voxel_bake assets/config/terrain.toml assets/maps/world_b_terrain.toml assets/maps/world_b.vxmap` | 1741224 字节 | `51b37e218ba8bff5f314895e9a3586e59e141c8b717144ca28ffa9537dbc4db4` |
| （10 km 大世界，如 A） | 不入库 | 按世界清单 | 同上脚本 | ≈200 MB 级 | 每次核对后回填 |

> **注意**：改动任何**入库输入**（地形预设 / `assets/config/terrain.toml` / 烘焙代码）都会改变 SHA-256 ⇒
> 需重新运行脚本、更新本表与 `.gitignore` 白名单（与"美术资源台账"同一口径：脚本 + 校验和 + 台账）；
> **入库**的产物还必须与代码**同一次提交**更新，否则干净克隆会与输入不一致。

## 待办

- [ ] 填入 `LICENSE` 中的 `<COPYRIGHT HOLDER>`（当前为占位符）
- [ ] 逐项核对上表标 \* 的许可与版本
- [ ] 首次发布前生成完整的许可原文归档（如 `licenses/` 目录）
- [x] 首套美术资源（地表 PBR 4 套 + 1 张 HDRI，全部 CC0）来源 / 许可 / 作者 / 采集日期 / SHA-256 已登记（2026-09-29）
- [x] **自然物素材（Kenney Nature Kit 选抽 12 个 GLB，CC0）**来源 / 许可 / 作者 / 采集日期 / SHA-256 已登记（2026-10-06，V8）
- [x] **预制地图烘焙产物（`world_b.vxmap`）**来源 / 生成命令 / 大小 / SHA-256 已登记（2026-10-06，V4）
