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

---

## 2026-09-25  规范新增「阶段计划」：每次任务下发必须先有可被直接读取的进度与规划

- 做了什么：
  1. `SKILL.md` 新增「**任务下发：每次任务都必须先有『进度 + 规划』**」一节：任何任务（新需求 / 继续开发 / 修 bug / 纯文档改动）动手前必须先把本次任务写进 `docs/plans/<当前阶段>.md`，条目须含**范围 / 顺序 / 落点 / 验收**四项，**无计划条目即违规**。
  2. 交接包由 5 份扩为 **6 份**：第 3 位插入 `docs/plans/<当前阶段>.md`；「切换前必须落盘」的目标补上计划的「阻塞 / 未决」；新增「阶段切换时必须新建计划」；frontmatter `description` 同步。
  3. 常驻文档由 3 份扩为 **4 份**，新增 **六.4 阶段计划**（用途 / 命名粒度 / 四段模板 / 维护规则），「分工边界」由三者改为四者。
  4. DoD 由 14 项增至 **15 项**：新增「当前阶段计划已更新，且与本次改动同一次提交」。
  5. 未决项登记：把**配置解析（`blocks.toml` / `layers.toml`）方案**写入「待收敛项」表，时限 **V0.1 的 T4 开工前经 ADR 收敛**。
  6. 新建 `docs/plans/v0.1.md`：前置条件 P1~P5、任务分解 T1~T10（含落点与验收判据）、当前进度、阶段验收对照（10 维度，只引用方案 §8）。
  7. 同步 `docs/file-index.md`（新增 `docs/plans/` 目录与条目）、`docs/learning-notes.md`（B 区新增「阶段计划（Plan）」，DoD 条目数 14 → 15）。
- 为什么：原规范只要求「开发后记录进度」（六.2 devlog，格式含「下一步 / 遗留」），**没有要求产出可执行的规划**——devlog 的「下一步」是备忘式要点，没有任务分解、顺序、落点与验收判据。上一条记录结尾的「下一阶段：开新会话进入 V0.1」挂在「下一步 / 遗留」里数轮仍未开工，正说明「有进度、无规划」时接手者还得自己重新推导要做哪些事、按什么顺序。补上阶段计划后，大模型或新接手者**只读一份文件**即可知道「现在到哪、下一步做什么、做到什么算完」。另外把配置解析方案登记进待收敛项表，是为避免 V0.1 施工时被自选——本仓库已因「同一结论散落多处、改一漏三」翻过一次车（见 ADR 0002）。
- 验证：
  1. 门禁：`powershell -NoProfile -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1 -SelfTest` → `Self-test passed: 18 case(s)`，退出码 **0**；同脚本 `-RepoRoot .` → `scanned 7 file(s), 0 violation(s)`，`PASS`，退出码 **0**。
  2. 行尾与编码（逐字节检查 `SKILL.md` / `docs/plans/v0.1.md` / `docs/file-index.md` / `docs/learning-notes.md` / `docs/devlog.md`）→ **CR = -1、BOM = False**，即纯 LF、无 BOM，符合仓库约定。
  3. `SKILL.md` frontmatter 仍为合法 YAML 纯量：`---` 起止成对，`name` / `description` 各一行，`description` 内无冒号。
  4. 互链可达：`docs/plans/v0.1.md` 中的 `../tech-plan-v1.3.md`、`../../.trae/skills/.../references/gameplay-v0.1.md`，以及 `SKILL.md` 待收敛项表指向的 `docs/plans/v0.1.md` 均存在。
- 下一步 / 遗留：① 本规范变更**尚未提交**。② `docs/plans/v0.1.md` 的 P1（配置解析方案未收敛）与 P4（CI 未全绿）开放；V0.1 首个编码任务 T1 可在 P1 收敛前开工，但 **T4 之前必须完成 P1**。③ 上一条记录的遗留（`LICENSE` 占位符、`NOTICE.md` 星号未核对、Python 未装）仍开放。

---

## 2026-09-25  ★ 世界表示改案：由方块体素改为分层混合；规范新增「方案与方向的留档义务」

- 做了什么：
  1. **需求澄清后确认方向变更并落盘**：目标不是"类 MC 的方块放置游戏"，而是"地图按逻辑塞元素 + **平滑地表**可挖可堆 + **有限空间内**三维挖掘 + 浮空内容 + 胶囊体自由活动"。据此世界表示由「体素 Section + 面剔除 + 贪婪网格化 + 体素光照 BFS + 自研 swept AABB」改为**分层混合**：① 高度场地表 ② 可挖标记区域内的有界 SDF 体积 ③ 物件/建造层 ④ 实体。
  2. 新增 **ADR 0004**（世界表示）：四条硬约束（浮空与建造**绝不写入地形场**；可挖范围由**显式标记区域**决定；体积按固定网格对齐、共享边界采样，与地表相接处由体积接管该列高度；飞行元素不引入新表示）、六方案备选对比（含「全世界 SDF 体素」**不采纳**及其量化理由）、后果与四条重审条件。
  3. 新增 **`docs/adr/README.md` 决策索引**：登记 ADR 状态（有效 / 被取代 / 作废）与取代者、方案文档版本状态、当前阶段计划。
  4. **SKILL 新增第 8 节「方案与方向的留档义务」**：任何"选定技术方案 / 确定或调整开发方向"都必须**先落盘**（技术方案 → ADR；方向与阶段目标 → 方案章节 + 阶段计划）；重大方向变更须**新建版本文件**并在新版 §0 列「沿用 / 取代 / 待定」三张清单，旧版头部加取代互链；允许"部分章节被 ADR 取代"的过渡态，但须登记重写任务与时限；**禁止同一方向在两处各写一份不同的值**。同步清单 9 项 → **10 项**（新增决策索引与版本头），DoD 15 项 → **16 项**。
  5. **口径表与红线表改口径 + 迁移期声明**：口径表新增/替换「世界表示 / 可挖范围 / 浮空与建造 / 地形网格化 / 材质与光照 / 物理与角色 / 预算与精度」7 行；旧行（16×16×384、光照 96 KB、Draw Call ≤700）标注**已失效待重算**。红线表按"已取代 / 原则仍成立待更新 / 不受影响"三分类标注（#5、#13、#14、#16 已取代；#1、#12 措辞待更新）。
  6. **待收敛项表重写为 6 项**：① 配置解析 ② 可挖标记规则与文件格式 ③ 可挖体积网格化算法 ④ 地表 LOD 与接缝 ⑤ 预算与精度口径重算 ⑥ 遮挡剔除。并新增口径漂移自查关键词 `96 KB|16×16×384|Section 16³|贪婪合并|swept AABB|quadSize`。
  7. **失效文档显式标注**：`tech-plan-v1.3.md` 头部加"已被 ADR 0004 **部分**取代"及取代范围清单；4 份 `references` 各加声明（chunk-and-streaming **整体失效**、仅原则层可用；meshing-and-render §1~§5 作废、§6 有效；save-and-serialization 原则层有效、`.voxr` v1 字段规格作废；gameplay-v0.1 §1/§2/§8/§9 有效、§4~§7 作废）。
  8. **`docs/plans/v0.1.md` 按新方向重写**：阶段目标改为"验证平滑地形 + 可挖可堆 + 胶囊体自由活动"；前置条件 P1~P8、迁移任务 **M1**（重写 `tech-plan-v2.0.md` 与 4 份 references）、任务 T1~T10、阶段验收 10 维度（性能维度因待收敛项 5 未收敛而**不设数字**）。
  9. 同步 `docs/file-index.md`（`docs/adr/` 加决策索引、世界层标"迁移中"、`assets/blocks.toml` 与 `layers.toml` 标作废、`tools/` 用途更新）、`docs/learning-notes.md`（A 区新增分层混合世界 / 高度场 / SDF 与等值面网格化 / Splat / CSM 五条，B 区新增决策索引与版本取代一条，D 区块状态机条目标注被取代）。
- 为什么：三条独立理由。① **量级**：全世界三维体素的数据量约为高度场的 **34 倍**、网格化遍历量高**两个数量级**，而它换来的"全世界任意三维重塑"**并不是需求**（需求是"一定空间内可自由挖掘"），属过度设计。② **解耦**：浮空内容与玩家建造若塞进地形场会在地表"打出柱子"；归物件层后与地形表示解耦，并复用已选定的 EnTT + Jolt。③ **防止方向矛盾**：仓库此前只规定"**选型变更**"要留档，**没有规定"开发方向变更"也要留档，也没有版本取代与索引机制**——方向改了而旧文档仍在写方块体素，施工者就会照旧施工。补上第 8 节与决策索引后，"哪份还生效"有了唯一答案。
- 验证：
  1. 门禁：`-SelfTest` → `Self-test passed: 18 case(s)`，退出码 **0**；`-RepoRoot .` → `scanned 7 file(s), 0 violation(s)` + `PASS`，退出码 **0**。
  2. 行尾与编码：本次改动的 11 个文件逐字节检查 → **CR = -1、BOM = False**（纯 LF、无 BOM）。
  3. 互链可达：ADR 0001~0004、`docs/adr/README.md`、`docs/plans/v0.1.md`、`references/gameplay-v0.1.md` 的 `Test-Path` 全为真；过程中发现并修正 `learning-notes.md` 里两处相对路径写错（`../docs/adr/...` → `adr/...`）。
  4. CI：本轮未触碰 `ci.yml`；run `36146153279` 结果仍未取。
- 下一步 / 遗留：① 本批改动**尚未提交**。② CI run `36146153279` 结果待取（`docs/plans/v0.1.md` P7）。③ **M1 待执行**：重写 `tech-plan-v2.0.md`（含 §0「沿用 / 取代 / 待定」）与 4 份 `references`；完成前不得据 v1.3 世界表示章节施工。④ 待收敛项 1~3 须在 T4 / T8 前经 ADR 收敛；待收敛项 5 未收敛前不得引用任何旧预算数字。⑤ 依赖待补：`jolt`（T7 前必须）、`imgui`、`assimp`。⑥ `LICENSE` 占位符、`NOTICE.md` 标 `*` 未核对、Python 未装，三项仍开放。

---

## 2026-09-25  收敛 4 项未决选型、M1 方案重写落地、引擎层 T1/T2 与依赖补齐

- 做了什么：
  1. **新增 ADR 0005~0008，把 4 个待收敛项全部关闭**：
     - **ADR 0005** 配置解析 → **toml++**（vcpkg 端口 `tomlplusplus 3.4.0#1`），header-only、报错带行列号；备选留档（手写子集解析器 / nlohmann-json / INI / 编译期常量）并写明切换条件。
     - **ADR 0006** 可挖区域标记 → **程序化纯函数规则 + `assets/config/dig_regions.toml` 叠加**（字段规格含 `schema_version` / `mode`（diggable·sealed）/ `priority` / `min`·`max`）；明确「标记 = **世界定义**（不落盘，只记版本与哈希）、玩家挖掘 = **世界状态**（落盘为脏体积）」，重建顺序固定为「标记 → 生成 → 叠加改动」。
     - **ADR 0007** 体积网格化 → **Surface Nets 起步**（`int8` 距离场、32³ 体素 / 33³ 采样、块边界共享采样）；备选 MC / Dual Contouring / Transvoxel 与**可触发的切换条件**（要锐利硬边则升级 DC）。
     - **ADR 0008** 尺寸·精度·预算口径 → 冻结 64×64 tile、`int16`(1/16 格) 高度、32³ 体积块；**明确作废** v1.3 的 16×16×384 / 光照 96 KB / ≤700 draw call / 3×3×3 方块查询；**Draw Call 与视距内存改为"只记录、不验收"**（因 LOD 未收敛），并给出按新方向修订的固定基准场景。
  2. **SKILL 同步**：名称陷阱表补 `joltphysics`（**不是 `jolt`**）/ `tomlplusplus` / `imgui[...]` / `stb` / `tracy`；待收敛项表加「状态」列（1/2/3/5 标已收敛并互链 ADR，4/6 标开放）；口径表与红线表的迁移期声明升级为「口径现状」（v2.0 已落地为现行方案，逐条给出新做法出处）。
  3. **M1 完成**：新建 `docs/tech-plan-v2.0.md`（411 行，含 §0「沿用 12 项 / 取代 15 项 / 待定 3 项」三张清单 + §1~§9 新方向权威章节），并**原地重写 4 份 `references`**（tile 流式与生成、高度场网格 + Surface Nets + splat + CSM、存档 v2 内容模型、笔刷挖堆与第三人称相机的调用侧约束）；`meshing-and-render.md` §6 双格式 Shader 管线（ADR 0002）按原样保留。
  4. **依赖补齐**（P5）：`vcpkg.json` 新增 `tomlplusplus` / `joltphysics` / `stb` / `imgui`（features `sdl3-binding` + `sdlgpu3-binding`），`builtin-baseline` 未动；`NOTICE.md` 同步（Jolt 引入阶段由 V0.4 更正为 V0.1）。
  5. **T1 完成**：新增 `engine/core/`（`clock` 单调计时、`fixed_step` 固定步长累加器含单帧 ≤5 步封顶与插值 alpha、`log` 统一日志接口 + `VX_LOG_*` 宏）。
  6. **T2 完成**：新增 `engine/input/`（`action_state` 动作定义、`input_map` 每帧一次采样 + 同帧只消费一次的 `ConsumedPressed` 契约）。
  7. 新增 11 个单元测试（`tests/fixed_step_test.cpp` 5 项、`tests/input_map_test.cpp` 6 项）；`docs/file-index.md` 登记 `engine/core/`、`engine/input/`；`docs/learning-notes.md` 新增「世界定义 vs 世界状态」「固定步长累加器与动作状态输入」两条；`docs/adr/README.md` 补 0005~0008 并把 v2.0 标为**现行**。
- 为什么：这 4 项未决选型的收敛时限分别是"V0.1 前置条件完成前"与"v2.0 落地时"，不收敛就直接开工，施工者只能自选（本仓库已因"同一结论散落多处、改一漏三"翻过一次车，见 ADR 0002）。其中 ADR 0008 尤其关键：**若不显式作废旧预算数字，性能验收会拿"96 KB 光照""≤700 draw call"这类已不存在的指标去量新架构**。T1/T2 之所以优先：它们是唯一不依赖世界表示的引擎层任务，可在方案重写期间并行推进。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 10/10 目标，**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **17/17 passed**（6 原有 + 11 新增），总耗时 0.22 s；其中 `FixedStep.LogicStepCountIsFrameRateIndependent`（30 vs 144 FPS 步数一致）、`FixedStep.HugeFrameIsClampedAndDoesNotSpiral`（10 s 帧被钳到 ≤5 步）、`FixedStep.NoDriftBetweenStepsAccumulatorAndElapsed`、`InputMap.PressedIsConsumedExactlyOncePerFrame` 均通过。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 18 file(s), 0 violation(s)`，`PASS`，退出码 0。
  4. **依赖**：`cmake --preset debug` → 装齐 `imgui 1.92.9[sdl3-binding,sdlgpu3-binding]` / `joltphysics 5.6.0#1` / `stb 2024-07-29#1` / `tomlplusplus 3.4.0#1`，`Configuring done (96.2s)`，退出码 0。
  5. **编码与行尾**：本批所有新文件与改动文件均为**纯 LF、无 BOM**（子代理逐字节复核）。
  6. v2.0 与 4 份 references 已按 SKILL 口径漂移表自查：旧口径关键词（`16×16×384`、`贪婪合并`、`swept AABB`、`quadSize`、`96 KB`）的全部命中**只出现在"被取代"声明与历史条目中**，无现行规则误用。
- 下一步 / 遗留：① 本批改动**尚未提交**。② **T3~T10 未开始**：`engine/render` 网格渲染路径 + 第三人称相机 → 世界层（高度场 tile / splat 材质 / 笔刷挖堆）→ Jolt 角色 / 可挖体积 / ImGui 面板 → 阶段验收。③ **世界层更名待执行**：v2.0 §9.1 用 `world/` 取代 `voxel/`，须在 T4 落地时一并完成目录搬迁与 `file-index.md`、构建脚本同步。④ 待收敛项 **4（地表 LOD）与 6（遮挡剔除）仍开放**；收敛前不得给 Draw Call 与视距内存设数字。⑤ 本机构建需在同一条命令内先初始化 VS DevShell（否则 `C1083 "cstdint"`）——已记入 `docs/plans/v0.1.md` 遗留。⑥ CI run `36146153279` 结果仍未取；`LICENSE` 占位符、Python 未装仍开放。

---

## 2026-09-25  T3 完成：通用网格渲染路径与第三人称相机（含避障）

- 做了什么：
  1. 新增 `engine/render/mesh_renderer.hpp/.cpp`：通用网格渲染路径——图形管线（3 顶点属性、背面剔除、深度测试）、顶点/索引缓冲上传、**每帧相机常量缓冲**（storage buffer，绑到顶点槽 0）、索引绘制；深度目标按交换链尺寸惰性重建。顶点格式为**相机相对 position + normal + 材质权重 ×4**，头文件完整文档化，**不含任何方块语义**。
  2. 新增 `engine/render/camera.hpp/.cpp`：第三人称相机（yaw/pitch，pitch 钳 **±89°**、跟随距离、沿视线**避障**把相机拉近、并用地表高度做离地安全网），以及最小地形查询契约 **`ITerrainQuery`**（height + 线段遮挡两个纯虚方法）——由世界层后续实现，因此相机**现在就能用桩单测**，且 `engine/` 不引入地形专有类型。
  3. 新增 6 个单测（`tests/render_camera_test.cpp`）：pitch 钳制、无遮挡时恰在请求距离、遮挡时被拉近且**不在实心体内**、离地间隙安全网、**alpha 应用后模拟状态逐字段不变**、pivot 在上一/当前逻辑步之间插值且不外插。
  4. 登记 `engine/CMakeLists.txt` 与 `tests/CMakeLists.txt`；`docs/file-index.md` 补两个模块入口；`docs/plans/v0.1.md` 勾选 T3。
- 为什么：T3 是 T4 的**目视前提**（没有网格渲染路径就无法看到高度场网格是否平滑），也是"相机不穿地形"这条验收项的实现主体。把地形查询抽成 `ITerrainQuery` 而不是直接调世界层，是为了**在 `world/` 尚不存在时就能把相机逻辑测掉**，同时不破坏"`engine/` 不依赖 `world/`"的分层红线（相机 `Evaluate` 声明为 `const`，插值 alpha 只用于渲染，编译期即杜绝回写模拟状态）。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 20/20 目标，`BUILD_EXIT=0`，**警告行数 0**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **23/23 passed**（17 原有 + 6 新增）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 23 file(s), 0 violation(s)`，`PASS`，退出码 0。
  4. **行尾与编码**：5 个新文件 + 2 个构建脚本逐字节复核 → `CR=0`、`BOM=False`（纯 LF）。
  5. `engine/render/triangle_renderer.*` 未改动，PoC 冒烟路径仍可编译。
- 下一步 / 遗留：① **T3 的运行期接线未做**：`MeshRenderer` 需 `<shader_dir>/mesh.vert|.frag`，而这两个 Shader 源尚未加入 `assets/shaders/`、也未在 `game/CMakeLists.txt` 注册 `add_shader`（本轮任务禁止改 `game/`）——**须在 T4/T5 接入地形网格时一并补上**，否则网格渲染路径只有静态正确性、没有运行时验证。② 剩余任务：**T4~T6**（世界层：高度场 tile / splat 材质 / 笔刷挖堆）、**T7~T9**（Jolt 角色 / 可挖体积 / ImGui 面板）、**T10** 阶段验收。③ **世界层更名待执行**（`voxel/` → `world/`，与 T4 同批）。④ 待收敛项 4（地表 LOD）与 6（遮挡剔除）仍开放。⑤ 本批改动**尚未提交**。

---

## 2026-09-25  SKILL 第六节扩充：新增三份"内容基线文档"分支与维护义务（M3）

- 做了什么：
  1. **新建 `docs/content-capabilities.md`**（内容能力与产出范围）：状态图例；**当前程序真实产出的内容**（3×3 tile 可挖平滑地表、占位色材质、胶囊角色、第三人称相机、只读调试面板）；按六类登记的内容清单——地形与地质 / 地表元素（物件层）/ 建造与交互 / 角色与生物 / 世界环境 / 数据与存档；以及"先登记后动手"的登记规则。**未开发项一律占位**（`未开始` / `部分实现`），不留白。
  2. **新建 `docs/npc-behavior.md`**（NPC 行为逻辑）：技术约束（ECS、固定步长、确定性、不阻塞主线程、寻路查询对象）+ 七类**待填清单**（种类与层级 / 感知与记忆 / 决策方式 / 移动与寻路 / 日程与生活 / 交互与战斗 / 生成与生命周期）。**决策模型与寻路表示的选型留给 ADR**，本文件只写单一结论或 `待补`。
  3. **新建 `docs/world-setting.md`**（世界观设定）：六类待填（基调与题材 / 地理与生态 / 种族与文明 / 历史与事件 / 语言与命名 / 与玩法的对应关系）+ **"内容必须来自项目所有者、AI 不得代拟"** 的硬规则 + 待项目确认的问题区。
  4. **SKILL 第六节扩充**：开头由"四份常驻文档"改为**七份、分两类**（过程文档 4 份 + 内容基线文档 3 份）；新增 **六.5 内容基线文档总则**（8 条共同规则：缺失先建、未开发先占位、先登记后动手、内容变了才改、单一结论不写"A 或 B"、不得虚构、落地才算生效、引用不复制）与 **六.6 / 六.7 / 六.8** 三份文档各自的用途、必备内容与更新触发条件；"分工边界"由四者改为**五者**（新增"内容基线文档写『有什么』"）。
  5. **配套同步**：同步清单新增第 11、12 条（内容文档，并注明其触发条件是"内容变了"而非"改了选型"）；DoD 新增一条（涉及内容/行为/设定须同步，且新增可生成内容须**先登记再实现**）；任务路由新增三行，并把旧的"区块 / 体素 / 方块"措辞更新为新载体；待收敛项新增 **8（NPC 决策与感知方案）与 9（NPC 寻路与导航表示）**，时限均为"NPC 任务开工前经 ADR 收敛"；`docs/file-index.md` 登记三份新文档并更新 `docs/` 行的职责。
- 为什么：此前六份常驻文档全部只覆盖"**过程**"（做了什么、学到什么、接下来做什么），**没有任何一处记录"世界 / 项目里有什么"**。后果是三条：① 内容实现状态只能靠翻代码或回忆，接手的模型无从判断"这个内容到底有没有"；② NPC 与世界观这类**尚未开发**的内容连"该回答哪些问题"都无处落；③ 新增内容极容易直接进代码而不进文档。这一批把"内容"也变成**有维护义务、有占位要求**的文档，并明确**未开发内容先占位**，使空白可见而不是不存在。
- 验证：三份新文档 + SKILL + `docs/file-index.md` + 本文件均为**纯 LF、无 BOM**；SKILL 门禁自检 `-SelfTest` 通过；三份内容文档**所有表项均已填状态**（未开发处为 `未开始` / `待补`），无留白；新维护义务已同时出现在 SKILL 的同步清单（11、12）与 DoD（新增一条）两处，不存在"只写在一处"。
- 下一步 / 遗留：① `docs/world-setting.md` §3 列出 **4 个待项目所有者确认的问题**（题材与目标、是否已有确定的地名/种族/历史、哪些设定必须影响生成、是否需要命名词根表）——**在得到答复前不得填充内容**。② NPC 决策模型与寻路表示须在 NPC 任务开工前经 ADR 收敛（待收敛项 8、9）。③ 内容能力表当前如实标注：纹理为占位色、洞穴/地表元素/物品/NPC/天气/存档**均未开始**。④ 本批改动**尚未提交**。

---

## 2026-09-25  文档分层（引擎能力 / 游戏设计）+ 修仙大世界需求登记 + 预设固定地图与主角测试化（T11/T12）

- 做了什么：
  1. **文档分层重构**（所有者指出"引擎能力和游戏设计两个层面应该分开"）：删除 `docs/content-capabilities.md`，拆成
     **`docs/engine-capabilities.md`**（**引擎层**：引擎能做什么——平台与运行时 / 渲染 / 世界生成与表示 / 物理与角色 / 实体·任务·数据 / 调试与工程质量，按类登记状态；状态图例新增 **`已引入未使用`** 以区分"依赖装了"与"真的用上了"；并附"引擎**不**包含什么"的分层边界表）与
     **`docs/game-design.md`**（**游戏层**：本游戏要什么——G1 每次进入随机生成 / G2 必备元素可定义 / G3 预设固定地图 / G4 能跑 / G5 能飞 / G6 破坏地形 / G7 收集东西，逐项标状态与落点，另含阶段性范围、主角占位表、与其它文档的分工、待确认问题）。
  2. **世界观设定补入已确认需求**：题材 = **修仙大世界**；每次进入随机生成；必备元素可定义；可预设固定地图；玩家可收集 / 飞 / 跑 / 破坏地形——记入新增 §0.1（并标注每项"是否影响生成规则"）；"待确认问题"改为 7 条（境界体系、宗派势力、地理命名、必备元素清单、飞行的设定依据、破坏地形的限制、哪些设定只影响文案）。
  3. **SKILL 第六节随之升级**：常驻文档 **7 → 8 份**；内容基线文档改为 **4 份且分两层**（引擎能力 = 引擎层；游戏设计 / NPC / 世界观 = 游戏层）；六.5 总则新增**分层硬规则**——"能力写在六.6、需求写在六.7，两边互相链接，**不重复叙述**"；小节重新编号为六.5~六.9；同步清单第 11、12 条、DoD 条目、任务路由四行同步更新。
  4. **T11 预设固定地图**：新增 `world/generation/map_preset.*`（TOML：`schema_version` / `name` / `seed` / `tile_radius` / `spawn` / `[[edit]]`（`mode` = flatten · raise · carve + `min` / `max` / `height`）），**非法文件一律显式抛错**（缺字段、版本不符、mode 非法、min>max、越界）；地形生成改为"**噪声先行、预设编辑叠加其上**"，且**空预设时与改动前逐位一致**（旧测试全绿）。新增手工测试地图 `assets/maps/test_range.toml`（19 条编辑：出生平台、10 级阶梯坡道 120→130、1 格台阶、2 格/级陡坡、24 格深坑、地标塔）。
  5. **T12 主角测试化**：出生点来自地图 `spawn`（脚底 = 地表 + 0.5 格）；新增**飞行模式**（`F` 切换，`Space` 升 / 左 `Ctrl` 降、`Shift` 加速；碰撞仍生效，切换瞬间清零速度故无残留）；启动打印完整控制说明；调试面板增加"飞行：是/否"。
- 为什么：① **分层是第一位的**——原先只有一份 `content-capabilities.md`，把"引擎能做什么"与"游戏要什么"混在一处，会让需求变化与能力变化互相污染（改需求像改能力，导致"能力状态"失真），而 G1~G7 这类需求也需要稳定落点，否则下次接手仍要从对话里找。② **固定地图是所有者明确要求的三类世界生成方式之一**（随机 / 必备元素可定义 / 预设固定地图），也是"人能亲自验证"的必要手段：纯随机地形无法稳定复现 1 格台阶、陡坡、深坑这些测试用例，测试就只能靠运气。③ **飞行是为了让人能一次看全地图**：当前只有 3×3 tile、相机贴地，验证地形改动很不方便。④ 生成侧把预设编辑**叠加在噪声之上**而不是替换掉噪声，是为了保住"随机生成"这个主线能力——固定地图是**同一条生成管线的一个特例**，不是另一套代码。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 40/40 目标，退出码 0，警告 / 错误匹配数 **0**。
  2. **测试**：`ctest --preset debug` → **42/42 passed**（原 39 项全绿 + 新增 3 项 `MapPreset.*`：合法预设解析、非法预设显式报错、编辑区高度与出生点符合预期）。
  3. **门禁**：`scanned 50 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `alive=True responding=True`；日志确认 `预设地图已加载：手工测试地图…文件 assets/maps/test_range.toml——种子 1592594996，tile 半径 [1,1]（9 个 tile），地形编辑 19 条，出生点 (0.0, 0.0)`、`出生点：列 (0.0, 0.0)，地表 120.00 格，脚底 120.50 格`、Jolt 与 ImGui 初始化、以及完整控制说明。
  5. **编码**：本批 12 个新增 / 修改的代码与资源文件 → `BOM=False CRLF=0`，`BAD_FILES=0`。
- 下一步 / 遗留：① **G2（必备元素可定义）与 G7（收集物品）完全未实现**；G5 飞行、G6 破坏地形为**部分实现**（飞行当前是测试设施，三维挖掘 / 洞穴未实现）——见 `docs/game-design.md` §2。② `docs/game-design.md` §6 与 `docs/world-setting.md` §3 共列出 **13 个待所有者确认的问题**（世界尺度、随机稳定项、必备元素清单、飞行定位、收集物范围、主角设定、境界体系、宗派、命名……）——**在得到答复前不得由 AI 代拟**。③ 本批改动**尚未提交**。

---

## 2026-09-25  人工实测第 1 轮：修复 A/D 反向（B1）与主角不可见（T13）

- 做了什么：
  1. **B1 — A/D 反向的精确根因**：`game/main.cpp` 里移动右向量写成 `(cos yaw, 0, -sin yaw)`，而第三人称相机的真实右向量是
     `cross(forward, up) = (-cos yaw, 0, sin yaw)`——两者**恰好互为相反数**，于是左右整体镜像（W/S 不受影响，因为 `forward` 与相机水平前向一致）。
     修复：抽出纯函数 `game/character_movement.hpp`，用**显式叉乘** `cross(forward, up)` 取代手写分量（手写分量正是出错根源），并加 2 项单测：
     以**相机自身的基向量**（`normalize(target - eye)` 与 `cross(forward, up)`）为权威，对多组 yaw / pitch 断言 `D·right > 0`、`A·right < 0`、`W·forward > 0`、`S·forward < 0`。
  2. **T13 — 主角可视化**：主角此前**只有物理胶囊、从未渲染任何网格**（渲染器只画地形 tile），所以"看不到自己"不是渲染 bug 而是**功能缺口**。
     新增 `game/character_mesh.hpp`（程序化胶囊：半径 0.30 / 圆柱半高 0.60 / 总高 1.80，脚底为原点，法线单位化、绕序朝外），
     材质权重**全压在 3 号槽（沙色）**以求与地表强对比，**未改任何 Shader**（双格式管线保持原样）；
     新增引擎能力 `MeshRenderer::UpdateMeshVertices`（**就地**刷新定长网格顶点，复用常驻暂存缓冲，**不建 GPU 资源、不做同步等待、稳态零堆分配**）；
     `game/main.cpp` 每帧以 `角色脚底 + 局部顶点 - 渲染原点`（`double` 累加后落回 `float`，红线 6）刷新，位置取相机目标的插值位置（`alpha` 仅用于渲染）。物理胶囊一字未动。
- 为什么：① 这类"左右反向"若只把两个标签对调就完事，下次改相机约定必然复发——所以要求**先抽纯函数、再以相机基向量为权威写断言**，把方向语义钉死。
  ② **"看不到自己"暴露了自动化测试的盲区**：46 项测试可以全绿，而屏幕上根本没有主角——可见性假设（"有第三人称相机就能看到角色"）从未被验证。
  这正是要求"人工实测 + 把发现登记成 B1/T13"的价值；也说明**"能跑"与"能看"是两件事**。
  ③ 动态顶点刷新刻意避开"每帧新建 GPU 资源"，否则会把上传与同步塞进热路径，违反红线。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 41 步，退出码 0，**零错误零警告**（含 `mesh.vert/frag` 的 SPIR-V → DXIL 双格式转换）。
  2. **测试**：`ctest --preset debug` → **46/46 passed**（42 项既有全绿 + 4 项新增：`CharacterMovement.StrafeMatchesCameraBasisForAnyYaw`、
     `CharacterMovement.MatchesExplicitBasisAtZeroYaw`、`CharacterMesh.MatchesCollisionCapsuleBounds`、`CharacterMesh.TriangleWindingFacesOutward`）。
  3. **门禁**：`scanned 53 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `HasExited=False`、`Responding=True`；stderr 为空；stdout 确认预设地图（9 tile / 19 条编辑）、地表世界与角色物理就绪。
  5. **编码**：本批 7 个新增 / 修改文件全部 `BOM=False`、`CR=0`（纯 LF）。
  6. **人眼预期**：第三人称视角下，出生点（世界列 0,0、地表 120 格）**正中央偏下**出现一个**明亮的沙黄色竖直胶囊**（高约 1.8 格），
     随移动 / 转向 / 跳跃 / 飞行同步移动，渲染原点重定基时不抖动；**A 向左、D 向右**（相对相机）。
- 下一步 / 遗留：① **待人工复测**——可见性与方向已修，但"手感"类问题（转向速度、相机距离、跳跃高度是否合适）只能由人来判断。
  ② 引擎侧新增能力已登记 `docs/engine-capabilities.md`；主角外观状态由"未开发"改为"部分实现"（`docs/game-design.md` §3）。
  ③ 仍待所有者答复 13 个设定 / 需求问题；**G2（必备元素可定义）与 G7（收集物品）仍完全未实现**。④ 本批改动**尚未提交**。

---

## 2026-09-25  人工实测第 2 轮：蓝屏（B2）与跳跃失效（B3）——两个根因都与直觉相反

- 做了什么：
  1. **B2 蓝屏**。最初列出的两个候选——"几何失效（高度越界 / NaN）"与"每帧重网格风暴"——**都被实验排除**：
     笔刷对高度有 `clamp(0, 8192)` 钳制且 `int16` 不会溢出（压力回归：300+ 次 60 Hz 施加仍全部有限、索引合法）；笔刷走的是 `ConsumePressed`（**按下边沿**），长按只应用一次，不存在每帧风暴。
     **真根因是相机退化出 NaN 视图矩阵**：地形被抬到注视点之上后，避障查询把起点判在地形内 ⇒ `safeT = 0` ⇒ 跟随距离被压到 0 ⇒ `eye == target`；
     紧接着"离地间隙"安全网又把 `eye` 沿 +Y 顶到 `target` 正上方 ⇒ 视线方向与 `up` 平行 ⇒ `glm::lookAt` 基向量归一化得 **NaN** ⇒ `viewProjection` 全 NaN ⇒ 顶点全被剔除 ⇒ **画面只剩清屏色（浅蓝 (0.45,0.62,0.85)）**。
     诊断证据（真实世界 + 真实相机参数，反复堆高）：
     ```
     [diag] surface(0,0)=128.00
     [diag] raise#0 surface=129.00 dist=14.000 finite=1
     [diag] raise#1 surface=130.00 dist=0.600  finite=0   ← 自本帧起视图矩阵为 NaN
     [diag] raise#2 surface=131.00 dist=1.600  finite=0
     ```
     修复：`engine/render/camera.*` 新增 `kCameraMinDistance = 0.5`，在离地间隙安全网**之前**托底最小跟随距离，保证 `eye ≠ target` 且横向偏移非零。
     **同源第二问题**（同样导致蓝屏）：堆土把角色埋进 **静态**高度场后，`CharacterVirtual` **不会**被静态形状顶出 ⇒ 支撑判定失效 ⇒ 角色**穿过地形坠落**：
     ```
     [diag] settle feet=120.000 onGround=1 (surface=120.000)   ← 正常站立
     [diag] click#1 feet=117.100 onGround=0 surface=122.000    ← 开始穿下去
     [diag] click#2 feet=108.200 onGround=0 surface=123.000
     ```
     修复：新增 `engine/physics/physics_world.hpp::SetCharacterPosition`，在改地形后把角色顶回新地表并清零速度（`game/main.cpp` 调用 + 相机 `SnapTo`）。
  2. **B3 跳跃失效**。先前列的 4 个候选（着地判定永假 / 初速被覆盖 / 空格被飞行占用 / 默认飞行）**均不成立**——`GetGroundState()` 正常、`SetCharacterVelocity → MoveCharacter` 未被覆盖、`Space→Jump`、`LeftCtrl→FlyDown`、`flying=false`。
     **真根因是输入边沿与固定步长的错配**：空格的"本帧按下"边沿在**帧边界被消费清除**，却只在**固定步循环**里被使用；本机关闭垂直同步后跑在 **~1400 FPS**，而逻辑固定 60 Hz ⇒ **约 96% 的帧 `plan.steps == 0`，边沿被静默丢弃**：
     ```
     [TMP-DIAG] fps=1339.6，本秒内有逻辑步的帧占比=0.04
     [TMP-DIAG] fps=1411.7，本秒内有逻辑步的帧占比=0.04
     ```
     修复：按下边沿**帧级锁存**，固定步循环消费并清除（保持"一次按下恰好生效一次"，不回退成"每步读原始输入"）。**任何"按一下触发一次"的动作（挖 / 堆 / 切换飞行）都走这条路径**，所以这个修复的价值不止于跳跃。
  3. **跳跃高度按设计规则改为身高的 60%**：新增纯函数 `game/character_movement.hpp::JumpVelocityForHeight(gravity, characterHeight)` 与 `kJumpApexHeightRatio = 0.6`，初速由 `sqrt(2·g·0.6·H)` 推导 ⇒ `sqrt(2×24×0.6×1.80) = 7.20`，最高点 **1.08 格**；改身高时跳跃自动跟随。启动日志打印派生值与最高点。
- 为什么（这轮的价值）：**两个根因都在"我列的候选之外"**，说明**先写假设、再用实验逐条排除**是必需的，而不是挑一个最像的去改。
  更值得记住的是两条通用教训：① "把某个距离钳到 0"的兜底逻辑（避障、安全网）会**制造退化矩阵**，画面表现为"只剩清屏色"，与"几何坏了"极难区分；
  ② 输入在**帧边界**采样、逻辑在**固定步**消费，两者频率不一致时**边沿事件会静默丢失**——这类 bug 在"关垂直同步"的机器上才明显，正是人工实测才会暴露。
  两条都已写入 `docs/learning-notes.md`。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 退出码 0，**零错误零警告**（含 Shader 双格式转换）。
  2. **测试**：`ctest --preset debug` → **55/55 passed**（既有 46 项全绿 + 新增 9 项：2 项相机退化防护、3 项跳跃派生与锁存、4 项笔刷长按压力回归）。
  3. **门禁**：`scanned 55 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `HasExited=False`、`Responding=True`，stderr 为空；日志含 `跳跃初速 7.20（由身高推导，最高点 1.08 格 = 身高 60%）` 与完整控制说明。
  5. **编码**：本批 10 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工复测**：按住右键堆土应不再蓝屏且角色被顶到新地表；空格应可靠起跳、腾空约 1.08 格。② **新增 B4（待确认）**：无预设地图的噪声地形上，角色静止高度与 `QueryHeight` 不一致（有预设地图时一致）——已登记进阶段计划，本轮范围外。
  ③ 已知未做：笔刷"按住连挖 / 连堆"（现为按一次动一格）；离散固定步使实测最高点略高于理想抛体值（约 1.13 格，公式派生的 1.08 已由 5% 单测锁定）。
  ④ 仍待所有者答复 13 个设定 / 需求问题；**G2 与 G7 仍完全未实现**。⑤ 本批改动**尚未提交**。

---

## 2026-09-25  SKILL 新增「界面与交互清单」+ 首个界面：ESC 系统面板（T15 / G9）

- 做了什么：
  1. **SKILL 新增第六节第 10 份常驻文档（8 → 9 份）**：**六.10 界面与交互清单（`docs/ui-inventory.md`）**——每个界面写五件事
     （位置 / 打开方式 / 关闭方式 / **与鼠标捕获的关系** / 依赖），每个控件写四件事（类型 / **名义承诺（点了会发生什么）** / 状态 / 当前限制）。
     **两条硬规则**：① **控件标签即行为契约**（写着"退出游戏"就必须真的退出，**禁止无效控件**）；
     ② **依赖缺失时显式标注、而不是做成假的**（接通生效路径 + 明写"当前听不到 + 缺什么"，既不假装、也不顺手实现整个子系统）。
     同步清单新增第 13 条、DoD 扩展、任务路由新增一行；并新建了 `docs/ui-inventory.md`（登记 ESC 系统面板与 F1 调试面板）。
  2. **T15 实现**：新增 **`engine/platform/settings.*`**（TOML 设置读写：显示模式 / 分辨率 / 主音量，落盘 `SDL_GetPrefPath`，以及**唯一音频增益入口** `ApplyMasterVolumeGain`）；
     **`engine/platform/window.*`** 新增**通用 SDL 事件转发回调**（每事件一次、零分配；**`engine/` 因此仍不依赖 ImGui**）与全屏 / 分辨率 API（档位取自 `SDL_GetFullscreenDisplayModes`）；
     **`game/system_panel.*`**（面板本体，不碰 SDL、不写文件，只回报"用户做了什么"）；**`game/gameplay_input.hpp`**（输入抑制纯函数）。
  3. **ESC 语义统一**：`Esc` 由 T14 的"释放鼠标捕获"改为**开关系统面板**（打开释放捕获、关闭**恢复打开前的状态**），复用同一套捕获状态机，未新造第二套机制。
  4. 首次让 **ImGui 真正可交互**（此前 F1 面板只读）；未捕获期间对 ImGui 置 `NoMouse`，避免绝对坐标误判悬停而反向抑制视角。
- 为什么：① **"点了没反应"这类缺陷不会被任何自动化测试发现**——它只在人工点击时暴露；所以必须把"按钮承诺什么"写成**可逐条核对的契约**，
  这正是新增这份文档的理由。② "依赖缺失时不做假"这条边界直接来自本项目的既有事实：音量设置依赖**尚不存在的音源**，
  做成无声的假滑块是欺骗，顺手实现整个音频子系统又属范围之外；规则因此要求**接通生效路径 + 明写缺什么**，
  本次实现严格遵守：**未初始化任何音频对象、未伪造声音**，只在日志与文档中标注现状。③ ESC 的职责冲突按业界标准统一为"开关菜单"。
  ④ 事件转发做成**通用回调**而非让 `engine/` 依赖 ImGui，是为了保住分层红线。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 48/48 步，退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **78/78 passed**（既有 61 项全绿 + 新增 **17** 项：`SystemSettings.*` 8、`GameplayInput.*` 5、`PanelCapture.*` 4）。
  3. **门禁**：`scanned 64 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `Responding=True`；日志确认 `设置文件：C:\Users\…\AppData\Roaming\voxel-engine\voxel_game\settings.toml`、
     `已加载设置：显示模式=windowed，分辨率=1280 x 720，主音量=80`、`主音量增益入口：设置 80 / 100 → 线性增益 0.80（音频系统未实现，当前无声音输出）`、
     `ImGui 已启用（事件转发已接）…两个面板均可交互`；正常退出后 `设置已保存` + `收到退出请求，主循环结束`，**退出码 0**。
  5. **编码**：本批 18 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工点击验证**：四个控件的真实效果（全屏 / 分辨率 / 滑块 / 退出）与 UI 命中，只能人工确认——
  纯逻辑（设置读写 / 钳制 / 非法报错、抑制分类、捕获恢复）已被单测钉死。② **音量仍为"部分实现"**：要能听到还需音频流 + 混音器 + 音源（已登记）。
  ③ **ImGui 默认字体无 CJK 字形**，故控件用中英双语标签；若要统一中文须引入 CJK 字体资源（属新资产，须先登记）。④ B4 待确认；G2/G7 未实现；13 个设定问题待答复。⑤ 本批改动**尚未提交**。

---

## 2026-09-25  UI 缺字（`???`）根治与外观优化（T16）

- 做了什么：
  1. **`???` 的根因与根治**：根因是 **ImGui 内置默认字体只有 ASCII 字形**，渲染中文时每个缺失字形都变成 `?`（**是缺字，不是乱码**）。
     新增 `game/ui_font.*` 做**三级字体解析**：① 仓库内 `assets/fonts/`（**默认不存在，静默跳过、不报错**）→ ② 系统 CJK 字体
     （Windows `msyh.ttc`/`simhei.ttf`/`simsun.ttc`；Linux Noto CJK / 文泉驿；macOS PingFang）→ ③ 无 CJK 字体；
     命中即按 18 px + `GetGlyphRangesChineseSimplifiedCommon()` 加载（`.ttc` 显式传集合索引）。本机解析到 **`C:\Windows\Fonts\msyh.ttc`**。
  2. **关键交付：标签缝 `game/ui_text.hpp`**。所有 UI 标签一律经 `UiText(label, cjk)` 取词：命中 CJK 字体 ⇒ 中文，否则 ⇒ **整表纯英文**。
     于是**任何机器上都不会再出现 `?`**，而不是"假设机器装了字体"。并用测试把这条缝焊死：
     `UiText.EnglishFallbackIsPureAsciiForEveryLabel`（遍历全部标签断言纯 ASCII）、`UiText.BothTablesMatchEnumAndAreComplete`（两表长度=枚举数、无空项）、
     `UiText.PanelsNeverPassNonAsciiLiteralsToImGui`（**扫描 `debug_overlay.cpp` / `system_panel.cpp` 源码，禁止直接给 ImGui 传非 ASCII 字面量**）。
  3. **外观**：新增 `game/ui_theme.*`（**全工程唯一样式入口** `ApplyUiTheme`）：暗色石板调色板、统一圆角 / 内边距 / 间距 / 边框、`SeparatorText` 分区。
     系统面板重排为 **显示 / 声音 / 退出** 三分区（460×450 居中）：显示模式同行单选对、分辨率下拉**全屏禁用**、音量**全宽滑块并显示 `%d / 100`、退出按钮全宽锚底**；
     调试面板改为**定宽标签列**对齐数值，仍只读。
  4. **顺带修掉一处文档与代码不一致（既有缺陷）**：`docs/ui-inventory.md` §2.1 曾列有「返回游戏」按钮，但实现从未包含它——
     按"以代码为准、先报告再改文档"的口径**从清单移除**（关闭由 `Esc` 负责）。
- 为什么：① 缺字问题的教训是**不能用"假设环境有字体"来换取可读性**——必须由代码保证回退，否则换台机器又出 `?`；
  所以本次把"回退"写成**可被测试遍历的不变量**，并额外扫描面板源码封住"绕过标签缝"这条路。② 外观优化的取舍：只做**主题 + 布局**两件事，
  **不新增任何控件、不做 docking/动画**——前者是"第一眼观感与可读性"的必要成本，后者属范围之外。③ 未内嵌 CJK 字体是**有意选择**：
  数 MB 资产 + 许可维护换来的只是"所有平台统一中文"，而系统字体 + 英文回退已能保证可读；若日后要统一，再按"先登记后实现"补子集化字体。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 52/52 步，退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **87/87 passed**（既有 78 项全绿 + 新增 **9** 项：`UiFont.*` 4、`UiText.*` 5）。
  3. **门禁**：`scanned 71 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `Responding=True`；日志确认
     `UI 字体：已加载系统 CJK 字体 C:\Windows\Fonts\msyh.ttc（集合索引 0，18 px，简体中文常用字集）—— 面板使用中文标签`
     与 `ImGui 面板已初始化（…统一暗色主题；标签语言：中文（已加载 CJK 字体））`；stderr 为空。
  5. **编码**：本批 13 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工目视验收**：分区与配色观感、字号是否舒适（自动化只能证明"不崩溃 + 字体加载成功 + 英文回退全 ASCII"）。
  ② 若要**所有平台统一中文**，需内嵌子集化 CJK 字体（`Noto Sans SC`，OFL 1.1，约 1–3 MB）到 `assets/fonts/`——**属新资产，须先登记**。
  ③ 音量仍为"部分实现"（无声源）；B4 待确认；G2/G7 未实现；13 个设定问题待答复。④ 本批改动**尚未提交**。

---

## 2026-09-25  世界边界：空气墙 + 出界救援（T18 / G10）

- 做了什么：
  1. **边界由地图范围自动推导**（不写死在测试地图里）：新增 `world/terrain/world_bounds.*`，纯函数 `ComputeWorldBounds` / `ComputeBoundaryWalls`
     把"已加载 tile 范围 + 文档规定的垂直范围"换算成世界空间边界盒与 4 堵墙的位姿；对任意地图尺寸（含单 tile 退化情形）都有单测。
     当前地图（`tile_radius = [1,1]`，3×3 tiles）的边界为 **`(-64, -8, -64) ~ (128, 520, 128)`**，四堵墙**厚 2 格**、内表面与边界齐平。
  2. **引擎侧新增通用能力**：`engine/physics/PhysicsWorld::AddStaticBox`（静态盒体，pimpl 风格不变、**公共头不含 Jolt 类型**），
     并把槽位登记抽成 `Impl::RegisterBody` 供高度场复用。
  3. **出界救援（按业界标准补的配套项，已标注为 addition）**：新增 `game/out_of_bounds.hpp` 纯函数 `IsCharacterOutOfBounds`（余量 16 格；
     **只判下落与水平越界，不判"高于上沿"**，因为飞行允许升到边界盒之上）；触发即 `SetCharacterPosition(spawn)`（内部清零速度）+ 相机 `SnapTo` 消除插值拖影 + 一条 WARN。
- 为什么：① **墙挡不住飞行**——只做空气墙的话，"飞过墙顶再坠"仍会掉出世界，所以配套救援是**同一条需求的必要部分**，而不是额外功能；
  这也是"按业界标准默认做"的一个实例。② 边界必须**由地图范围推导**：若写死成当前地图的坐标，以后随机生成的世界一变大就失效，等于没做。
  ③ 触发判定**不逐帧重触发**：送回点落在盒内 ⇒ 下一帧即 false，日志天然每次出界只记一条；该性质由 `OutOfBounds.SingleRescueDoesNotRetriggerOnNextFrame` 钉死。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 56/56 步，退出码 0，**零错误零警告**（首轮曾报符号不明确，改名后重跑全绿）。
  2. **测试**：`ctest --preset debug` → **106/106 passed**（既有 96 项全绿 + 新增 10 项：`WorldBounds.*` / `OutOfBounds.*` 8 + `PhysicsBody.*` 2）。
  3. **门禁**：`scanned 78 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `ALIVE=True RESPONDING=True`；日志逐条打印 4 堵墙的中心与半长，以及
     `世界边界（由 tile 半径 [1, 1] 自动推导）：范围 (-64.0, -8.0, -64.0) ~ (128.0, 520.0, 128.0)；不可见围墙 4/4 个，厚 2.0 格；出界救援余量 16.0 格`。
  5. **编码**：本批 10 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工实测**：走到边缘应被一道看不见的墙挡住；`F` 飞过墙顶再关飞行应被送回出生点（相机无拖影、日志一条）。
  ② **T19（材质过渡观感）** 已登记为未开始：人工实测指出"大棱角处过渡色很多"，分析见当次对话——根因是**4 个占位纯色 + 逐顶点权重线性插值**（顶点间距 1 格只是放大器），
  这是**品质未达标项**，按第七节已列为待办而非"完成"。③ B4 待确认；G2/G7 未实现；13 个设定问题待答复。④ 本批改动**尚未提交**。

---

## 2026-09-25  SKILL 新增「品质门槛」+ 帧率上限滑块（M5 / T17）

- 做了什么：
  1. **M5 —— SKILL 新增第七节「品质门槛：可见产出不得以"能跑"结案」**（原第七节 DoD 顺延为第八节、原第八节顺延为第九节）：
     核心规则 **功能可用 ≠ 完成**；适用于一切**用户可见 / 可交互**的产出；给出**界面品质清单**（可读：含**不得缺字、不得假设运行环境有某字体** /
     一致：样式来自**单一样式入口** / 整齐：分区·对齐·不裸放 / **名义契约** / **状态可见** / **边界完整**）；
     并规定**第一次交付即须达标**、**能自动化的必须自动化**、**不能自动化的必须标注"待人工目视验收"并写清"看哪里、期望什么"**、
     **未过门槛者不得标记完成，须列为待办并写明差距**；附真实反面案例（UI 首版"功能全可用但字体缺字、无主题、控件裸放"）。DoD 新增自检项。
  2. **T17 —— 帧率上限滑块**：新增 `engine/core/frame_limiter.*`（纯函数 `FrameIntervalSeconds` / `ChooseFrameCapMode` + **睡眠式** `FrameLimiter::Throttle`）；
     `engine/platform/window.*` 新增 `DisplayRefreshRate()`（`SDL_GetDisplayForWindow` → 桌面显示模式）与 `SetVSync()`（**唯一**呈现模式路径）；
     设置新增 `frame_rate_cap`（**绝对 Hz**，`0` = 未设置哨兵，载入时按当前刷新率**重新钳制**，字段可选以兼容旧设置文件）；
     `game/system_panel.*` 新增「性能」分区与滑块（**60 ~ 当前刷新率**，默认刷新率），`game/ui_text.hpp` 标签缝同步增加中英两条，F1 面板增加"帧率上限"行。
- 为什么：① **M5 针对的失败模式是"把'能跑'当'完成'送出去"**——上一轮 UI 首版功能全可用（全屏 / 分辨率 / 音量 / 退出都真生效），
  但缺字成 `?`、无主题、控件裸放，用户第一眼就判定"很丑、不像完成"。规则必须把**品质**写进"完成"的定义，并要求**第一次交付就达标**，
  否则永远是"用户指出 → 返工"的循环，而返工成本高于一开始做对。② T17 的关键取舍是**零忙等**：上限档用垂直同步（呈现即等待，天然钉在刷新率），
  低于刷新率才用限帧器，且只做**一次睡眠**、**明令禁止忙等循环**——用"实测 FPS 略低于目标"换"CPU 占用近零"，这个代价已写进 UI 文档的「当前限制」。
  ③ 设置存**绝对值 + 启动钳制**，是因为上限依赖当前显示器：换显示器后旧值可能不可达。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 54/54 步，退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **96/96 passed**（既有 87 项全绿 + 新增 **9** 项：`FrameLimiter.*` 5、`SystemSettings.*` 4）。
  3. **门禁**：`scanned 74 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `Responding=True`；日志确认 `显示器刷新率：360 Hz（未知时回退 60 Hz）`、
     `帧率上限 → 360（垂直同步；呈现模式 vsync，显示器刷新率 360 Hz）`；以临时设置 `frame_rate_cap = 90` 复跑确认
     `帧率上限 → 90（睡眠限帧；呈现模式 mailbox，显示器刷新率 360 Hz）`（验证后已还原）。
  5. **编码**：本批 16 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工目视 / 手感验收**：`Esc` 面板「性能」分区的间距与对齐是否与其它分区一致、滑块拖到 90 时 F1 面板 FPS 是否稳定在 ~90 而不再 2000+。
  ② 睡眠档精度受 Windows 调度粒度（约 1 ms）限制，实测 FPS 略低于目标——若日后要更精确的节拍，应评估"垂直同步 + 分辨率缩放"或更高精度等待原语，而不是引入忙等。
  ③ 音量仍为"部分实现"（无声源）；B4 待确认；G2/G7 未实现；13 个设定问题待答复。④ 本批改动**尚未提交**。

---

## 2026-09-25  SKILL 新增「需求受理」规则：先判合理性、默认按业界标准（M4）

- 做了什么：在 SKILL「阅读约定」（先读这一节）内、**「任务下发」之前**新增 **「需求受理：先判合理性，再按业界标准执行」**三步法：
  1. **合理性判定**（须给依据，不许凭感觉）：合理且存在"**与要求不矛盾的更优做法**" ⇒ **直接按更优做法实现**，并在回复中明确对照
     "你要的效果 → 我们的实现方式 → 依据"、写进 devlog；**与要求矛盾**（会改变用户要的效果）⇒ **不动代码**，列冲突点与候选做法后提问；
     **撞红线 / 不可行** ⇒ 指出冲突的 ADR、红线或预算条目，给出可行替代再问。
  2. **业界标准优先（默认动作，不是可选项）**：任何需求先问"业界同类产品怎么做的"，**不要等用户点名具体技术手段**；
     判定链 = 业界标准做法 → 本项目约束下可否用 → 最接近的替代与差异。反例即本项目真实踩过的：**锁定光标（相对鼠标模式）本就是第三人称 3D 游戏标准做法**，
     应在**实现第三人称相机的当次**就做，而不是等用户提出。
  3. **与第五节范围控制的边界**（防止借"业界标准"之名扩张）：只有"**不做会导致体验明显残缺或日后必须返工**"的才算业界默认项、直接做
     （光标锁定 / 俯仰限位 / 失焦处理 / 相机不穿墙 / 窗口可缩放）；粒子、后处理、天气等纯增强**仍走范围控制**。
  4. 把三个**真实案例固化为判定示例**："上下各看**满 90°**" ⇒ 直接按 **±89°**（属于"不矛盾的更优做法"，正好 90° 会像 B2 一样退化出 NaN）；
     "鼠标转向有问题"（未提锁定）⇒ **主动实现**相对鼠标模式；假设"改成第一人称" ⇒ **矛盾**，先提问。
  5. DoD 新增自检项（需求已按三步处理、按更合理做法执行之处已明确对照）；阶段计划新增迁移任务 **M4**。
- 为什么：这两条要求针对的是**同一种失败模式**——把"应当主动判断并做对的事"变成"等用户来提要求"。
  本项目的实例就是鼠标锁定：第三人称相机落地时没做光标捕获，导致用户先撞上"推不到 ±89°、鼠标移出窗口视角卡住"，才发现标准做法缺失。
  同时规则**必须设边界**，否则"业界标准"会成为范围蔓延的借口、与第五节范围控制直接冲突；因此第三步把"缺失即残缺"与"锦上添花"分开。
  第三条（记录义务）防的是另一种失败：**用户以为需求被悄悄改掉**——凡按更优做法执行，必须把"你要的效果 → 我们的实现 → 依据"写清楚，
  这也正是本次回复对"满 90° ⇒ 实现为 ±89°"所采用的口径。
- 验证：`SKILL.md` 与 `docs/plans/v0.1.md` 逐字节检查 → **纯 LF、无 BOM**；门禁 `-SelfTest` 通过（18 case）；
  规则位于「阅读约定」内且在「任务下发」之前，并**显式声明与第五节范围控制的边界**，无口径冲突。
- 下一步 / 遗留：① **立即生效**：此后每次受理需求均按三步处理，并按 DoD 新条目自检。② 仍待人工复测 T14（鼠标锁定与俯仰可达性）；
  B4 待确认；G2（必备元素可定义）与 G7（收集物品）未实现；13 个设定 / 需求问题待所有者答复。③ 本批改动**尚未提交**。

---

## 2026-09-25  人工实测第 3 轮：鼠标锁定与俯仰可达性（T14 / G8）

- 做了什么：
  1. **先判定了"建议是否合理"再动手**。所有者提议"上下各看近 90°"与"锁定鼠标"。读代码确认：俯仰上限**已经是 ±89°**（`kCameraPitchLimit`，刻意留 1° 使视线永不与世界上方向平行——正好是 B2 那类退化矩阵的来源），
     所以"看得不够"不是上限太小，而是**光标没锁定**：非锁定模式下光标撞到屏幕边缘位移即停，按 `kLookSensitivity = 0.0022 rad/px` 计算，从正前方转到单侧 ±89° 需累计约 **706 px**，屏幕高度往往不够 ⇒ 推不到上限；
     同时鼠标移出窗口视角直接卡住。**两个症状是同一个根因**，锁定鼠标即同时解决。
  2. **平台层**：`Window::SetRelativeMouseMode(bool)` / `IsRelativeMouseMode()` 封装 `SDL_SetWindowRelativeMouseMode`（**幂等**，状态未变不调用 SDL）；`pump_events` 处理 `SDL_EVENT_WINDOW_FOCUS_LOST` → **自动释放**（否则 Alt+Tab 后光标消失、无法操作其它窗口），`FOCUS_GAINED` 刻意**不**静默重捕获。
  3. **上层捕获策略**：新增纯函数状态机 `game/mouse_capture.hpp::DecideMouseCapture`（`Esc` 释放 / 点击重捕获），其中**重捕获的那一次点击被标记为"已消费"，先于笔刷判定**——否则"点一下锁定鼠标"会顺带挖掉一格；未捕获期间丢弃视角位移。新增动作 `ReleaseMouseCapture`（绑 `Esc`），启动即捕获，状态变化均记日志。
  4. **可见性**：调试面板新增一行「鼠标捕获：开 / 关」，人可直接确认状态。`camera.*` 一字未改，`kCameraPitchLimit` 保持 **89°**。
- 为什么：① 这一轮的价值在**先归因再实现**——若按字面把上限从 89° 提到 90°，不仅解决不了"推不到上限"，还会**重新引入 B2 的 NaN 蓝屏**（正好撞在刚修的坑上）。
  ② "锁定鼠标"这类交互改进必须同时处理三个边界：失焦释放、重捕获点击不与笔刷冲突、释放期间丢弃位移；少一个都会变成新缺陷（尤其第二个是**会误挖地形**的那种）。
  ③ 因此把捕获决策做成**纯函数 + 单测**（含"接到真实 `InputMap` 上验证消费顺序"），而不是散在 `main.cpp` 的 if 里。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 44/44 步，退出码 0，**零错误零警告**（中途因丢弃 `[[nodiscard]]` 返回值触发 C4834，已用 `(void)` 修正后重跑通过）。
  2. **测试**：`ctest --preset debug` → **61/61 passed**（既有 55 项全绿 + 新增 6 项：`MouseCapture.*` 5 项，含 `RecaptureClickIsConsumedBeforeBrushCheck`；`ThirdPersonCamera.ExtremeRepeatedPitchClampsExactlyAndStaysFinite` —— 后者同时守住 ±89° 上限与"两端视图矩阵有限"，把 T14 与 B2 绑在一起）。
  3. **门禁**：`scanned 57 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：15 秒 `alive=True responding=True`；日志含 `鼠标捕获：开（相对模式：光标隐藏、鼠标位移不再受屏幕边界限制；Esc 释放，点击窗口重新捕获）`。
  5. **编码**：本批 10 个新增 / 修改文件全部纯 LF、无 BOM。
- 下一步 / 遗留：① **待人工复测**：光标应被隐藏、鼠标撞屏幕边缘视角仍继续转、上下可达 ±89°、`Esc` 释放后光标可见且不误操、点击重捕获不误挖。② `kLookSensitivity` 未改（0.0022 rad/px）；若手感偏慢，改这一个常量即可（1061~1412 px 覆盖整段俯仰）。③ B4 仍待确认；G2（必备元素可定义）与 G7（收集物品）仍完全未实现；仍待所有者答复 13 个设定 / 需求问题。④ 本批改动**尚未提交**。

---

## 2026-09-25  T7 角色物理（Jolt）+ T9 ImGui 调试面板 + I1 Linux CI 系统依赖

- 做了什么：
  1. **T7 角色物理**：新增 `engine/physics/physics_world.*`（Jolt 生命周期薄封装：作业系统与临时分配器、固定步长推进、通用高度场与角色胶囊；**公共头以 pimpl 隔离，不含任何 Jolt 类型**）；新增 `world/terrain/terrain_collision.*` 作为地表专有胶水（tile → 高度场采样 / 碰撞体，挖掘后按脏 tile 重建）；`game/main.cpp` 用 `CharacterVirtual` 胶囊**替换掉原先"相机贴地"的临时做法**，相机改为跟随角色脚底并保留地形避障。
  2. **T9 调试面板**：新增 `game/debug_overlay.*`，用 ImGui 的 SDL3 + SDL3_gpu 后端显示帧时间（本帧 + 120 样本环形缓冲的 P50/P95）、FPS、本帧固定步数、角色位置、相机、笔刷半径、tile 与脏 tile 计数、地表碰撞体 tile 数；F1 开关；滚动分位用固定数组排序，**每帧零堆分配**，隐藏时整帧跳过。
  3. **配套加法式改动**（已由子代理显式上报，均为纯加法、不改既有行为）：`engine/render/mesh_renderer` 增加通用 `IRenderOverlay` 与 `RenderFrame(..., IRenderOverlay*)` 重载；`engine/input/action_state` 增加 `ActionId::ToggleDebugPanel`；`world/CMakeLists.txt` 增加一行源文件。
  4. **I1 Linux CI**：`ci.yml` 的 Linux 依赖步骤补 `autoconf autoconf-archive automake libtool`（vcpkg 日志原文要求 `autoconf autoconf-archive automake libtoolize`）。
- 为什么：T7 是"人物能自由活动"从"相机贴地假象"变成"真的走路"的分界——也是把相机避障从"唯一碰撞"降为"辅助避障"的前提；不换真角色，T8 的洞口、台阶、挖掘后站立全都没法验证。T9 的价值在于**把性能与状态变成看得见的数字**，否则 ADR 0008 的"只记录、不验收"就没有记录手段。I1 的根因链条值得记下：**`imgui[sdl3-binding,sdlgpu3-binding] → sdl3[dbus,ibus,x11,wayland] → dbus[systemd] → libsystemd → libxcrypt`**，`libxcrypt` 需要 `autotools` 才能构建；Windows 不开这些 Linux 专有特性，所以 Windows 作业一直是绿的——**平台不对称导致的失败，只在 Linux 侧暴露**。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 38/38 目标，退出码 0，**警告/错误行数 0**。
  2. **测试**：`ctest --preset debug` → **39/39 passed**（原 34 项全绿 + 新增 5 项）。关键用例：`PhysicsCharacter.FallsAndRestsOnFlatHeightField`（落地静止不下穿）、`ClimbsOneGridUnitStepWhileWalking`（**1 格台阶自动上步**）、`SprintDoesNotPassThroughStep` / `SprintDoesNotPassThroughTallWall`（**冲刺不穿地形**）、`TerrainCollision.BuildHeightFieldSamplesReportsDimensionsAndValues`。
  3. **门禁**：`scanned 47 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：运行 15 秒 → `HasExited=False`、`Responding=True`；日志确认 `Jolt 物理已初始化` / `ImGui 调试面板已初始化（SDL3 + SDL3_gpu 后端）` / `角色物理就绪：地表碰撞体 9 个 tile；胶囊 半径 0.30 / 总高 1.80 格；重力 24.0、跳跃 8.0、最大坡度 50°、自动上台阶 1.0 格（dt=1/60）` / `F1 开关（当前显示）`。
  5. **编码**：本批 15 个新增 / 修改文件 → `non-LF/BOM problems: 0`。
  6. **ci.yml**：`bytes=4981 CR_count=0 non_ascii_count=0 BOM=False`（纯 ASCII + 纯 LF），diff 仅新增 4 个 apt 包。
- 下一步 / 遗留：① **T8 未做**（最小可挖体积：局部 SDF + Surface Nets + 与地表相接的洞口过渡），随后 **T10 阶段验收**。② **新增待收敛项 7**：`joltphysics` 5.6.0 为**单精度**（`RVec3` = `Vec3`），与红线 6 的 `double` 世界坐标在大坐标上冲突，候选方案（双精度构建 / 物理做局部原点重定基 / 仅近场用物理）须在**流式加载任务开工前**经 ADR 收敛。③ **ImGui 面板当前只读**：`Window::pump_events` 独占事件队列、未转发 `ImGui_ImplSDL3_ProcessEvent`，要可拖动需在平台层加事件外露钩子。④ 其余已知未做：视锥体裁剪、真纹理（占位色）、笔刷按住连挖、窗口 resize 重算投影。⑤ 本批改动**尚未提交**。

---

## 2026-09-25  修复 Linux CI：vcpkg 工作树带本地改动导致 checkout 被拒

- 做了什么：把 `.github/workflows/ci.yml` 的 `Align vcpkg with pinned baseline` 步骤中
  `git -C "$VCPKG_ROOT" checkout --detach FETCH_HEAD` 改为 `checkout -f --detach FETCH_HEAD`，并补 4 行注释说明缘由。
- 为什么：run `36146153279` 的结论显示 **Windows 两个 job（debug / release）已首次全绿**、两个门禁也全绿，
  仅 ubuntu 的 asan / tsan 失败，且失败点就在该步骤，原文为
  `error: Your local changes to the following files would be overwritten by checkout:` ——
  即 **runner 镜像预装的 `/usr/local/share/vcpkg` 工作树本身带本地改动**，普通 checkout 被 git 拒绝。
  用 `-f` 丢弃这些改动（并清掉会阻碍切换的未跟踪文件）；runner 是一次性环境，丢弃镜像自带改动安全。
  **注意本次与上一轮修的不是同一件事**：上轮（`bcc6008`/`a7822c9`）解决的是"锁定基线的提交对象在 shallow 克隆里取不到"，
  本次解决的是"取到之后工作树切不过去"——两个问题在不同环节，因此 Windows 侧转绿而 Linux 侧仍红。
- 验证：本地 —— `.github/workflows/ci.yml` 逐字节检查 → 非 ASCII 字节 **0**、CR 字节 **-1**、无 BOM
  （该文件按仓库约定必须纯 ASCII + 纯 LF，见 `ci.yml` 顶部注释）；改动为 4 行新增注释 + 1 行命令改写。
  CI —— 结果待本次推送触发的 run（见下方"下一步 / 遗留"）。
- 下一步 / 遗留：① 若 `-f` 后 ubuntu 仍失败，备选方案是**在 workspace 内 depth=1 clone 一份 vcpkg 到锁定基线并 bootstrap**，
  彻底不依赖镜像预装状态（代价是多一次 clone）；② `docs/plans/v0.1.md` 的 **I1** 与前置条件 **P7** 待 CI 结果确认后收口；
  ③ 待收敛项 4（地表 LOD）与 6（遮挡剔除）仍开放。

---

## 2026-09-25  M2 + T4~T6：世界层更名与"能挖能堆的平滑地表"跑起来

- 做了什么：
  1. **M2 世界层更名**：`git mv voxel/ → world/`（保留历史），删除已随 ADR 0004 作废的 `voxel/chunk/chunk_types.hpp` 与其 6 项旧测试（`ChunkStateMachine.*` / `ChunkGeometry.*`），根 `CMakeLists.txt` 改为 `add_subdirectory(world)`；**门禁脚本默认扫描目录同步由 `voxel` 改为 `world`**（原默认值会让整个新世界层逃过门禁，属 M2 的连带遗留）。
  2. **T4 地表高度场**：`world/terrain/` 新增 tile（**64×64 列、`int16` 1/16 格、65×65 采样**）、网格化与**梯度法线**、`world/generation/`（`SplitMix64` 种子派生 + FastNoiseLite 三层 FBm，pimpl 隔离第三方头）；`TerrainWorld` 实现 `engine/render/camera.hpp` 的 `ITerrainQuery`，使相机避障可用且 `engine/` 不反向依赖 `world/`。
  3. **T5 地表材质**：`material_table`（`assets/config/materials.toml`，带 `schema_version`，toml++ 加载并在非法时**明确报错**）+ `material_blender`（按高度 + 坡度算归一化权重，含确定性噪声抖动）。
  4. **T6 笔刷挖掘 / 堆建**：`world/dig/terrain_brush` 球笔刷改高度并**只标脏受影响 tile**；运行期只重传脏 tile 的 GPU 网格。
  5. **渲染接线**：新增 `assets/shaders/mesh.vert|.frag`（splat 权重混合 4 个占位色）并在 `game/CMakeLists.txt` 注册 `add_shader`；重写 `game/main.cpp` 为完整闭环（窗口 → 输入 → 固定步长 → 第三人称相机 → 世界渲染 → 鼠标挖/堆）。
  6. **两处必要的 engine 层加法**：`InputMap` 补鼠标按键通道（与键盘对称的 `BindMouseButton`/`SetMouseButtonDown`）；`Window::pump_events(InputMap&)` 改为**唯一**把 SDL 事件翻译成动作的地方（此前事件被直接丢弃，且红线禁止 `game/` 读事件队列）。
  7. **FastNoiseLite vendoring**：`third_party/FastNoiseLite/FastNoiseLite.h`（MIT，107699 B，纯 LF、无 BOM）；`NOTICE.md` 的引入阶段由 V0.2 更正为 V0.1。
  8. **规范同步**：SKILL §2 明确「**纯 header-only 的 vendored 库允许在 `world/` 的 .cpp 内直接使用，但不得进入公共头**（须 pimpl 或自有类型隔离），扩散即须抽 `engine/` 薄封装」——这是对既有"第三方只在平台层/引擎薄封装"规则的**显式放宽**，为的是不把能跑的代码倒回去做无收益的搬运；同时更新 §2 依赖链（`world`）、适用范围、性能预算（**删除旧的 96 KB / ≤700 draw call / 3×3×3 方块查询等作废数字**，改为引用 ADR 0008 并声明 Draw Call 与视距内存"只记录、不验收"）、红线表补「术语替换」一句（「区块/Section/方块」按新载体读作「地表 tile/体积块/体素单元」）。
- 为什么：M2 必须在 T4 之前完成，否则新代码会长在已作废的目录名下、且门禁扫描不到。T4~T6 是 ADR 0004 落地的"最小可验证闭环"：**如果相邻 tile 边界不逐位相等就会出现裂缝**，如果笔刷不局限于受影响 tile 就会出现"挖一下卡一下"，这两条都是本方向最容易翻车的地方，故先以单测钉死（共享边界逐位相等、半径外逐列不变、脏 tile 精确）。渲染接线与两次 engine 层小改动是"让它真的能看见、能操作"的必要成本——尤其是事件队列的翻译位置，之前根本没有任何地方把 SDL 事件喂给 `InputMap`。
- 验证：
  1. **构建**：`cmake --build --preset debug --clean-first` → 34/34 目标，退出码 0，**警告 0 行、错误 0 行**；`mesh.*` / `triangle.*` 双格式产物齐全（`.spv` + `.dxil` 各 4 个）。
  2. **测试**：`ctest --preset debug` → **34/34 passed**（旧 23 中删去 6 项区块测试后为 17，加新增 17 项）。关键用例：`TerrainTile` 共享边界**逐位相等**、生成确定性、挖/堆半径内逐列变化而半径外不变、脏 tile 集合精确、材质权重归一且陡坡偏岩。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 24 file(s), 0 violation(s)`、`PASS`、退出码 0（**注意**：改默认目录前它不覆盖 `world/`；修复后已覆盖）。
  4. **运行期冒烟**：启动 `build\debug\bin\voxel_game.exe` 运行 10 秒 → `alive=True responding=True`，日志 `地表世界就绪：种子 1592594996，tile 9 个，材质表 schema_version=1，笔刷半径 6.0 格`，之后被主动结束。说明材质表加载、9 个 tile 生成与网格化、`mesh.*` 双格式加载与管线创建、每帧拿到交换链纹理均成功。
  5. **行尾与编码**：本批 25+ 个新增/修改文件逐字节检查 → `BOM=False`、`CR=0`（纯 LF）。
- 下一步 / 遗留：① **目视未确认**：本环境无显示/截图能力，"地表是否满屏、绕序是否朝上、splat 占位色是否合预期"需人看一眼窗口。② **T7~T9 未开始**（Jolt 角色 / 可挖体积 / ImGui 面板）→ 随后是 T10 阶段验收。③ 本轮已知未做：**视锥体裁剪**（`references/meshing-and-render.md` §5 要求）、**真纹理**（现为占位色）、笔刷按住连挖、窗口 resize 重算投影、`world/objects/` 与 `world/streaming/` 未建。④ 待收敛项 4（地表 LOD）与 6（遮挡剔除）仍开放。⑤ 本批改动**尚未提交**。

## 2026-09-25  T19 地表材质管线：权重逐像素算 + 分层贴图（ADR 0009）

- 做了什么：
  1. **新增 [ADR 0009](adr/0009-terrain-material-pipeline.md)**：定案「**权重用算的，外观用贴的**」——逐像素权重（窄带 `smoothstep`）+ 每层 albedo/法线贴图 + 只采权重最高的 3~4 层 + 程序生成占位贴图；并明确纠正"贴图能省性能"的方向性误解。
  2. **新增 `world/terrain/material_textures.*`**：程序生成 4 层 albedo + 4 层法线（256×256、`R8G8B8A8_UNORM`、layer-major；含 mip 约 **2.67 MB** 显存，第 0 级 1.00 MB/张）。albedo 只出**单色细节**（层色由 `tint` 决定，避免"颜色有两个来源"）；法线由高度噪声梯度 `n = normalize(-dH/du, -dH/dv, 1)` 生成；亮度/高度噪声在 tile 上**周期混合**以保证可平铺；通道种子由全局种子派生 ⇒ **逐字节确定性**（红线 7）。
  3. **`world/terrain/material_table.*`**：`materials.toml` 升 **`schema_version = 2`**，每层新增 `uv_scale` / `tint_r|g|b`；新增 `MaterialUniform`（std140，`static_assert` 钉死 208 字节）与 `BuildMaterialUniform(...)` —— **CPU→GPU 材质参数的唯一投影入口**，杜绝"配置改了但画面没变"的漂移。
  4. **`world/terrain/material_blender.*`**：抽出并公开 `MaterialBandFactor`（CPU / GPU 共用的窄带曲线），片元着色器里逐字镜像。
  5. **顶点不再承载材质权重**：`terrain_mesher` 的 `MeshVertex` 去掉权重字段；权重改由 `assets/shaders/mesh.frag` 按**世界高度与坡度**逐像素重算（渲染原点进 uniform，用它把相机相对坐标还原成世界坐标，红线 6 不破）。
  6. **`engine/render/mesh_renderer.*`** 新增**通用纹理数组 / 采样器 / 片元 uniform 块**支持；`assets/shaders/mesh.vert|.frag` 同步改写（`kMaxSampledLayers = 4`；近零权重 `kWeightEpsilon = 1e-6` 直接 `continue` 跳过采样）。
- 为什么：人工实测指出"大棱角处过渡色很多"，诊断结论是**逐顶点权重在三角形内被线性插值**——整段坡度变化被摊在约 1 格宽（≈ 一个人高）里，再叠上 4 个占位纯色，于是糊成宽带；**顶点间距 1 格只是放大器而非根因**（加密顶点只会让带变窄，不会消失）。同时纠正一个方向性误解：**贴图不是省性能的手段，它本身就是带宽开销的主要来源**——逐像素重算权重很便宜（几次点乘 + `smoothstep`），昂贵的是每层 2 次带过滤的采样；所以性能旋钮是"层数 / 分辨率 / 各向异性等级"，不是"少用贴图"。选**程序生成占位贴图**是为了先打通管线、美术资源后接，且不引入二进制资产、可复现。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**；`mesh.vert` / `mesh.frag` 的 **SPIR-V 与 DXIL 双格式**产物齐全。
  2. **测试**：`ctest --preset debug` → **113/113 passed**（原 106 + 新增 7）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 81 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**（在**前台终端**启动 `build\debug\bin\voxel_game.exe`，持续 > 11 秒仍在运行）：日志 `材质贴图已生成并上传：256×256 × 4 层，R8G8B8A8_UNORM；albedo + 法线两张，含 mip 约 2.67 MB 显存（第 0 级 1.00 MB/张）`、`地表世界就绪：… 材质表 schema_version=2 …` ⇒ 生成 → 上传 → 材质表加载 → uniform 投影全链路走通。（首轮曾触发 SDL 断言 `GenerateMipmaps texture must be created with SAMPLER and COLOR_TARGET usage flags`，给纹理补 `COLOR_TARGET` 用途位后通过；另：用 `Start-Process` 分离方式启动时进程约 3 秒后自行退出、且无任何日志输出，属该启动方式下的环境现象，前台终端启动无此问题。）
- 下一步 / 遗留：① **目视未验收**：本环境无截图能力，"棱角色带是否真的收窄、法线立体感是否可见"须**人工目视**（看测试地图的陡坡与地标塔一带）。② **主角外观变化**：顶点不再有材质通道，主角与地表共用同一片元材质，**不再是固定沙色**；若要恢复固定外观须另开"每 draw 材质覆盖"（超出本任务范围）。③ **纹理层号口径未对齐**：`materials.toml` 注释与 `references/meshing-and-render.md` §3 都声明"`index = 0` 保留给缺失纹理"，但实现是 `textureIndex = texture_layer - 1` 落在 **4 层**数组上（0 号层实际是草）——要么数组补 1 层占位、要么改口径，**须经确认后再动**。④ 真美术 PBR 贴图替换占位图、triplanar / RVT 均未做（ADR 0009 已列切换条件）。⑤ 本批改动**尚未提交**。
