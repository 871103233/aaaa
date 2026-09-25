# 开发记录（Devlog）

**追加式**：只在末尾新增条目，不回改历史。规则见 [开发规范 · 第六节](../.trae/skills/voxel-engine-dev-standards/SKILL.md)。
最新条目在**文末**。

每条格式：

````markdown
## YYYY-MM-DD  <一句话主题>

- 做了什么：<改动要点，可含关键文件>
- 为什么：<动机 / 被推翻的假设 / 决策依据>
- 验证：<可复现证据——命令 + 真实结果>
- 下一步 / 遗留：<已知未决项，或写"无">
````

---

## 2026-09-25  建立项目骨架、开发规范与构建基线

- 做了什么：技术方案定稿为 `docs/tech-plan-v1.3.md`；建立 AI 开发规范技能 `.trae/skills/voxel-engine-dev-standards/`（正文 + 5 份 references + 结构性禁止门禁脚本）；搭起构建骨架（根 `CMakeLists.txt` + `CMakePresets.json` 五预设 + `vcpkg.json`）；建立目录骨架 `engine/`、`voxel/`、`game/`、`tests/`、`assets/`、`cmake/`；仓库级配置（`.clang-format`、`.clang-tidy`、`.editorconfig`、`.gitattributes`、`.gitignore`、`LICENSE`、`NOTICE.md`）；CI workflow。
- 为什么：先把"决策"与"可检查的约束"落成文件再动代码，避免边写边改架构。方案经两轮评审从 v1.0 修到 v1.3：对齐 SDL3_gpu、补目标硬件基线、修正内存口径（光照应为 96 KB/区块而非 48 KB）、分离 CPU/VRAM 预算、补跨区块依赖与线程模型等缺失章节。
- 验证：门禁脚本自检 `14/14` 通过、扫描 7 个源文件 0 违规；`git ls-files` 36 个文件；首次提交 `9b5301c` 已推送至 `origin/main`。
- 下一步 / 遗留：`LICENSE` 的 `<COPYRIGHT HOLDER>` 仍是占位符；`NOTICE.md` 带 `*` 的条目待逐项核对。此时**尚未编译过任何一行代码**。

---

## 2026-09-25  CI 首次运行 0 job —— 根因与修复

- 做了什么：改写 `.github/workflows/ci.yml`（注释改纯 ASCII、`shell:` 用字面量、去掉 matrix 表达式、门禁拆为 `gate-linux` / `gate-windows` 两个独立 job）；新增 MSVC 环境准备步骤；vcpkg 改用 runner 预装的 `VCPKG_INSTALLATION_ROOT`；补 `CMakePresets.json` 中缺失的 `release` / `relwithdebinfo` 的 `testPresets`。
- 为什么：Run #1 结论 `failure` 且 **0 个 job**，且 `created_at = updated_at = run_started_at` 三个时间戳完全相同 → 判定为 **workflow 启动阶段被拒**，而非构建失败。静态排查先行排除两类可能：GitHub 上的文件与本地逐字一致（3637 字节）、文件纯 LF / 无 BOM / 无 Tab。同时发现两处**独立缺陷**：① `testPresets` 缺 `release`，会让 `ctest --preset release` 必然失败；② Presets 用 Ninja + MSVC，而 hosted Windows runner 默认没有 `cl.exe` 在 PATH，configure 必失败。
- 验证：`ConvertFrom-Json` 解析 `CMakePresets.json` 通过；`buildPresets` 与 `testPresets` 均为 `debug, release, relwithdebinfo, asan, tsan` 五项齐全；`ci.yml` 非 ASCII 字节 = 0、Tab 字节 = 0。
- 下一步 / 遗留：**根因尚未最终确认**。若保守重写后仍出现 0 job，则问题在仓库设置——需确认 `Settings → Actions → General → Actions permissions` 为 "Allow all actions and reusable workflows"。该接口返回 401，无法自动核查。

---

## 2026-09-25  工具链从零装齐；记录沙箱路径屏蔽的边界

- 做了什么：装齐 Ninja 1.13.2、sccache 0.17.0、vcpkg 2026-07-27、LLVM 23.1.2、shaderc 2026.2、sdl3-shadercross 3.0.0-preview2，并配置 `PATH` 与 `VCPKG_ROOT`。VS 2022 Build Tools 与 Python 改由**编辑器沙箱外**安装。
- 为什么：踩到三个坑。① **沙箱终端的 `PATH` 是残缺的**——`cmake` 明明装在 `C:\Program Files\CMake`，`Get-Command` 却报找不到；从注册表重建 `Machine` + `User` PATH 后立刻可见。**教训：探测环境不能只信 PATH，否则会得出"工具全缺"的错误结论。** ② 沙箱按**特定路径**拦截写入：`C:\Program Files (x86)\Microsoft Visual Studio`、`%LOCALAPPDATA%\Microsoft\VisualStudio`、`%LOCALAPPDATA%\Package Cache` 被拒，而 `C:\Program Files\LLVM`、`D:\...` 与手动解压均允许——这正是 VS 安装器（退出码 5002）与 Python MSI 失败的原因。③ winget 的 portable 安装失败，根因是 `%LOCALAPPDATA%\Microsoft\WinGet\Links` 目录缺失。
- 验证：`cmake --version` → 4.4.3；`ninja --version` → 1.13.2；`sccache --version` → 0.17.0；`clang-format --version` → 23.1.2；`vcpkg version` → 2026-07-27；MSVC 工具集 `14.44.35207`；Windows SDK 头文件存在。
- 下一步 / 遗留：Python 仍未安装（`python --version` 返回 9009）。若纹理打包工具需要，需补装。

---

## 2026-09-25  ★ 假设被推翻：Shader 不能只编 SPIR-V

- 做了什么：把 `cmake/Shaders.cmake` 从「GLSL → SPIR-V」改为**两段式**：`glslc` 产出 `.spv`，再由 `shadercross` 转出 `.dxil`；`engine/render/triangle_renderer.cpp` 改为按 `SDL_GetGPUShaderFormats()` 的返回值选择加载 `.spv` 还是 `.dxil`。
- 为什么：方案与技能白名单原本都写定"glslc → SPIR-V"。实际运行**立刻断言失败**：
  `Assertion failure at SDL_CreateGPUShader_REAL: '!"Incompatible shader format for GPU backend"'`。
  进一步实测 `SDL_GPU_DRIVER=vulkan` 得到 `SDL_HINT_GPU_DRIVER vulkan unsupported!`，说明本机（RTX 3080）**没有可用 Vulkan**，SDL3_gpu 落在 **D3D12** 后端，而 D3D12 只接受 **DXIL**。
  → **"只产 SPIR-V"的方案在本机根本不成立，必须双格式并存。** 附带结论：不再需要 Vulkan SDK——vcpkg 的 `shaderc` + `sdl3-shadercross` 两个端口即可完整覆盖。
- 验证：`vcpkg install shaderc:x64-windows` 4.6 min 成功，`glslc.exe` 落在 `installed/x64-windows/tools/shaderc/`；`vcpkg install sdl3-shadercross:x64-windows` 1.2 min 成功，`shadercross.exe` 落在 `installed/x64-windows/tools/sdl3-shadercross/`；构建输出 `转换 Shader：triangle.vert.spv -> DXIL（D3D12 后端）`。
- 下一步 / 遗留：技能白名单与方案文档中"SPIR-V"的表述需同步为"SPIR-V + DXIL 双格式"；另需补一条 ADR 记录该决策。

---

## 2026-09-25  PoC #1 / #2 通过：配置、编译、测试、渲染四关全绿

- 做了什么：修掉 `vcpkg.json` 缺 `builtin-baseline` 导致的配置阻塞（`vcpkg x-update-baseline --add-initial-baseline` → `10541e317a660f4165ba4ac2851ab54a8d4577b1`）；修掉 `VCPKG_ROOT` 被 VS 开发环境覆盖的问题；跑通完整链路。
- 为什么：两个错误相互掩盖。`builtin-baseline` 缺失让 vcpkg 报 `this vcpkg instance requires a manifest with a specified baseline`；而 `Enter-VsDevShell` 会把 `VCPKG_ROOT` 改成 VS 自带的 vcpkg（`…\BuildTools\VC\vcpkg`），使 Presets 指向错误的 vcpkg 实例——**解决方法是在加载 VS 开发环境之后再设置 `VCPKG_ROOT`**。当时跟在后面的 `Ninja not found` / `CMAKE_CXX_COMPILER not set` 都是级联误报，不是真实问题。
- 验证：
  1. **配置**：`cmake --preset debug` → `Configuring done (84.8s)`；vcpkg 装入 `sdl3 3.4.16#1`、`gtest 1.18.0`、`glm`，耗时 50s
  2. **编译**：7/7 目标，`MSVC 19.44.35229.0`，**零错误零警告**（`/W4` + 警告即错误）
  3. **测试**：`ctest --preset debug` → **6/6 passed**（`ChunkStateMachine` ×5、`ChunkGeometry` ×1），0.16s
  4. **运行**：进程持续运行且 `Responding = True`；窗口标题 `Voxel Engine - SDL3_gpu smoke test`，窗口 1296×759；截屏采样 109296 像素中 **10441（9.6%）为高饱和亮像素**、最大饱和度 176 → 彩色三角形确实已渲染
- 下一步 / 遗留：① CI 尚未跑绿——`vcpkg.json` 未含 `shaderc` / `sdl3-shadercross` 的 host 依赖，CI 上 Shader 会被跳过（构建能过但程序跑不起来），需决定是否补入；② Python 未装；③ 技能与方案文档的 Shader 表述待同步；④ 之后进入 V0.1 正式开发（单区块 + 面剔除 + 破坏放置 + 走动跳跃）。

---

## 2026-09-25  规范补「会话交接」约定；Shader 双格式与常驻文档入库

- 做了什么：提交并推送 `a81aaaf`（9 个文件，+848 / −85）——Shader 双格式管线、`ci.yml` 保守重写、`CMakePresets.json` 补 `testPresets`、`vcpkg.json` 补 `builtin-baseline`，并新增三份常驻文档 `docs/devlog.md`、`docs/file-index.md`、`docs/learning-notes.md`。随后在 `SKILL.md` 的「阅读约定」下新增两节——**会话启动先读 5 份「交接包」**、**会话边界按任务阶段开新会话**，并扩展 frontmatter `description`，让新会话开场即触发本技能加载。
- 为什么：项目知识此前只活在对话里，换会话即丢失。把"读什么、按什么顺序读、什么情况换会话、换之前必须落盘"写成硬规则，交接才不依赖人记——这是"能让完全不会写代码的人接手"这条门槛的最后一环。
- 验证：`SKILL.md` 结构完整（frontmatter 仍为合法 YAML 纯量，无引号无冒号），两节均位于「阅读约定（先读这一节）」之下、先于「技术白名单」；`git log --oneline` 显示 `a81aaaf` 已推送，`main` 与 `origin/main` 同步、工作树干净。
- 下一步 / 遗留：① **Trae 的 Markdown 预览只渲染标题**——文件侧已排除（无 BOM、纯 LF、无 Tab/全角空格/零宽字符、无 HTML 注释或标签、代码围栏成对、正文行号可查），GitHub 渲染正常；已按官方排错在 `~/.trae/argv.json` 加 `"disable-hardware-acceleration": true`，**待重启 Trae 验证**。② `vcpkg.json` 未含 `shaderc` / `sdl3-shadercross`，CI 上 Shader 会被跳过（能编过但跑不起来），需决定是否补入。③ `LICENSE` 的 `<COPYRIGHT HOLDER>` 仍是占位符。④ 下一阶段：开新会话进入 V0.1（单区块 + 面剔除 + 破/放方块 + 走动跳跃）。

---

## 2026-09-25  补齐五处规范欠账：NOTICE 回归、ADR 0002、host 依赖、门禁边界、V0.1 玩法引用

- 做了什么：
  1. **修复 `NOTICE.md` 回归**——上一次未提交的表格重排顺手删掉了「待办」下的全部 3 条清单，并把 RenderDoc 行的「引入阶段」误写成"区块状态机与跨区块唯一判据V0.1"。本次回填待办、修正该单元格，并新增 `shaderc`（`glslc`，Apache-2.0）一行（因它本次成了直接依赖）。
  2. **新增 `docs/adr/0002-shader-dual-format-pipeline.md`**（SPIR-V + DXIL 双格式并存：背景、决策、备选对比、后果、重现条件），并在 ADR 0001 头部加「后续更新」互链而不回改其正文。同步 6 处旧表述：`SKILL.md` 技术白名单与第三节编码约定、`references/meshing-and-render.md` §6、`tech-plan-v1.3.md` §8 V0.1 与 §9.1 环境清单及其过时核查记录、`engine/platform/window.cpp` 的 `pick_shader_format` 注释。
  3. **`vcpkg.json` 补 `shaderc` / `sdl3-shadercross` 的 host 依赖**（`{ "name": "...", "host": true }`），并在 `references/build-and-tests.md` §1 写明 host 依赖写法、以及它会传递性拉入 `sdl3[vulkan]` 等带来的首次 configure 代价。
  4. **门禁补 `no-rand` 规则**（`rand()` / `srand()` 破坏生成确定性），自检用例 14 → 18；修正 `.PARAMETER IncludeDir` 注释（漏了 `tests`）；`references/build-and-tests.md` 新增 §6.1「门禁的覆盖边界」，列明哪些红线不可机械化、只能靠 DoD 自检与评审；`learning-notes.md` 的 DoD 条目数 12 → 13，并把 ADR、双格式 Shader、门禁三处交叉引用补齐。
  5. **新增 `references/gameplay-v0.1.md`**（主循环与固定时间步 / 输入 / 玩家移动与碰撞调用侧约定 / DDA 拾取与破坏放置 / `blocks.toml` / `layers.toml` / 调试设施 / 内存与日志 / 两张表的交叉一致性），并在 `SKILL.md` 任务路由表补对应行。
- 为什么：这五条都是架构审查查出的「文档与实测不符」或「规则覆盖不全」，属 DoD 的「方案文档与实际代码同步」欠账。留着的直接后果是下一个人或 AI 会按已推翻的旧结论施工——只编 SPIR-V、以为必须装 Vulkan SDK、以为 CI 上 Shader 工具链可以缺。**附带更正**：上一条记录的「验证」写成"工作树干净"，但该记录所述工作当时并未提交，且其未提交的 `NOTICE.md` 改动含有信息丢失——「工作树干净」不应在未提交时写入。
- 验证：
  1. 门禁：`powershell -NoProfile -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1 -SelfTest` → `Self-test passed: 18 case(s)`；同脚本 `-RepoRoot .` → `Banned-identifier gate: scanned 7 file(s), 0 violation(s). PASS`，退出码 0。
  2. 依赖安装：`cmake --preset debug` → vcpkg 装入 9 个包（新增 `shaderc 2026.2`、`sdl3-shadercross 3.0.0-preview2`，连带 `glslang`、`spirv-tools`、`spirv-cross`、`directx-dxc`），`Configuring done (505.7s)`，退出码 0。
  3. **host 依赖确实让工具可见（CI 路径已证）**：先 `cmake --preset debug -U VOXEL_GLSLC -U VOXEL_SHADERCROSS` 清掉缓存里的旧搜索值使其重新搜索 → `Configuring done (3.0s)`，得
     `VOXEL_GLSLC = D:/…/build/debug/vcpkg_installed/x64-windows/tools/shaderc/glslc.exe`、
     `VOXEL_SHADERCROSS = D:/…/build/debug/vcpkg_installed/x64-windows/tools/sdl3-shadercross/shadercross.exe`。
     即工具来自**本工程**的 `vcpkg_installed`，不依赖全局 vcpkg 或手工 `PATH`。
  4. 编译：`cmake --build --preset debug` → 7/7 目标，退出码 0，零错误零警告（`/W4` + `/WX`）；
     `build/debug/assets/shaders/` 下 `triangle.vert.spv`(1260B) / `triangle.vert.dxil`(3308B) / `triangle.frag.spv`(432B) / `triangle.frag.dxil`(2844B) **四种产物齐全**。
  5. 测试：`ctest --preset debug` → **6/6 passed**，总耗时 0.18s。
- 下一步 / 遗留：① `NOTICE.md` 中新增的 `shaderc` 与既有 `enkiTS` / `Taskflow` / `Lua 5.4` / `sol2` / `SDL_shadercross` 仍标 `*`（未逐字核对上游 LICENSE）。② `LICENSE` 的 `<COPYRIGHT HOLDER>` 仍是占位符。③ 本轮只验证了本机链路，**CI 仍未实测**；注意 `sdl3-shadercross` 会连带要求 `sdl3[vulkan]`，Linux 侧需 `libvulkan-dev`（`ci.yml` 现有步骤已装）。④ Python 仍未安装。⑤ `references/build-and-tests.md` 的命令示例全写 `pwsh`，而本机只有 Windows PowerShell 5.1，需临时改用 `powershell`；待决定是改示例还是在文档注明二者等价。⑥ 下一阶段：开新会话进入 V0.1（单区块 + 面剔除 + 破/放方块 + 走动跳跃），施工按 `references/gameplay-v0.1.md`。

---

## 2026-09-25  收敛两项未决选型；技能新增「技术栈口径统一」机制

- 做了什么：
  1. **收敛两处从未拍板的选型**（原文长期写成"A 或 B"，无法据以开工）：
     - 任务调度 → **enkits**（vcpkg 端口名 `enkits`，无连字符）；未采纳的 Taskflow 与自研无锁线程池写入备选并给出切换条件。
     - ECS → **EnTT（起步）**；未采纳的自研稀疏集写入备选，切换条件为"V0.4 评估 EnTT 实际使用面"。
     新建 `docs/adr/0003-task-scheduler-and-ecs.md` 承载两项的备选对比表与切换条件（上一轮 ADR 0002 只修了"结果"，本轮补的是"从未决策"）。
  2. **消除方案文档的内部矛盾**：§3.1 标准由"C++17 主力 + 预留 C++20 / 子模块逐步试点"收敛为 **C++17 唯一标准**（C++20 移入备选并要求引入走 ADR，与 `CMAKE_CXX_STANDARD 17` 及门禁规则一致）；§3.2「ECS 实现」选型列由"自研稀疏集"更正为 **EnTT**，消除与 §5 / §9.2 / 技能范围控制的矛盾。
  3. **钉死两处模棱表述**：§5 zstd 由"引入方式未定"→ **Vendored 单文件 amalgamation**；§5 Shader 工具链由三工具斜杠并列 → **`glslc` → `SDL_shadercross` 两段式**；并同步 §6.2 资源格式里遗留的"仅 SPIR-V"。
  4. **技能新增防复发机制**：`SKILL.md` 原「技术白名单」升级为「**技术栈：单一事实来源与口径统一**」，含 7 小节——三层职责（**权威层**方案+ADR / **索引层**唯一口径表 / **执行层** references）、唯一口径表（一格一个值并标注权威位置）、vcpkg 端口名与业界通称对照、**选型变更四步流程**、**同步清单 9 项**（第 9 项"源码注释"是上轮实际漏掉的那类）、**口径漂移自查**（可执行的 Grep 关键词表）、**待收敛项登记表**（现登记"遮挡剔除方案"与"LOD 接缝"两项，均要求 V0.5 开工前经 ADR 收敛）。DoD 由 13 项增至 **14 项**：选型变更须按同步清单逐处更新且不得引入新的"A 或 B"。
  5. **按同步清单逐处执行**（这是新规则第一次被自己执行）：`references/concurrency.md`（任务调度口径 + 移除与 ADR 重复的选型理由）、`references/build-and-tests.md`（端口名规则）、门禁脚本 `no-std-async` 的 `Required` 文案、`NOTICE.md`（`enkits（enkiTS）`）、`docs/file-index.md`（`vcpkg.json` 约束列补 host 声明与端口名要求）、`cmake/Shaders.cmake` 两条缺工具提示（原文让用户"安装 Vulkan SDK"，与 ADR 0002 结论冲突）、`game/CMakeLists.txt`、`engine/render/triangle_renderer.hpp`、`assets/shaders/triangle.vert`、`docs/learning-notes.md`（A 区两个名词条目 + B 区 ADR / DoD 条目 + D 区白名单条目并新增 SSOT 条目）。
  6. 顺带修掉一处**非本次引入但同类**的隐患：`docs/file-index.md` 工作区文件是 CRLF，与本仓库"纯 LF、无 BOM"的约定（也是 Trae 预览排查时确认过的约束）不符，已归一化；现全仓库已跟踪文件与新文件均为纯 LF、无 BOM。
- 为什么：ADR 0002 修的是"结论写错了"，但病根是**同一决策在多处重复展开、且没有同步清单**——只改结论不改机制，下次照样漂移。本次把"技术结论只在权威层展开一次，其它层只能引用"写成硬规则，并配 9 项同步清单 + 一组可执行的 Grep 自查，让"漏同步"从"靠人记住"变成"有清单可逐项核对、有命令可验证"。同时，两处"A 或 B"若不在此刻收敛，V0.3 开工时仍要停下来做选型仲裁。
- 验证：
  1. 门禁：`-SelfTest` → `Self-test passed: 18 case(s)`；`-RepoRoot .` → `scanned 7 file(s), 0 violation(s)`，退出码 0。
  2. **口径漂移自查**（按 `SKILL.md` 第 6 节的 Grep 表逐条跑）：
     - `或 \*\* | / \*\* | V 或 Vendored` → 仅命中 `SKILL.md` 自查表自身 1 处（该行就是关键词表，属预期）；
     - `enkiTS | Taskflow` → 余 17 处**全部**为端口名对照、备选说明、ADR 历史陈述、devlog 历史与自查表自身，**无并列候选残留**；
     - `自研稀疏集 | 自研 ECS | 预留 C++20 | 逐步试点` → 余下命中同样全部落在备选说明 / 历史修订记录 / 范围控制（"自研 ECS → 直接用 EnTT"）。
  3. 构建：`cmake --preset debug` → `Configuring done (2.7s)`；`cmake --build --preset debug` → 6/6 目标、退出码 0、**零警告**（`/W4` + `/WX`），Shader 双格式重新产出。
  4. 测试：`ctest --preset debug` → **6/6 passed**，0.07s。
  5. 行尾与编码：全仓库已跟踪文件 + 3 个新文件，CR 字节 = -1、BOM = False。
- 下一步 / 遗留：① 本轮与上一轮改动**均未提交**（累计 17 改 + 3 新）。② `NOTICE.md` 标 `*` 的许可仍未逐字核对；`LICENSE` 的 `<COPYRIGHT HOLDER>` 仍是占位符。③ CI 仍未实测。④ 待收敛项表两项（遮挡剔除、LOD 接缝）**必须在 V0.5 开工前经 ADR 收敛**，否则按违规处理。⑤ 下一阶段：开新会话进入 V0.1（单区块 + 面剔除 + 破/放方块 + 走动跳跃），施工按 `references/gameplay-v0.1.md`。

---

## 2026-09-25  上两轮改动已提交推送；装 gh 并首次查得 CI 真相——Configure 早退，且非本次改动所致

- 做了什么：
  1. 把上两轮积压改动一次提交并推送：`fbcfe4e`（20 文件，+533 / −63，含 3 个新文件），`main` 与 `origin/main` 同步，工作树干净。**上一条遗留①（未提交）就此关闭。**
  2. 安装 **GitHub CLI（gh）2.74.2**：从官方 Release 下 `gh_2.74.2_windows_amd64.zip`，对着同期 `gh_2.74.2_checksums.txt` **校验 SHA256（一致）** 后解压到 `D:\dev\tools\gh`，并把 `D:\dev\tools\gh\bin` 追加进 User `PATH`（沿用 ninja / sccache 的"手动解压"路径，无需提权、不受沙箱写入拦截影响）。
  3. **首次查清 CI 状态**（gh 未登录，故先用公开仓库的免鉴权 API）：
     - `total_count = 3`，其中 run #2（`a81aaaf`）与 run #3（`fbcfe4e`）**结论均为 `failure`**。
     - 两个门禁 job（Gate Linux / Windows）**均 `success`**——说明本次对门禁脚本的改动可用。
     - 4 个 build matrix job **全部在 `Configure` 步失败**，其后 `Build` / `Unit tests` 均为 `skipped`。
     - **关键：run #2 是我动手之前的提交，同样在 `Configure` 失败，Linux 侧仅 2 秒、Windows 8 秒。**
- 为什么：CI 从未跑绿这件事在 devlog 里挂了两轮，一直是"未验证"状态。装了 gh 才能自助查询，而不是让用户反复开网页截图。查之前最大的怀疑是"是不是我把 shaderc / sdl3-shadercross 加成 host 依赖把 CI 弄坏了"——**对比 run #2 后该假设被推翻**：
  - 反证一：run #2 的 `vcpkg.json` 只有 sdl3 / glm / gtest，仍同样失败；
  - 反证二：失败耗时 2~8 秒，连 vcpkg 解析清单都不够，更像是 CMake 在极早期就退出（如 `CMAKE_TOOLCHAIN_FILE` 解析失败、生成器缺失一类的"启动即失败"），而不是依赖安装或编译失败；
  - 反证三：基线提交里 `ports/shaderc` 与 `ports/sdl3-shadercross` **确实存在**（`git ls-tree 10541e31… ports/` 已确认），"端口不在基线里"这条也排除。
  - 结论：**这是既有的 CI 配置缺陷，与双格式 Shader、host 依赖两轮改动无关。** 若不做这次对比，很可能误改 vcpkg.json 去找一个不存在的问题。
- 验证：
  1. `gh --version` → `gh version 2.74.2 (2025-06-18)`；`D:\dev\tools\gh\bin` 已写入 User PATH。
  2. 下载物 SHA256 与官方 `checksums.txt` 逐字符一致（`3ac27af5…eb29`），大小 13956052 字节。
  3. 免鉴权 API：`/actions/runs` → run #2/#3 均 `failure`；`/actions/runs/<id>/jobs` → 门禁 `success`、4 个 build job 在 `Configure` 处 `failure`；`/actions/jobs/<id>/logs` → **403 `Must have admin rights to Repository`**（公开仓库的日志下载仍要求鉴权）。
  4. `gh run list` → 退出码 4，报 `To use GitHub CLI in automation, set the GH_TOKEN environment variable`（未认证，符合预期）。
- 下一步 / 遗留：
  ① **需要用户认证 gh**（`gh auth login` 走浏览器/设备码，或设置 `GH_TOKEN`）——认证后即可 `gh run view <run-id> --log-failed` 直接拿到 `Configure` 的真实报错，无需再猜。这是当前唯一阻塞项。
  ② CI 自建仓以来从未跑绿，`Configure` 早退的根因**尚未定位**（候选：`CMakePresets.json` 里 `$env{VCPKG_ROOT}` 未生效导致工具链文件找不到、或 runner 上 Ninja 生成器不可用），待拿到日志后按证据判定。
  ③ `NOTICE.md` 标 `*` 的许可仍未逐字核对；`LICENSE` 的 `<COPYRIGHT HOLDER>` 仍是占位符。
  ④ 待收敛项表两项（遮挡剔除、LOD 接缝）必须在 V0.5 开工前经 ADR 收敛。
  ⑤ 本轮文档改动（learning-notes C 区与四条环境要点、本条 devlog）**尚未提交**。

---

## 2026-09-25  ★ CI 根因定案：锁定的 vcpkg 基线取不到（shallow 克隆），非编译问题

- 做了什么：用刚装好的 `gh`（用户完成浏览器登录后）执行 `gh run view 36136188745 --log-failed`，拿到了那个挂了三轮的 `Configure` 报错原文；据此在 `.github/workflows/ci.yml` 的 `Locate vcpkg` 与 `Configure` 之间插入新步骤 **`Fetch pinned vcpkg baseline`**，在 configure 之前把锁定的基线提交 fetch 进 runner 的 vcpkg 克隆。
- 为什么：报错全文如下（Linux 与 Windows 一致）——
  ```
  -- Running vcpkg install
  error: while checking out baseline from commit '10541e317a660f4165ba4ac2851ab54a8d4577b1',
         failed to `git show` versions/baseline.json. This may be fixed by fetching commits with `git fetch`.
  fatal: path 'versions/baseline.json' exists on disk, but not in '10541e31...'
  -- Running vcpkg install - failed
  ```
  根因链条：
  1. `vcpkg.json` 的 `builtin-baseline` 是 `x-update-baseline` 从**本机 vcpkg 当时的 main HEAD** 抓来的，该提交日期是 **2026-09-25 01:53** —— 也就是当天最新；
  2. runner 镜像里预装的 vcpkg 是 **shallow 克隆**，HEAD 早于今天，因此取不到这个提交；
  3. vcpkg 解析清单第一件事就是 `git show <baseline>:versions/baseline.json`，失败即中止 —— 所以 `Configure` 在 **2 秒（Linux）/ 8 秒（Windows）** 内退出，`Build`/`Unit tests` 根本没跑。
  → **这是"把基线锁成当天最新提交" + "shallow 克隆"两者叠加的必然结果，与双格式 Shader、host 依赖、门禁改动全部无关**（run #2 已从时间上排除）。日志里随后出现的 `unable to find a build program corresponding to "Ninja"` / `CMAKE_CXX_COMPILER not set` 是同一中止的连带报告，待下一轮 CI 确认是否随之消失。
- 修法要点（避免埋下新的"双份事实"）：SHA **不在 `ci.yml` 里硬编码**，而是用 `grep` 从 `vcpkg.json` 现场提取，保证 `vcpkg.json` 仍是基线的唯一来源——这与本轮刚建立的「技术栈口径统一」原则一致。步骤内先做 `git cat-file -e "<sha>:versions/baseline.json"` 存在性判断：已可解析则跳过，否则 `git fetch --depth=1 origin <sha>`，退一步用 `--unshallow`；最后再断言一次存在性，失败即显式报错，不留"静默降级"。
- 验证：
  1. 基线合法性（本机）：`git -C D:\dev\vcpkg cat-file -t 10541e31…` → `commit`；`ls-tree … versions/` → `100644 blob 494ba512… versions/baseline.json`；`log -1` → `2026-09-25 01:53:17 -0700 | PASSING REMOVE FROM FAIL LISTS 2026-09-24 (#54102)`；`rev-parse HEAD` 与该基线**完全相同**，且 `--is-shallow-repository` = `true`（说明本机能解析是因为它恰是 HEAD，runner 不能解析是因为它不是）。
  2. `ci.yml` 合规：非 ASCII 字节 **0**、CR 字节 **-1**（该文件要求纯 ASCII + 纯 LF）。
  3. `gh auth status` → `✓ Logged in to github.com account 871103233 (keyring)`，scopes `gist, read:org, repo, workflow`；`gh run view <id> --log-failed` 成功返回 62822 字节日志。
  4. **CI 结果待本轮推送后的运行确认**（见下条记录）；`Build`/`Test` 从未在本仓库跑通过，因此它们是否还有独立问题属于未知。
- 下一步 / 遗留：① 观察新 run：基线 fetch 是否成功、Linux 侧 Ninja/编译器报错是否随之消失、若消失则首次真正跑通编译与测试。② runner 上 `C:\vcpkg` 是否为 shallow、`git fetch --depth=1 origin <sha>` 是否被 GitHub 接受，都以新 run 的日志为准（本机无法预演该网络行为）。③ 若 `--depth=1` 取不到，备选是改用自建完整 vcpkg 克隆（代价是 CI 时间）。④ 本轮 `ci.yml` 改动**尚未提交**。
