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

## 2026-09-26  渲染质量线立项 + P0 落地：HDR / 色调映射 / sRGB 编码；F1 面板补性能开销

- 做了什么：
  1. **新增 [ADR 0010](adr/0010-render-quality-pipeline.md)**：人工反馈"地表材质像 20 年前的游戏"，诊断后定案「**HDR + 色调映射 → 光照与阴影 → PBR → MSAA 与细节**」四阶段质量线（P0~P3），并明确**顺序不得跳步**。该线属 SKILL 第五节"纯增强"范畴，**由项目所有者于本日明确追加授权**，已在 ADR、阶段计划与 `docs/game-design.md`（G11）三处留档。
  2. **T24 · F1 面板性能开销数据**：`engine/render/mesh_renderer.hpp` 新增通用 `RenderStats`（`drawCalls` / `triangleCount` / `vertexCount` / `textureBytes`）与纯函数 `EstimateTextureArrayBytes`（含完整 mip 链的 4/3 估算），显存在纹理 / 目标创建与释放时增减；`game/debug_overlay.*` 与 `game/ui_text.hpp` 新增展示行（14 个中英标签，仍全部经标签缝）；`game/main.cpp` 用核心单调时钟分相测**逻辑步 / UI 构建 / 渲染提交**三段 CPU 耗时。**GPU pass 时间不做假**：SDL3_gpu 无时间戳查询 API，面板显式显示"不可用（SDL3_gpu 无时间戳查询）"。
  3. **T20 · P0 HDR 管线**：主通道颜色目标由交换链格式改为 **`R16G16B16A16_FLOAT` 离屏目标**（`COLOR_TARGET | SAMPLER`，随窗口尺寸重建，深度目标沿用）；新增全屏三角形 `assets/shaders/tonemap.vert|.frag`（**曝光 → ACES 近似（Narkowicz）→ 分段精确 sRGB 编码**）写入交换链；`assets/shaders/mesh.frag` 改为**线性空间**着色（albedo 与 `tint` 均转线性、光照常量从 sRGB 口径的 `0.35 + 0.65·diffuse` 改为线性口径的 `0.10 + 1.00·diffuse`）；曝光进 `engine/platform/settings.*`（**可选字段**，缺失取默认 1.0、类型错误报错、越界钳制 `[0.1, 8.0]`，`schema_version` 保持 1 以免破坏既有设置文件）；`game/CMakeLists.txt` 注册 `add_shader`。**叠加层（ImGui）仍在色调映射之后写交换链**，面板不被色调映射处理。
  4. **文档同步**（按 SKILL 同步清单）：ADR 索引、`tech-plan-v2.0.md` §4.3/§4.4/§4.5/§7.2、`references/meshing-and-render.md` §3/§4、SKILL 唯一口径表"材质与光照"行、`engine-capabilities.md`（先登记后实现）、`ui-inventory.md`（§2.2 展示项 + §3 设置项）、`game-design.md` G11、阶段计划 T20~T24；并**新增待收敛项 10**（质量线的显存与带宽重算）。
- 为什么：诊断表明差距主体**不在贴图分辨率，而在光照、色彩与抗锯齿**——全仓库检索 `sRGB` / `gamma` / `tonemap` / `exposure` 曾**零命中**，即完全没有 HDR 与色调映射环节，亮部一过 1.0 就死白截断、暗部没有环境色；叠加只有 Lambert 漫反射、单频程序贴图与 `sample_count = 1`（无抗锯齿）。因此**先把 P0 做掉**：没有 HDR 与色调映射，后续阴影与 PBR 的高光收益会被亮部截断直接吃掉。同时按 ADR 0010 的要求**先补可观测性（T24）**——每一步都在"变好看"的同时增加开销，没有开销数字就无法判断收益与代价，也无法履行 §7.2 的预算记账义务。**明确否定一条错路**：单纯加密网格（1 格 → 1/4 格）只会把色带变窄并抬高开销，不会产生"圆润"。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + 警告即错误）。
  2. **双格式 Shader 产物**：`build\debug\assets\shaders\` 下 `mesh.*`（4）、`tonemap.vert/.frag`（4）、`triangle.*`（4）齐全 —— 新增的 tonemap **SPIR-V 与 DXIL 都有**（ADR 0002）。
  3. **测试**：`ctest --preset debug` → **116/116 passed**（原 113 + 新增 3：`RenderStats.MipChainEstimateFollowsFourThirdsRule`、`SystemSettings.ClampExposurePureFunction`、`SystemSettings.ExposureClampedOnLoadAndRoundTrips`）。
  4. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 82 file(s), 0 violation(s)`、`PASS`、退出码 0。
  5. **运行期冒烟**：前台启动 `build\debug\bin\voxel_game.exe`，持续 **> 12 秒仍在运行**；日志 `已加载设置：… 帧率上限=360 Hz，曝光=1.00`、`材质贴图已生成并上传…`、`地表世界就绪：… 材质表 schema_version=2 …`；**无 SDL 断言、无 ERROR / WARN 行**（纹理用途位、采样器绑定、管线格式一次性通过）。
  6. 本批改动文件逐字节校验：**纯 LF、无 BOM**。
- 下一步 / 遗留：① **观感待人工目视验收**（SKILL 第七节）：本环境无截图能力，须人看**测试地图的陡坡与地标塔一带**——期望"无死白截断、暗部有层次、颜色不发灰"，以及 F1 面板的新数字（Draw Call 应约等于 tile 数、纹理显存应约 2.67 MB + HDR 目标 + 深度）。**色调映射落地后既有颜色常量需要重新校准**（`tint` / 清屏色 / UI 主题对比度），本轮只做了必要的线性化，**最终校准留到 P1 有天空光之后**。② **待收敛项 10 已转为"必须核算"**：P0 已落地，核清显存与带宽并回填方案 §7.2 前**不得开工 P1**。③ 后续阶段：**P1**（方向光 + CSM + 半球天空光 + 指数高度雾）→ **P2**（PBR 四件套 + 多尺度贴图）→ **P3**（MSAA + 多频细节法线）。④ 本轮已知未做：**光照参数仍未进配置文件**（方向光仍写死在 `mesh.frag`，P1 随 `assets/config/lighting.toml` 一并处理）；ESC 面板暂无曝光控件（已登记在 `ui-inventory.md` §3）。⑤ 本批改动**尚未提交**。

## 2026-09-26  P0 缺陷 B5：色调映射通道把画面上下翻转 —— 根因是 SDL_gpu 的两套 Y 方向

- 做了什么：修 `assets/shaders/tonemap.vert` 的全屏三角形 UV ⇒ `v_uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5)`；在**三处**留档防复发：着色器注释、`references/meshing-and-render.md` §4 新增硬规则（"全屏后处理通道的 UV 必须翻转 V"）、`docs/learning-notes.md` 新增「SDL_gpu 的坐标约定」条；阶段计划登记为 **B5**。
- 为什么：人工实测第 4 轮报"**视角不对，之前是对的**"。定位过程与结论：
  1. **先排除相机路径**：`git diff` 显示本轮 `game/main.cpp` 只加了相位计时器与清屏色线性化，**没有触碰**相机 / 投影 / 视口 / 输入的任何代码；`engine/render/camera.*` 本轮零改动。⇒ 不是"相机算错了"。
  2. 本轮唯一新增的**全屏几何**是 P0 的色调映射通道，其 `tonemap.vert` 的 `v_uv = position * 0.5 + 0.5` 是**凭直觉**写的（未查约定）。
  3. 查本机权威依据 —— `build/debug/vcpkg_installed/x64-windows/include/SDL3/SDL_gpu.h` §Coordinate System：
     **NDC** = "左下角 `(-1,-1)`、右上角 `(1,1)`"（**+Y 向上**）；**纹理坐标** = "左上角 `(0,0)`、右下角 `(1,1)`"（**+Y 向下**）；后端差异（如 Vulkan 的 NDC 是 +Y 向下）**由 SDL 自动转换**，明令不要自行翻转。
     ⇒ 屏幕**上方**（NDC `y=+1`）映射到 `v_uv.y = 1`，而 `v=1` 是图像的**底部** ⇒ **整帧垂直翻转**。
  4. **症状为何是"UI 正常、世界倒置"**：ImGui 叠加层是在色调映射**之后**、直接以交换链为目标的，不受该 UV 影响 —— 这正好解释了"面板看着正常、世界却不对"的不对称现象。
- 验证：`cmake --build --preset debug` → 退出码 0，`tonemap.vert` 重新产出 **SPIR-V 与 DXIL 两份**产物；前台启动冒烟 **> 9 秒**运行正常、无 SDL 断言、无 ERROR / WARN；`ctest --preset debug` → **116/116**（本缺陷属着色器 UV 约定，**无法用单测覆盖**，故改为"规则 + 注释"防复发，并把目视确认列为下一步）。
- 下一步 / 遗留：① **待人工目视确认**：世界不再上下倒置（看**天际线与地标塔/深坑的相对上下位置**：天空应在上方、坑应在下方）。② 本次教训已固化为 `references/meshing-and-render.md` §4 的硬规则（全屏后处理必须翻转 V），后续 P1 的阴影、P3 的 MSAA resolve 等任何新通道**都必须照此写**。③ B5 是 P0 的连带缺陷，**T20 的观感验收仍待人工完成**。④ 本批改动**尚未提交**。

## 2026-09-26  待收敛项 10 收敛：显存与带宽记账回填方案 §7.2

- 做了什么：
  1. **记账做成可观测**：`engine/render/mesh_renderer.cpp` 在 HDR 目标创建 / 分辨率变化时，把纹理显存**按项打进日志**（材质数组 / 深度目标 / HDR 目标 / 合计）。
  2. **回填方案 §7.2**：新增 **§7.2.1 显存记账**（当前各项 + P1 / P3 的增量预估 + 最坏合计）与 **§7.2.2 每帧带宽口径**（HDR / 深度 / 材质采样 / CSM 四路）；VRAM 的"记录方式"由 RenderDoc 改为"引擎侧记账 + RenderDoc 交叉核对"。
  3. **SKILL 待收敛项 10 → 已收敛**（指向 §7.2.1 / §7.2.2）；阶段计划同步；学习笔记新增「显存与带宽记账」条（含 mip 的 4/3、字节宽差异、"显存账 ≠ 带宽账"）。
- 为什么：ADR 0010 的记账义务要求"每一阶段落地都要单独记账"，P1 开工前必须先回答"这些新目标会不会挤爆 VRAM"。单纯凭公式估算无法核对，所以**先把记账做成运行期可打印的数字**，再据实回填预算表——否则 §7.2 只是另一份"应该够用"。
- 验证：
  1. **实测证据**：前台启动 `voxel_game.exe`，日志 `GPU 纹理显存记账：材质数组 2.67 MB + 深度目标 3.52 MB + HDR 目标 7.03 MB = 合计 13.21 MB（1280x720）` —— 与公式（4 B/px、8 B/px、mip 4/3）逐项吻合。
  2. **构建**：`cmake --build --preset debug` → 退出码 0，零错误零警告（新增 `core/log.hpp` 依赖与一条日志）。
  3. 1080p 同公式推算：深度 7.91 MB + HDR 15.82 MB + 材质 2.67 MB + 网格（估算 1.71 MB）≈ **28 MB**；加上 P1 阴影（≈48 MB）、P2 材质（≈5.6~22.4 MB）、P3 MSAA 4×（≈95 MB）后**最坏合计 ≈ 195 MB**，仍在 ≤300 MB 内但**余量不足一半**。
  4. **带宽结论（重要）**：HDR 通路每帧仅 ≈33 MB（约 2.0 GB/s @60FPS），而**材质采样最坏 ≈253 MB/帧（约 15 GB/s）为最大一笔** ⇒ 瓶颈在采样层数 / 分辨率，**不在渲染目标**——与 ADR 0009「性能第一旋钮 = 采样层数」的判断相互印证。
- 下一步 / 遗留：① **网格顶点 / 索引缓冲尚未接入运行期记账**（`RenderStats` 目前只记纹理），故预算表里"地形网格缓冲"一行是估算值；接入后并入同一张表（已在方案 §7.2.1 末尾登记）。② **P3 开工前必须定 MSAA 的颜色格式**（`RGBA16F` ≈95 MB vs `R11G11B10_FLOAT` ≈63 MB）。③ 下一步进入 **T21（P1）**，已拆为 T21a（光照配置 + 方向光 + 半球天空光）→ T21b（阴影通道 + CSM）→ T21c（指数高度雾）。④ 本批改动**尚未提交**。

## 2026-09-26  P1 前两步落地：光照配置化 + 方向光 + 半球天空光 + 指数高度雾（T21a / T21c）

- 做了什么：
  1. **新增 `assets/config/lighting.toml`**（`schema_version = 1`）：`[sun]` 方向 / 颜色 / 强度、`[sky]` 天顶 / 地平 / 地面色 + 强度、`[fog]` 启用 / 密度 / 高度衰减 / 颜色（**可选，缺失默认取天空地平色**）。文件头写明单位与口径（方向为"**由地表指向太阳**"、密度与衰减的量纲为 1/格）。
  2. **新增 `engine/render/lighting_table.*`**：照 ADR 0009 的机制落地 —— `LightingUniform`（std140，`static_assert` 钉死 **128 字节**）与 `BuildLightingUniform(table, 相机世界位置)` 作为**光照 → GPU 的唯一投影入口**；**颜色在 CPU 侧转线性**（`SrgbToLinear`），方向在此归一化。其余口径与 `material_table` 一致：非法配置（颜色越界 / 方向为零向量 / 缺字段 / `schema_version` 不符）**抛异常中止启动**，`Default()` 仅供测试。
  3. **`engine/render/mesh_renderer.*`**：新增 `SetLightingUniform`（与 `SetMaterialUniform` **完全同构**），经**片元 uniform 槽 1** 上传；片元着色器资源计数由 1 组 uniform 改为 **2 组**。槽 0 仍为材质。
  4. **`assets/shaders/mesh.frag`**：**删除写死的 `kLightDirection` / `kAmbientLight` / `kDiffuseLight`**，改为「方向光 + **半球天空光**」；随后在**光源之后、写 HDR 之前**在线性空间施加**指数高度雾**（`fogAmount = 1 - exp(-density · 视距 · exp(-heightFalloff · 相对相机高度))`，`fog.enabled = false` 时整段跳过）。逐像素权重、窄带 smoothstep、法线贴图、细节噪声、近零权重跳过采样**一律未动**。
  5. **`game/main.cpp`**：启动期加载 `lighting.toml`（失败即启动失败，与材质表同源）并打一行汇总日志；每帧构建 `LightingUniform`（**相机世界位置逐帧变化**）并上传；**清屏色改取配置的天空地平色（线性）**——它同时是雾色的默认来源，两者同源才能让远景与天空无缝、无硬边。
- 为什么：P1 是"体积感"的主体，而**只做阴影不做天空光是最常见的翻车方式**——一盏方向光下背光面接近全黑，物体像剪纸；先补天空光与雾，等于先把"环境与纵深"这两块底座铺好，阴影（T21b）才是锦上添花。另一条硬理由是 ADR 0010 的"**参数必须进配置**"：本轮把光照从着色器常量搬进 `lighting.toml` 后，**调光不再需要重编 Shader**，这也是后续凭目视校准色调的前提。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + 警告即错误）；`mesh.vert` / `mesh.frag` 的 **SPIR-V 与 DXIL 双格式产物均重新产出**。
  2. **测试**：`ctest --preset debug` → **131/131 passed**（原 116 + 新增 **15** 项，全部围绕配置表：合法解析、`Default()` 与文件一致、文件缺失 / `schema_version` 不符 / 颜色越界 / 颜色为负 / 方向为零向量 / 缺字段 / 标量为负 均抛异常、雾色缺失取地平色、显式覆盖、**线性化断言（sRGB 0.5 → uniform ≈ 0.2176）**、方向归一化、布局 128 字节与相机位置透传、`enabled=false` 标志位）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 85 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：前台启动 `voxel_game.exe`，持续 **> 12 秒**正常运行、**无 SDL 断言、无 ERROR / WARN**（多 uniform 槽位、std140 布局、管线格式一次性通过）；日志 `光照配置已加载：太阳方向 (0.45, 0.80, 0.30)（**由地表指向太阳**）强度 1.30；天空强度 1.00；雾 启用（密度 0.0030 /格，高度衰减 0.0200 /格，雾色 (0.70, 0.80, 0.92)）`。
  5. 文件逐字节校验：**纯 LF、无 BOM**；`toml++` 只出现在 `lighting_table.cpp`，未进入任何公共头文件。
- 下一步 / 遗留：① **观感待人工目视验收**（本环境无截图能力）：期望"**背光面呈天空色而非死黑**、远景自然融入天空、近景不被雾洗白"，且受光面不死白。② **中值色调推算已写进着色器注释**（受光面：草地 ≈ (0.21,0.54,0.16)、沙地 ≈ (0.80,0.76,0.57)、岩石 ≈ (0.45,0.48,0.55)；背光面感知亮度 0.13~0.32），若目视与推算不符，优先怀疑配置值而非着色器。③ **天空光强度取 1.00 而非物理比值**（约 1/10）是有意的：ACES 之前环境项过弱会把背光面压到 0（理由已写进配置与着色器注释）。④ **相机世界位置按绝对坐标上传**（V0.1 地图量级 ±100，float 无可见误差）；大世界 / LOD 阶段需与"相机相对渲染"一并复核（属待收敛项 4 的范围，本轮未引入新的精度类别）。⑤ 余下 **T21b（阴影通道 + CSM）** 与 **T22 / T23**。⑥ 本批改动**尚未提交**。

## 2026-09-26  P1 收官：阴影通道 + CSM 级联阴影（T21b）—— P1 三步全部完成

- 做了什么：
  1. **配置**：`assets/config/lighting.toml` 升 **`schema_version = 2`**，新增 `[shadow]`（启用 / 级数 3 / 分辨率 2048² / 覆盖距离 180 格 / `split_lambda` 0.75 / 深度偏移 / 法线偏移），逐项校验、非法即抛异常。
  2. **纯函数**：新增 `engine/render/shadow_cascade.*` —— `ComputeCascadeSplits`（practical split scheme）、`BuildCascadeLightMatrix`（正交光空间矩阵 + **texel 对齐**）、`ShadowUniform`（std140，`static_assert` **304 字节**）。**无任何世界 / 游戏专有类型**。
  3. **阴影通道**：`D32_FLOAT` **深度数组**（层数 = 级数，`DEPTH_STENCIL_TARGET | SAMPLER`，尺寸变化重建）+ 深度管线 + 每级一次渲染通道，**复用既有 tile 网格的顶点 / 索引缓冲**（不重复上传）。
  4. **主通道采样**：`mesh.frag` 新增 `sampler2DArray u_shadow` 与**片元 uniform 槽 2**；**按视轴线性深度选级**、**3×3 PCF**、几何法线偏移抗 acne；方向光项乘 `(1 − 遮蔽量)`，**天空光不受遮挡**；`enabled = false` 时整段跳过。
  5. **记账**：阴影图显存（`级数 × 分辨率² × 4 B`）计入 `RenderStats::textureBytes`，启动日志自动包含 —— ADR 0010 的记账义务。
- 为什么：阴影是"体积感"的最后一环——没有它，天空光与雾只能把画面变"亮"，不能让物体"立起来"。而 **CSM 的两个经典翻车点是"抖动"与"错位"**，所以本轮把两件事做成了**可单测的纯函数**：① **texel 对齐**（亚 texel 平移不改变矩阵 ⇒ 相机移动时阴影边缘不抖）；② **光空间矩阵在渲染原点坐标系下构造**（顶点是相机相对坐标，等价于世界空间矩阵右乘 `translate(-renderOrigin)` ⇒ 阴影不整体错位）。这两条现在都有断言钉死，而不是"看起来还行"。
- 验证：
  1. **构建**：退出码 0，**零错误零警告**；`mesh.*` 与新增 `shadow.vert/.frag` 的 **SPIR-V 与 DXIL 双格式产物齐全**（6 个 Shader 全齐）。
  2. **测试**：`ctest --preset debug` → **154/154 passed**（原 131 + 新增 **23** 项）；关键断言：分割单调且首尾正确、λ=0/1 退化、光空间矩阵把包围球 8 点全部映射进 `[-1,1]³`、**亚 texel 平移矩阵逐元素不变**、每级视锥切片被该级矩阵覆盖、`max_distance` 钳制覆盖、布局 304 字节，以及阴影配置的 11 种非法值**全部抛异常**。
  3. **门禁**：`scanned 88 file(s), 0 violation(s)`、`PASS`。
  4. **运行期冒烟**：前台运行 **> 14 秒**，**无 SDL 断言、无 ERROR / WARN**；日志 `阴影配置：启用（级数 3，分辨率 2048²，覆盖 180 格，split_lambda 0.75，depth_bias 0.0015，normal_offset 0.050 格）；阴影图预估 48.00 MB（占 VRAM 预算 300 MB 的 16.0%）`、`GPU 纹理显存记账：材质数组 2.67 MB + 深度目标 3.52 MB + HDR 目标 7.03 MB + 阴影 3 级 2048² 48.00 MB（占 78.4%）= 合计 61.21 MB（交换链 1280x720）`。
  5. 文件逐字节校验：**纯 LF、无 BOM**；`toml++` 只在 `.cpp`。
- 下一步 / 遗留：① **观感待人工目视验收**：看**地标塔与陡坡背光面** —— 期望地形起伏与塔身投出**方向一致**的阴影、随相机移动**不抖动**、背光面**无 acne 条纹**、阴影与物体**无可见分离**（本环境无法目视，已把"看哪里 / 期望什么"写清）。② **一处强制偏离（已留档）**：SDL3_gpu **不允许 `fragment_shader == nullptr`**（首次冒烟即断言命中），故深度通道挂一个**空入口**片元着色器 `shadow.frag`；该约束已写进 `references/meshing-and-render.md` §4 与学习笔记 Q17。③ **两处实现取舍**：阴影采样器用 **`NEAREST`**（线性过滤会插出非物理深度、使 PCF 失真，平滑靠手动 3×3）；剔除用 **BACK**（地表是单面高度场外壳，剔 FRONT 会让阴影图空白）。④ **已知未做**：阴影关闭时仍按配置分配深度数组（着色器需要该采样器）；矩阵缓冲不计入 `textureBytes`（属 buffer 而非 texture，与既有记账口径一致）。⑤ **预算结论更新**：纹理预算的**大头是阴影图**（48 MB / 78.4%），成本旋钮是**级数 × 分辨率²**——已回填方案 §7.2.1，现状合计 **61.21 MB**（1280×720 实测）。⑥ **P1 三步（T21a / T21b / T21c）全部完成**，下一步进入 **T22（PBR + 材质四件套）**。⑦ 本批改动**尚未提交**。

## 2026-09-26  P2 落地：PBR（Cook-Torrance）+ 材质四件套 + 宏观变化 + 多尺度程序生成（T22）

- 做了什么：
  1. **配置**：`assets/config/materials.toml` 升 **`schema_version = 3`**，每层新增 `roughness` / `ao` / `macro_uv_scale` / `macro_strength`（逐项校验、非法即抛异常）。
  2. **程序生成升级为多尺度**（`world/terrain/material_textures.*`）：albedo / normal 由**单频噪声**改为**低频结构 + 高频颗粒双频段叠加**（这是"细腻"的关键，单频噪声永远平）；**新增三张数组** —— roughness（4 层）、AO（4 层）、**macro 宏观变化（1 层）**；全部保持**逐字节确定性**与可平铺。材质总显存 **2.67 → 5.67 MB**。
  3. **`world/terrain/material_table.*`**：`MaterialLayer` 加 4 字段，`MaterialUniform` 扩到 **272 字节**（≤ 512 上限，`static_assert` 钉死），仍由 `BuildMaterialUniform` **单入口投影**。
  4. **`assets/shaders/mesh.frag` 改为 PBR**：Cook-Torrance（**GGX + Smith + Schlick**，电介质 `F0 = 0.04`）；**AO 只作用环境项**（不压直接光）；宏观变化按 `macro_uv_scale` 调制 albedo 与 roughness；**四件套的采样仍在"仅权重最高 3~4 层"守卫内**（沿用 ADR 0009 的第一性能旋钮）；`set = 2` 绑定槽 0~5（albedo / normal / roughness / AO / macro / 阴影）。逐像素权重、法线贴图、CSM 阴影、指数高度雾、线性 HDR 输出**语义未动**。
  5. **`engine/render/mesh_renderer.*`**：支持 5 张材质数组绑定（片元采样器声明 3 → 6）；`game/main.cpp` 创建上传并绑定，日志补充材质显存。
- 为什么：P1 把画面从"平"变"有光影"，但**地表材料本身仍只有 albedo + 法线**——没有镜面项与粗糙度差异，岩石与沙地在数学上是同一种材质，只能靠颜色区分，这就是"不够 3A"的最后一块短板。补 PBR 与四件套后，"硬"与"软"、"湿"与"干"才有可表达的维度。同时按 ADR 0010 P2 的要求把**程序生成升级为多尺度**：单频噪声无论怎么调都是"糊"，多频叠加才能让近景有颗粒、远景有结构。
- 验证：
  1. **构建**：退出码 0，**零错误零警告**；`mesh.frag` 的 SPIR-V 与 DXIL **双格式产物均重新产出**。
  2. **测试**：`ctest --preset debug` → **163/163 passed**（原 154 + 新增 **9** 项）：新字段解析与 7 种非法值抛异常、`schema_version` 必须为 3、uniform 布局 272 字节且新字段投影正确（**改 TOML 必须改变 uniform**）、三张新图的尺寸 / 层数 / 确定性（两次生成逐字节相同）/ 换种子变化 / 值域、**albedo 多尺度的高频能量显著高于纯低频结构**。
  3. **门禁**：`scanned 88 file(s), 0 violation(s)`、`PASS`。
  4. **运行期冒烟**：前台运行 **> 14 秒**，**无 SDL 断言、无 ERROR / WARN**；日志 `材质贴图已生成并上传：256×256，R8G8B8A8_UNORM；albedo/normal/roughness/AO 各 4 层 + macro 1 层，含 mip 约 5.67 MB 显存`、`地表世界就绪：… 材质表 schema_version=3 …`、`GPU 纹理显存记账：材质数组 5.67 MB + 深度目标 3.52 MB + HDR 目标 7.03 MB + 阴影 48.00 MB（占 74.8%）= 合计 64.21 MB（1280x720）`。
  5. 文件逐字节校验：**纯 LF、无 BOM**；`toml++` 只在 `.cpp`。
- 下一步 / 遗留：① **观感待人工目视验收**：看**陡坡岩壁的掠射角** —— 期望岩石出现**可分辨的方向性高光**（粗糙度 0.40）、草地与沙地仍为哑光（0.90 / 0.95），且**不出现死白**（着色器注释里给了推算：岩石无高光 ≈ 0.44、含高光 ≈ 0.69）。② **一处口径决定（已留档）**：漫反射的 `1/π` **折进光照强度常量**，以保持与 P1 的亮度对齐、避免重新校准 `lighting.toml`（若将来改成"显式 /π + 强度乘 π"，必须同步复核中值色调表）。③ **带宽影响**：每层最多 5 次采样（比 P1 多 3 次），§7.2.2 里材质采样本就是最大一笔 —— 这是 ADR 0009/0010 预期内的代价，压它的旋钮仍是"层数 / 分辨率"。④ **仍无真实美术 PBR 资源**（程序生成占位），故能力状态标"部分实现"。⑤ 下一步 **T23（P3：MSAA 4× + 多频细节法线）**——**开工前须先定 MSAA 的颜色格式**（`RGBA16F` ≈95 MB vs `R11G11B10_FLOAT` ≈63 MB，见方案 §7.2.1）。⑥ 本批改动**尚未提交**。

## 2026-09-26  P3 落地：MSAA 4× + 多频细节法线（T23）—— 渲染质量线 P0~P3 全部完成

- 做了什么：
  1. **MSAA 档位进配置**：`engine/platform/settings.*` 新增 `msaa_samples`（**合法档 {1,2,4,8}**，默认 **4**；可选字段、缺失取默认、类型错误报错、越界**取最近合法档（并列向上）**）。
  2. **多采样渲染路径**（`engine/render/mesh_renderer.*`）：主通道渲进 `R16G16B16A16_FLOAT` **多采样颜色目标**（+ `D32_FLOAT` 多采样深度），再 **resolve 到单采样 HDR 目标**（色调映射仍读单采样目标，**未改动色调映射通道**）；**阴影通道保持 1×**；**档位 = 1 时走零开销路径**（不创建多采样纹理）；档位或尺寸变化时重建（管线须与目标采样数严格一致，故**按档位重建主通道管线**，Shader 改为持有到析构）；`SetMsaaSampleCount` 会按 `SDL_GPUTextureSupportsSampleCount` 取**不高于请求的受支持档**，避免管线与目标不一致。
  3. **记账**：MSAA 颜色 + 深度目标**均计入 `RenderStats::textureBytes`**，启动日志按项打印 ⇒ "开 / 关 MSAA 的代价"可直接对比（实测 **4× = 38.67 MB，1× = 0.00 MB**）。
  4. **多频细节法线**（`assets/shaders/mesh.frag`）：在基础法线之上叠加**第二频段**（UV ×4）细节法线，用 **RNM** 合成；**只对权重最高的层**做（每片元仅多 1 次采样，而非每层一次）——取舍已写进注释（§7.2.2 确认材质采样是最大一笔带宽）。PBR / 阴影 / 雾 / 线性 HDR 语义**未动**。
- 为什么：**MSAA 治的是"锯齿 + 远处闪烁"**——这是"20 年前观感"里最容易辨认的一项，且它与前三个阶段的收益**不重叠**（色调映射治"灰"、阴影治"没有体积"、PBR 治"纸板"、MSAA 治"锯齿"）。顺序上它排在最后，因为它的成本最"硬"（纯显存与带宽，且与分辨率² 成正比）。
- 验证：
  1. **构建**：退出码 0，**零错误零警告**；`mesh.frag` 的 SPIR-V 与 DXIL **双格式产物均重新产出**。
  2. **测试**：`ctest --preset debug` → **165/165 passed**（原 163 + 新增 2）：`ClampMsaaSampleCount` 纯函数（`3→4`、`5→4`、`6→8`、`0→1`、`64→8`；**并列向上**）与"可选字段 / 类型错误报错 / 往返持久化"。
  3. **门禁**：`scanned 88 file(s), 0 violation(s)`、`PASS`。
  4. **运行期冒烟（两档对比，各 >13 秒，无 SDL 断言、无 ERROR/WARN）**：
     - 4×：`MSAA：4×（开启，主通道渲进多采样目标后 resolve 到单采样 HDR 目标）`、`GPU 纹理显存记账：材质数组 5.67 + 深度 3.52 + HDR 7.03 + 阴影 48.00 + MSAA 4×（颜色 28.12 + 深度 10.55 = 38.67）= 合计 102.89 MB（1280x720）`
     - 1×：同一行显示 `MSAA 1×（… = 0.00 MB）`，**合计回落到 64.21 MB**（与 P2 基线一致 ⇒ 1× 确实零额外显存）
  5. **采纳的一处更优做法（按「需求受理」第一步直接执行，此处对照说明）**：实现首版用了 `SDL_GPU_STOREOP_RESOLVE_AND_STORE`（任务说明的字面写法），但 `SDL_gpu.h` 明说它"需要可观的显存带宽"、而 `SDL_GPU_STOREOP_RESOLVE` 才是"最省带宽"的解析方式 —— 而本项目**多采样目标的内容此后不再被采样**，保留它纯属浪费。⇒ 已改为 **`SDL_GPU_STOREOP_RESOLVE`**（观感完全相同、带宽更省）。**你要的效果：抗锯齿；我们的实现：resolve 后丢弃多采样内容；依据：SDL_gpu.h §SDL_GPUStoreOp + 本通道的消费关系。**
- 下一步 / 遗留：① **观感待人工目视验收**：看**地标塔边缘与远处地形** —— 期望边缘无可见锯齿、远处地形随相机移动**不闪烁**。② **显存现状（实测 1280×720）= 102.89 MB**，其中**阴影图 48.00 + MSAA 38.67 = 86.67 MB 占约 85%**——已回填方案 §7.2.1，并写明"要压预算先动这两处，而不是先降材质贴图"。③ **已知取舍**：MSAA 档位变化**需重启生效**（当前无 UI 控件，已登记在 `ui-inventory.md` §3）；阴影通道不开 MSAA（只写深度 + 手动 PCF，开了只增开销）；深度目标不做 resolve（`SDL_gpu.h`：深度/模板目标不支持多采样解析）。④ **渲染质量线 P0~P3 全部落地**；后续可选（ADR 0010 的切换条件）：SSAO / 视差遮蔽 / triplanar / RVT / 后处理 AA，均**须另开 ADR 或按切换条件重新评估**。⑤ 本批改动**尚未提交**。

## 2026-09-26  SKILL 新增「缺陷先判真伪」；修 B6（阴影随视角变化）与 B7（材质带空洞）；落地 T25（陡壁三平面）

- 做了什么：
  1. **SKILL 新增「缺陷报告：先判真伪，再决定动作」**（项目所有者明确要求）：四类判定（**确认是缺陷** / **已登记的已知限制** / **设计使然** / **无法判定**）+ 每类的**依据要求**（确认是缺陷必须同时给出**被违背的契约**与**机制链条**，并补回归测试）+ 三条禁止（不许不判就改、不许只回"不是 bug"、不许靠"看起来像"）+ 三条真实案例（下文的 B6 / 陡壁拉伸 / B7）；同步写进 DoD 自检项。
  2. **B6 · 超高投射体的阴影随视角变化（确认是缺陷，已修）**：级联光空间盒只包"视锥切片外接球" ⇒ 高于该球的地标塔顶部被裁掉（`enable_depth_clip` + X/Y 越界）⇒ 只有落在盒内的那段塔身参与投射，相机一动投射段就变。修复 = **shadow caster extension**（沿光轴远端前移 `casterHeight/|dir.y|`、X/Y 各扩 `casterHeight·tanθ`），`casterHeight` 由 `TerrainWorld::MaxSurfaceHeightBlocks()` **自动推导**（每级独立取 `max(配置下限, 最高地表高度 − 该级切片中心高度)`），配置新增 `caster_height_min`（160 格）。
  3. **B7 · 高处平地一律被涂成草色（确认是缺陷，已修）**：旧材质带在 **高度 120、坡度 0** 处**四层隶属度全为 0** ⇒ 落到"无匹配 → 槽位 0"兜底 ⇒ 整片平地强制草色；"土环"来自土的中坡带、"岩边"来自岩的坡度带。修复 = 重调带参数使**覆盖完备**（新增**不变量单测**：`高度∈[0,512] × 坡度∈[0,1]` 全域覆盖量 > 阈值），并把**岩改为纯坡度驱动**（坡度带 `[0.55,1.0]`、**不含 0**、高度带覆盖全域）。
  4. **T25 · 陡壁三平面（已登记的已知限制，经批准提前实施）**：按坡度**逐像素**在平面投影与三平面之间混合；项目所有者追加的硬要求「**随地形改变自动切换**」由"权重由世界空间法线算出"天然满足 —— **笔刷挖 / 堆后无需任何额外动作**（无烘焙、无重建、无 CPU 分支）。
- 为什么：
  1. 三条判定的**依据**（按新 SKILL 的要求写全）：B6 违背"阴影是几何与光照的函数、不随相机朝向变化"（T21b 验收"可见且不抖动"）；B7 违背"兜底分支只给异常输入"（权重归一化的数学完备性 ≠ 正常路径）；陡壁拉伸则是**已登记**的限制（ADR 0009「不在本次范围」+ ADR 0010「切换条件」），故按规则**不动代码**、由所有者批准后才实施。
  2. **我否决了子代理的第一版材质带修复**（这条值得记）：那版让"岩"的坡度带取全量程 `[0,1]`，靠"岩在任意高度恒有正权重"来保证覆盖 —— 结果是 **平台平地变成草:岩 ≈ 1:1 的灰绿混合**（用户抱怨的"颜色不自然"其实没解决），而它的单测只看**主导层**（并列时返回层号较小者）所以**通过了**。⇒ 教训：**"主导层正确"不等于"观感正确"**，权重比例本身也要有断言或至少人工目视。重调后的带：草 `[0,320]±60`×`[0,0.45]±0.10`、土 `[300,512]±60`×`[0,0.55]±0.10`、岩 `[0,512]`×`[0.55,1.0]±0.10`、沙不变 —— 平台 `(120,0)` 现在是**纯草（草 1.0、土/岩 0）**，坡沿是"草 →（窄过渡）→ 灰岩"，**低海拔陡壁仍是灰岩**（不是土色）。
  3. **显式取舍（已写入配置与文档）**：草的高度带止于 320 ⇒ **高海拔缓坡由土（碎石土）接管**，即"高海拔偏土"而非旧版"高海拔偏岩"。原因是"高海拔**平地**呈岩"要求岩的坡度带包含 0，而那会同时把平台涂成灰绿 —— **两者在乘积模型下不可兼得**，选了观感更常见的"平台纯草 + 陡壁灰岩"。若日后要"高山雪线 / 岩顶"，须**新增第 5 层**（槽位与纹理数组层数都要改，须另立任务）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**；`mesh.frag` 的 **SPIR-V 与 DXIL 双格式产物均重新产出**（本机 Shader 时间戳 23:41:09 / 23:41:11）。
  2. **测试**：`ctest --preset debug` → **176/176 passed**（基线 165 → **+11**：A 项 4、B 项 3、C 项 4）。
  3. **修复前失败 → 修复后通过的证据**（新 SKILL 要求）：**B6** 旧实现下 `coveredClip.y = 4.068 > 1`、`coveredClip.z = -2.291 < -1`（高大投射体确实越界）；**B7** 旧带下全域扫描在 `height=0 slope=0.76` 处覆盖量为 0。修后均通过，且既有断言（亚 texel 平移矩阵不变 / 包围球 8 点在内 / 每级视锥切片覆盖 / 窄带约束）**一字未放宽**。
  4. **门禁**：`scanned 88 file(s), 0 violation(s)`、`PASS`。
  5. **运行期冒烟**：前台运行 **> 15 秒**，**无 SDL 断言、无 ERROR / WARN**；日志 `阴影配置：… **投射体扩展下限 160 格**`、`地表世界就绪：… 材质表 schema_version=4`、`GPU 纹理显存记账：… 合计 102.89 MB`。
  6. 文件逐字节校验：**纯 LF、无 BOM**；`toml++` 只在 `.cpp`。
- 下一步 / 遗留：① **观感待人工目视验收**：**相机转动时高柱阴影是否稳定**（B6）、**平台平地是否是一条自然的绿、坡沿是否不再有宽棕环**（B7）、**陡壁侧面纹理是否不再竖向拉伸**（T25）。② **已知取舍**：高海拔缓坡现为"土（碎石土）"而非岩；三平面的法线未做逐轴 swizzle（本管线无一致切线空间，属已知近似）；两处若要改都需新任务。③ **A 项的固有代价**：为覆盖高塔，近景级联盒变大 ⇒ 阴影 texel 变粗；若观感不可接受可下调 `caster_height_min` 或提高 `resolution`（两者都在 `lighting.toml`）。④ 两项仍未闭环：**I1（CI 红：Windows C4864 + Linux libxcrypt）**、**T19 的两处待决口径**。⑤ 本批改动**尚未提交**。

## 2026-09-26  SKILL 新增两条交付规范：「3A 基线」与「交付必附运行命令」（M6）

- 做了什么：按项目所有者要求补充两条**过程硬规则**，并写入 DoD 自检项：
  1. **「3A 基线」**（新增于「需求受理」节）：**每一次动手前**除"业界标准做法"外，还必须显式对照 3A 大作的质量要求，回答三问 ——
     ① **业界 / 3A 同类问题怎么做**（须给 **2~3 个具体参照**，不许只写"业界标准"）；② **本项目的 3A 基线形态**（必须落成**可判定的判据**：看什么现象、量什么数字）；
     ③ **是否降级**。若达不到或受条件限制而降级，**必须**写明「**降级到什么程度（可判定差异）· 为什么达不到（具体约束）· 备注（何时补上、切换条件）**」。
     附三条硬性要求：**禁止默默降级**（只在代码注释里写不算，必须写进回复与 devlog 的「为什么」）、**禁止拿"条件所限"当免罪符**（要点出具体约束并给依据）、**降级必须可回滚**（写明触发条件并登记待办）；并声明**不得以"3A 也这么做"为范围扩张理由**。
  2. **「交付必附运行命令」**（新增为第七节 **7.6**）：每次开发完成后必须在**回复中直接给出**（不是"见某文档"）——**构建命令**（写全 VS DevShell 初始化 + `VCPKG_ROOT`）、**运行命令**（完整路径，含"先停残留进程"的提醒）、**测试要点**（逐条写清看哪里 / 期望什么），**若涉及配置**还要写明配置文件**实际路径**与**关键字段**及临时开关方式。
- 为什么（按新规则本身也做了 3A 基线对照）：
  1. **业界 / 3A 参照**：成熟 3A 团队的实践中，① 每个特性开工前都有 **milestone / tech review** 与 **tech art bible** 对标（先定"达标长什么样"再动手），② 交付时附**可复现的构建与运行说明 + QA 验收清单**（QA 不看代码，只看现象）；本条正是把这两件事落成本仓库可执行的规则。
  2. **本项目的可判定判据**：规则本身"可判定"的定义 = ①条文成文且带**来源**；②每问都有**格式要求**（2~3 个具体参照 / 可判定的现象或数字 / 降级三点）；③进 **DoD 自检项**（能被逐条勾选）。三条均满足。
  3. **降级情况**：**本次无降级**（两条规则为纯文本改动，无技术约束）。
- 验证：规则位置 = `SKILL.md`「需求受理」节的「3A 基线」小节 + 第七节 7.6 + DoD 两项自检；阶段计划登记为 **M6**；**本回复已按新规则执行**（给出了可直接复制的构建命令、运行命令与逐条测试要点，见文末）。
- 下一步 / 遗留：① 后续每一条开发任务的**计划条目**都要带上「3A 基线三问」的答案（含降级三点），否则视为未填；② 每次交付的回复末尾都要附运行命令与测试要点；③ 两项仍未闭环：**I1（CI 红）**、**T19 的两处待决口径**。④ 本批改动**尚未提交**。

## 2026-09-26  缺陷 B8：阴影"仍随视角变化"（B6 未闭环）——真因是级联半径漂移 + 跨级联重采样

- 做了什么：
  1. **接受复报、重新判定**（按 SKILL「缺陷先判真伪」）：人工复测"超高柱阴影仍随视角变化" ⇒ 判定为**确认是缺陷**（B6 只治了"高大投射体被裁"这一条因，机制链条未断）。复查后补出**两条独立真因**并各自修复。
  2. **真因一 · 级联半径随视角漂移**：每级光空间盒半径取"视锥切片 AABB 的外接球"，相机朝向 / 位置微动即变 ⇒ texel 对齐的量化网格逐帧漂移，阴影边缘游移。修法 = 新增纯函数 **`QuantizeTexelWorldSize`**：把每级 texel 的**世界尺寸向上量化为 2 的幂**，再由量化值**反推** `halfExtent`（覆盖不缩水），使每级尺寸**分段恒定**。
  3. **真因二 · 跨级联重采样**：级联按"沿视轴的线性深度"切换，边界附近同一地面点会在相邻两级间跳变，而两级 texel 尺寸与光空间盒不同 ⇒ 重采样出硬跳。修法 = 新增纯函数 **`CascadeBlendWeight`**：在 `cascade_blend × 该级远平面` 宽的过渡带内**同时采样相邻两级并按 smoothstep 加权混合**（两级权重和恒为 1）；并把"超出最远级联"从**直接返回无阴影**改为 **clamp 到最远级联**。
  4. `assets/config/lighting.toml` 升 **`schema_version = 5`**、新增 `shadow.cascade_blend = 0.1`（校验 ∈ `[0, 0.5]`）；`ShadowUniform` **304 → 320 B**（新增 `cascadeBlend` 槽）；`mesh.frag` 的 `ShadowBlock` 相应增一 `vec4`。
- 为什么：
  1. **依据（契约 + 机制）**：被违背的契约 = T21b 验收"阴影**可见且不抖动**"（阴影是几何与光照的函数，不随相机朝向变化）。机制链条 = ① 半径源为视锥切片外接球（朝向相关）→ texel 网格漂移；② 级联边界硬切换（两级采样参数不同）→ 重采样跳变。
  2. **B6 未闭环的复盘**：上一轮把"随视角变化"整体归因到投射体被裁，**未把"边界游移"与"级联跳变"当作独立机制分开验证** —— 教训与 SKILL 已记的"一个现象可能有多条独立真因，必须逐条给机制"一致。
  3. **3A 基线对照**：① **业界参照**：主流引擎（Unreal / Unity）的 CSM 普遍采用 **texel 尺寸量化到稳定步长** 与 **级联间过渡带混合（cascade blend / fade）**，两者都是治"阴影抖动 / 级联接缝"的标准手段（参照 Unreal "Cascaded Shadow Maps" 文档、Unity URP shadow cascade blending、Frostbite 的 CSM 实践）。② **本项目可判定判据**：同一个小半径扰动下，每级 texel 世界尺寸与光空间矩阵的缩放分量**必须不变**（单测）；级联边界处相邻两级权重**和恒为 1**、过渡带外退化为单级（单测）。③ **降级**：见下条。
  4. **降级三点（显式记录）**：**程度** —— texel 世界尺寸因 2 的幂量化最多粗化约 **2×**，阴影边缘略软；**原因** —— 固定 2048² 分辨率下，量化步长必须粗于原始 texel 才能吸收半径微动，属"稳定 vs 锐利"的固有取舍；**备注 / 切换条件** —— 若观感不可接受，可提高 `shadow.resolution`，或改回非 2 的幂步长（代价是跨帧稳定性变差，须重测）。此结论已同步写入 ADR 0010 修订说明。
- 验证：
  1. **修复前失败 → 修复后通过**（新 SKILL 要求的证据）：新增回归测试 `CascadeScaleStaysConstantUnderSmallRadiusChange` —— 修前，级联 2 半径 +3% 后光空间矩阵缩放分量变化 `3.14e-5 > 1e-5`、texel `0.35370` vs `0.34892`（不一致）；修后通过。
  2. **测试**：`ctest --preset debug` → **200/200 passed**（基线 176 → **+24**：B8 与 T26 合计）。
  3. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**；`mesh.frag` 的 **SPIR-V 与 DXIL 双格式产物均已重新产出**。
  4. **门禁**：`scanned 88 file(s), 0 violation(s)`、`PASS`。
  5. **运行期冒烟**：前台运行 **> 15 秒**，**无 SDL 断言、无 ERROR / WARN**；日志 `阴影配置：启用（级数 3，分辨率 2048²，覆盖 180 格，… 投射体扩展下限 160 格，**级联混合 0.10**）`、`GPU 纹理显存记账：… 合计 102.89 MB`。
- 下一步 / 遗留：① **待人工目视验收**：绕高柱转一圈，确认阴影**边缘不再游移、级联接缝不可见**。② 已知取舍见上（2 的幂量化 ≤ 2× 粗化）。③ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。④ 本批改动**尚未提交**。

## 2026-09-26  T26：地形笔刷升级为「平整填平 / 削平 / 平滑爆破」+ 能力清单入库

- 做了什么：
  1. **新增配置表 `assets/config/brush.toml`**（`schema_version = 1`，`radius` / `strength` / `falloff` / `crater_depth` / `crater_radius` / `crater_rim`）：笔刷行为的**唯一事实来源**，参数校验失败**中止启动**、不静默回退（口径同材质 / 光照表）。
  2. **`world/dig/terrain_brush.*` 能力升级**：新增纯函数 **`BrushFalloff`**（`1 − SmoothStep(r·(1−band), r, d)`，边界一阶连续）、**`ApplyTerrainLevel`**（`Level` 模式：半径内每列向目标高度收敛，`Fill` 只抬低处 / `Shave` 只削高处，另一侧不动）、**`ApplyTerrainCrater`**（`Crater` 模式：坑体下沉 + 外环隆起，`rim·RimProfile(d/R) − depth·BrushFalloff(d/R)`，中心与边界值及一阶导均归零）。保留"只标脏受影响 tile"的精确性。
  3. **`game/main.cpp` 接线与键位**：**右键 = 平整填平、左键 = 削平、Shift + 左键 = 爆破**（按住持续施力，按固定步长 dt 把 `strength` 折算为每步最大改动）；启动日志打印生效值。
  4. **内容基线文档**：`docs/game-design.md` 新增 **§2.3 主角与 NPC 能力清单**（能力 / 状态 / 参数 / 落点），原 §2.3 顺延为 §2.4；SKILL 第六节 6.7「内容基线文档」补入**能力清单的持续维护义务**。
- 为什么：
  1. **需求受理**：项目所有者要求"当前是粗糙的范围内每个像素高度增加 ⇒ 改成增加时优先填平脚下一定范围、破坏同理、爆破区域应平滑，为修仙世界战斗破坏地形做准备"。判定：**不矛盾的更优做法**（旧实现是"整列等量增减"，会把地面切成台阶）⇒ **直接执行**，不提问。
  2. **3A 基线对照**：① **业界参照**：3A 开放世界的地形编辑普遍用**带平滑衰减的笔刷**（Unreal Landscape 的 flatten / sculpt 用 falloff 曲线；Unity Terrain 的 smooth / flatten paint；《雾锁王国 / Enshrouded》《Rust》等的爆破弹坑为平坦底部 + 平滑外环），**没有**用"范围内等量硬增"的做法。② **本项目可判定判据**：平整后目标区高度**极差 → 0**；爆破剖面**二阶差分峰值有界**（平滑 = 无尖角），且坑心 / 边界一阶导为 0。③ **降级**：见下条。
  3. **降级三点（显式记录）**：**程度** —— 破坏仍限于**高度场**（只能上下移动既有列），**不是真体积破坏**（无法掏出洞穴、悬垂、地道）；**原因** —— 可挖体积（局部 SDF + Surface Nets，T8）**尚未开始**，当前世界表示只有高度场；**备注 / 切换条件** —— 待 T8 落地后，爆破笔刷将在 SDF 体积内做**三维**平滑挖除，届时 `Crater` 的高度场实现作为"仅地表层"的快速路径保留。此边界已在 `game-design.md` 能力清单与阶段计划中标注。
- 验证：
  1. **平滑性证据**（单测，可判定）：爆破剖面**二阶差分峰值 1.3125 格**，对照"硬边笔刷"的 **6.3125 格**（阈值 2.0）；平整收敛后目标区高度**极差 = 0**（真正"填平"而非近似）。
  2. **测试**：`ctest --preset debug` → **200/200 passed**；新增测试文件 `tests/terrain_brush_test.cpp`。
  3. **构建 / 门禁 / 冒烟**：退出码 0、零错误零警告；门禁 `0 violation(s)`；前台冒烟 > 15 秒无 ERROR / WARN，日志 `笔刷配置已加载（schema_version=1）：半径 6.0 格，平整速率 6.0 格/秒，衰减带 0.60；爆破 深 6.0 / 坑半径 8.0 / 外环 2.0 格`。
- 下一步 / 遗留：① **待人工目视验收**：右键填平 / 左键削平 / Shift + 左键爆破的**手感与形状**（坑底是否平坦、外环是否自然、边缘是否无台阶）。② **T8 体积破坏仍未开始**（降级备注里的切换条件）。③ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。④ 本批改动**尚未提交**。

## 2026-09-27  世界层第 ② 层落地：T8 可挖体积（SDF + Surface Nets + 层间接管）+ T27 光球（重力弹道 / 命中爆炸 / 破坏改由光球触发）

- 做了什么：
  1. **T8-a 可挖区域标记表**：新增 `assets/config/dig_regions.toml`（`schema_version = 1`，`test_hill_flank`）+ `world/dig/dig_region.*` —— 区域包围盒**向外吸附到 32 的整数倍**、`Contains` 半开区间判定、`priority` / `diggable` 合并（高优先级的 `sealed` 覆盖低优先级可挖），并在 `RebuildBlocks()` 里**按合并结果过滤**：被覆盖的块直接剔除；**文件缺失返回空表、不报错**（与材质 / 光照表"非法即抛"口径一致，唯独"文件不存在"按"本关无可挖区"处理）。
  2. **T8-b Surface Nets 网格化**：新增 `world/dig/volume_mesher.*` —— 32³ 体素 / 33³ 采样 / **34³ 采样范围（索引 -1..32）**，每 cell ≤ 1 顶点、顶点位置随密度连续变化，**法线由密度场梯度给出**；**块间共享边界采样**（越界采样回退到世界采样，保证块间接缝为零，与地表 tile 65×65 同构）；纯函数，只依赖 `IVolumeSampler` 接口。
  3. **T8-c 可挖体积世界**：新增 `world/dig/dig_volume.*` —— 33³ `int8` 密度块（区域**外**由高度场公式 `d = clamp((y − 地表高度) × 127, ±127)` 派生）、球体 **CSG 挖除**（`d = max(d, radius − dist)`，并在球外 1.5 格内再写一次 `max` 使墙面附近密度成为**真实距离**而非饱和值）、脏块重网格、区域内外密度回退。
  4. **T8-d 层间交接**：新增 **[ADR 0011](adr/0011-layer-transition-volume-takeover.md)** —— **在可挖区域内，地表网格让位**：凡"**四角全部**落在可挖区域内"的地表四边形，`terrain_mesher` **直接不发射**，由体积等值面接管这片可见表面。落地为 `ITerrainQuadFilter`（声明在地表网格化侧）+ `TerrainWorld::SetQuadFilter` + `DigRegionTable::SkipQuad`，**依赖方向保持 `dig → terrain`**（地表网格化不认识"可挖区域"这个概念）。
  5. **T27-a 光球弹道与命中**：新增 `game/orb.hpp`（header-only，便于测试）—— `IOrbWorldQuery` 契约、`MarchRay` 步进命中（命中点取 `(lastSafe + point) · 0.5`）、`StepOrb`（**半隐式欧拉**：先 `v.y -= g · scale · dt` 再位移；超时置 `active = false`）、`OrbPool`（固定容量）、`BuildOrbMesh`（程序化球网格）。
  6. **T27-b 弹丸配置表 + 自发光**：新增 `assets/config/projectiles.toml`（`schema_version = 1`，`max_active = 16`，一条 `[[projectile]]` `id = "light_orb"`；`[[projectile]]` 是**数组** ⇒ 多类型弹丸的架构位已经留出，本轮只实现光球）+ `world/dig/projectile_table.*`（逐项校验、非法即抛）；`mesh.frag` 新增 `EmissiveBlock`（片元 uniform **槽 3**），在**雾之后**叠加自发光，光球因此不会像"贴图发亮的石头"。
  7. **T27-c 接线**：`game/main.cpp` **删除 `BrushAction` / `ApplyBrush`**，改为**鼠标左键发射**（按 `fireIntervalSeconds` 冷却）、固定步内遍历活动弹丸调 `StepOrb`、命中即 `Detonate` **分流**为「区域内 → `CarveSphere` 体积挖除 + 重网格」与「区域外 → 地表爆破」；另加 `LiftCharacterIfBuried` 防止玩家被自己炸出的洞"埋"进体积。`gameplay_input` 的 `mouseBrush` → `mouseAction`；`ui_text` / `debug_overlay` 增「爆炸半径」「光球（活动 / 上限）」「可挖体积块（含已挖数）」（中英两表同步）；`assets/maps/test_range.toml` 新增 `dig_hill`（x∈[6,26]、z∈[-56,-36]，高出平地 80 格）供侧向挖洞试射。
  8. **文档同步**：ADR 索引 + 阶段计划 T8 / T27 + `engine-capabilities.md` + `game-design.md`（能力清单）+ `file-index.md` + `ui-inventory.md`（§2.2 展示项）+ `learning-notes.md`（新增「SDF 与等值面提取」条）。
- 为什么：
  1. **根因：高度场"只能改高度"是数学限制，不是实现 bug。** 人工反馈"破坏只是简单的地形高度改变，想从山的侧面挖洞" —— 高度场每列只有一个高度值，**根本无法表达悬垂与横向洞穴**（这正是 ADR 0004 引入第 ② 层的理由）。故本轮的**必要动作不是修笔刷，而是把 ② 层建起来**，再把破坏能力从"改高度"迁到"改体积"。经与项目所有者确认范围，选择**光球 + 同时做 T8 可挖体积**，并把**地形破坏改为只由光球触发**（原鼠标挖 / 堆笔刷解绑）——避免两套破坏入口并存造成"到底谁在改地形"的混乱。
  2. **3A 基线对照**：① **业界参照**：可破坏地形的主流做法是"**分层世界表示** + 局部 SDF 体积"，如《雾锁王国 / Enshrouded》《Deep Rock Galactic》（全可挖体素或分区体素 + 等值面网格化）、《Teardown》（体素 + 精确 CSG 破坏）、Unreal 的 Geometry Collection / Chaos Destruction（局部破坏体）；**弹丸**普遍是"配置驱动的弹道 + 命中回调触发破坏"（Unreal Projectile Movement Component + `OnHit`），本项目与之同构（`projectiles.toml` + `StepOrb` + `Detonate`）。② **本项目可判定判据**：区域包围盒吸附与包含判定一致（单测）；SDF"地下负 / 空中正 / 表面跨零"（单测）；**球面 SDF 的 Surface Nets 网格面积与解析值一致**（单测，阈值内）；球体挖除后**球心为空、球外采样逐值不变**（单测）；地表网格**只在区域足迹内**被跳过、足迹外逐 tile 逐列不变（单测）；无命中时**弹道落点与离散解析解一致**（单测）；命中点落在表面附近（单测）。③ **降级**：见下条。
  3. **降级三点（显式记录）**：**程度** —— 可挖体积**尚无物理碰撞**（未接 Jolt `MeshShape`），故**洞口可见但走不进去**（仍被原地表的隐形高度场挡住）；另有 ADR 0006 的**程序化规则**部分未做（本轮只用数据文件标记）、体积**无流式加载与存档**。**原因** —— 体积是**运行期重建的等值面网格**（挖除后重网格），接进 Jolt 需要"网格变更 → 物理体重建"的链路与增量代价评估；本轮范围只到"可见 + 可挖"，物理与大量破坏下的性能留作下一步。**备注 / 切换条件** —— 下一步任务（已登记在阶段计划 §3「下一步」第 2 条）即为体积物理；在它落地前，洞口是**纯视觉**的。
  4. **层间过渡为什么用"区域整体接管"而不是"按爆炸球在洞口打洞"**：后者会让地表网格的洞是**四边形台阶边界**、而体积腔壁是**球面**，两者对不上 ⇒ 洞口一圈裂缝。ADR 0011 里有完整的备选方案对比与"何时重新审视"。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + 警告即错误）；`mesh.frag` 的 **SPIR-V 与 DXIL 双格式产物均重新产出**。
  2. **测试**：`ctest --preset debug` → **226/226 passed**（基线 200 → **+26**：`volume_mesher` 7 + `dig_region` 7 + `dig_volume` 3 + `orb` 9）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 100 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**：前台启动 `build\debug\bin\voxel_game.exe`，持续 **> 300 秒**仍在运行、且**无 SDL 断言、无 ERROR / WARN**；日志 `可挖体积就绪：4 个块（密度 0.14 MB），其中 4 块存在等值面（Surface Nets 网格化，法线由密度梯度给出）；**区域内已由体积接管地表网格**（ADR 0011）`、`可挖体积网格已上传：4/4 个块有可见表面（其余块全实心或全空，无等值面）`、`渲染原点重定基` 等正常运行记录。
  5. **修复前失败 → 修复后通过的证据**（两处都是真 BUG，均按 SKILL「缺陷先判真伪」走了"给机制 + 补回归测试"）：
     - **Surface Nets 四边形发射几乎全漏**：球面面积测试得到 **292** 而解析值约 **1810**。诊断做法 = 加一个"**水平面应产生 1024 个四边形**"的最小用例，立刻暴露"发射数远小于应有值"。**根因**：取"该 cell 下侧棱"的两个端点时，角的位编号应为 `1 << axis`（**位移后的位**），而代码里误写成 `1 << axis` 的**值**参与掩码比较（等价于读错角）⇒ 绝大多数 cell 判定为"两端同号"而被跳过。修好后平台用例通过、球面面积回到容差内。
     - **`SDL_CreateGPUGraphicsPipeline` 失败 `0x80070057 (E_INVALIDARG)`**：首次怀疑"4 个片元 uniform 槽超过 SDL_gpu 每阶段 4 槽上限"⇒ 改为**独立自发光管线**（新增 `orb.frag` + `m_orbPipeline`），**仍然失败**；再做一版**零 uniform** 的 `orb.frag` 并声明 `uniformBuffers = 0`，**仍失败** —— 至此排除"槽数超限"。**真根因**：SDL_gpu 的 `set = 3` uniform 绑定**必须从 0 连续编号**，且**个数必须与创建 Shader 时声明的 `num_uniform_buffers` 一致**；`mesh.frag` 实际用了 binding 0/1/2/3 四个，创建时却只声明了 **3** 个 ⇒ D3D12 根签名与 SPIR-V 不符。**修法**：回退到单管线方案（删除 `orb.frag` 与 `m_orbPipeline` / `m_orbFragmentShader`），把声明数改为 **4**，自发光颜色走 `mesh.frag` 片元 uniform **槽 3**（`DrawMeshes` 按网格推送）。结论已写进 `mesh.frag` 顶部注释、`mesh_renderer.hpp`、`engine-capabilities.md` 与阶段计划。
     - **另有两处口径修正**（非 BUG，是断言 / 数值口径写错，已在同一轮纠正）：`DigRegion.SealedOverridesLowerPriorityDiggable` 的期望算式须为 **4×1×4 = 16**（原写成 63）；弹道单测容差 1e-6 对 `float` 累加 60 步过严（实测误差 4.7e-6）⇒ 放宽到 1e-4 并在注释写明"离散解析解本身也有累加误差"。
  6. 文件逐字节校验：**纯 LF、无 BOM**；`toml++` 只在 `.cpp`。
- 下一步 / 遗留：① **观感与手感待人工目视验收**（本环境无截图能力）：站在出生点朝西南的山体（x 6~26 / z -56~-36）**左键连发**，期望山壁被掏出**三维洞体**（而不是只削平高度）；朝平地射击看弹坑半径 6 格 / 深 6 / 外环 2 是否平滑；F1 面板看「光球（活动 / 上限）」「可挖体积块（含已挖数）」。② **最大遗留 = 体积物理碰撞**（洞口可见但**走不进去**），已登记为下一步任务。③ **已知限制**：体积与地表在**区域边界一圈共面重叠**（平面地形上两者逐字重合，理论上可能轻微 z-fighting），故区域边界须选在玩家不细看处；彻底无缝见 ADR 0011「何时重新审视」。④ **体积尚无流式加载 / 存档**，且爆炸**不作用于物件层与实体**。⑤ `[[projectile]]` 已是数组、`OrbPool` 与 `ProjectileSpec` 已是多类型友好的形态，**加第二种弹丸不需要改架构**。⑥ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。⑦ 本批改动**尚未提交**。

## 2026-09-27  可挖区域由"一处山体"扩为**整张测试地图**（人工反馈："只有新增的那一柱可以挖，希望整张 demo 地面都能挖"）

- 做了什么：
  1. **`assets/config/dig_regions.toml` 重写**：由 1 个只圈住山体的区域（`test_hill_flank`，块 1 × 4 × 1 = 4 块）改为**覆盖已加载世界全部地表列与相关高度**的单区域 `test_world_all` —— `min = [-64, 0, -64]`、`max = [127, 319, 127]`，按 32 格块**向外吸附**后为块 `x ∈ [-2, 3]`、`y ∈ [0, 9]`、`z ∈ [-2, 3]`，即世界 `x ∈ [-64, 128)`、`y ∈ [0, 320)`、`z ∈ [-64, 128)` ⇒ **360 块**（密度数据约 **12.34 MB**，在 `kMaxTotalBlocks = 512` 上限内）。文件头补上"为什么必须铺满""代价""与地图 `tile_radius` 的耦合"三段说明。
  2. **测试同步**（`tests/dig_volume_test.cpp`）：`DigRegion.LoadsShippedTableAndToleratesMissingFile` 断言的区域名 / 块范围 / 块数按新配置更新（`test_world_all`、块 `[-2,0,-2]`~`[3,9,3]`、360 块）；"文件缺失返回空表"这条断言未动。
  3. **地图注释同步**（`assets/maps/test_range.toml`）：`dig_hill` 的注释由"可挖体积的**唯一**载体"改为"保留为一处**高而薄的竖直壁面**，打它最容易看出掏出的是三维洞体"。
  4. **阶段计划同步**：T8 行的落点与 §3「当前进度」按实测数据更新（360 块 / 12.34 MB / 118 块有等值面 / 82 块已上传），并把"debug 下启动期 SDF 初始化约 +2.7 秒"作为代价写明。
- 为什么：
  1. **需求受理**：人工反馈"只有新增的那一柱（即原区域那根 32×128×32 的"块柱"）可以挖，希望整张 demo 地面都能挖"。判定为**不矛盾的更优做法** ⇒ 直接执行，未提问：可挖范围本来就是一个纯数据（`dig_regions.toml`）问题，不涉及架构选择。
  2. **为什么"铺满"是必要而非过度**（这是本轮唯一需要论证的点）：ADR 0004 硬约束 2 规定**区域外不可三维挖掘**，而 ADR 0011 的层间交接是"**四角全部可挖**的地表四边形才交给体积网格"。两条合起来意味着：**区域不完整 ⇒ 区域外那部分地表仍由地表网格绘制、且打不穿**，玩家会看到"有的地面能挖、有的不能"的割裂感 —— 与"整套地表都是可挖材质"的目标直接相违。**顺带收益**：区域边界那一圈"地表 + 体积共面重叠"（ADR 0011 的已知限制）被推到**世界边缘**，玩家通常看不到。
  3. **3A 基线对照**：① **业界参照**：全可挖体素的游戏（《Teardown》《Deep Rock Galactic》《雾锁王国 / Enshrouded》）里地形**整体**由体素 / SDF 表示，不存在"部分地表可挖"的设定 —— 本改动的方向与之相同；反例是"只给若干建筑 / 矿脉加可破坏属性"的做法（如若干射击游戏的局部可破坏掩体），那是**故意**保留不可挖地形的设计，与本 demo"整体可破坏"的目标不符。② **本项目可判定判据**：装载后的**块数 = 6 × 10 × 6 = 360**（单测断言）；启动日志的块范围与密度字节数与配置注释**逐项一致**；修改前后的**地形瓦片网格**从"有面"变为"无面"（由 `SkipQuad` 的四角判定保证，机制已有单测）。③ **降级 / 代价**：见下条。
  4. **代价三点（显式记录）**：**程度** —— ① 整张地表改由 **Surface Nets** 绘制，Surface Nets 不保留锐边（ADR 0007 已记载），故**1 格台阶 / 陡坎的法线会比地表网格"软"**（几何位置仍在 1 格精度内）；② debug 下启动期 SDF 初始化由约 0.5 秒增至约 **3.2 秒**（360 块的采样 + 网格化，约 **+2.7 秒**）；③ 可见体积块由 4 增至 **82**（Draw Call 随之增加，本轮"只记录不验收"）。**原因** —— ①② 是"用整套地表换可挖性"的固有代价：要让任意位置都能挖，就必须让那一处由体积表示，而体积的表示方式是等值面；启动开销与块数成正比，360 块 = 12.34 MB 密度 + 每块 32³ cell 的网格化。**备注 / 切换条件** —— 想收窄只需改 `dig_regions.toml` 的 `min` / `max` 一行（例如只保 `[-64, 64]` 的测试区 ⇒ 160 块、约 5.5 MB）；启动开销的优化方向是"**对全实心 / 全空的块跳过 cell 遍历**"（未实现，属可选优化，不是本轮范围）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **226/226 passed**（更新后的 `DigRegion.LoadsShippedTableAndToleratesMissingFile` 通过，证明 360 块与块范围吸附与配置注释一致）。
  3. **运行期冒烟**（前台启动 `build\debug\bin\voxel_game.exe`，持续 > 35 秒仍在运行、**无 SDL 断言、无 ERROR / WARN**），关键日志：
     - `可挖区域表已加载（schema_version=1）：1 个区域，共 360 个体积块（32³，密度数据约 12.66 MB）`
     - `区域 [test_world_all]（diggable，优先级 10）：块 x∈[-2,3] y∈[0,9] z∈[-2,3] ⇒ 世界 x∈[-64,128) y∈[0,320) z∈[-64,128)（min/max 已**向外吸附**到 32 格块边界）`
     - `可挖体积就绪：360 个块（密度 12.34 MB），其中 118 块存在等值面；**区域内已由体积接管地表网格**（ADR 0011）`（耗时 0.532 → 3.246 秒）
     - `可挖体积网格已上传：82/360 个块有可见表面（其余块全实心或全空，无等值面）`
  4. 文件逐字节校验：**纯 LF、无 BOM**。
- 下一步 / 遗留：① **待人工目视验收**（本环境无截图能力）：**任意位置**朝地面 / 山壁左键连发，期望**处处**都能掏出三维洞体，而不是只有西南那根山柱能挖；同时看 **1 格台阶与阶梯坡道的观感**是否因改由 Surface Nets 绘制而明显变软（这是本轮已知代价，若不可接受需另立任务：让层间交接保留锐边，或在台阶区用 `sealed` 排除）。② **启动开销 +2.7 秒（debug）**：可选优化 = 全实心 / 全空块跳过 cell 遍历，未做。③ 体积物理碰撞、流式加载 / 存档、ADR 0006 程序化规则**均未做**（同前一条 devlog 的遗留）。④ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。⑤ 本批改动**尚未提交**。

## 2026-09-27  规范升级（M7）：降级**必须先问**；新增「世界内一致性」两关 —— 玩法改动不得只做字面要求

- 做了什么（改的是**规范**，`.trae/skills/voxel-engine-dev-standards/SKILL.md`）：
  1. **「3A 基线」节新增第一硬规则 —— 降级必须先问**：凡做出"达不到 3A 基线 / 本阶段不做 / 先做简版顶着"的判断，
     **不得**自行降级、事后只在 devlog 补三点说明就算闭环；必须**在动手前**把「**做**（完整实现，代价）/ **不做**（世界或体验残缺在哪、何时必须补）/
     **先做最小版**（首期到什么程度、后续补什么）」三选项与**建议 + 理由**摆出来，**等确认后再动手**；
     **未经确认的降级不算已登记、也不算闭环**。两条例外明确写出：① 属「需求受理」第一步的"不矛盾的更优做法"（那是加强、不是降级）；
     ② 项目所有者本次已明确说"可以先这样 / 后续再完善"（视为已确认，但仍须登记待办 + 切换条件）。
  2. **新增「世界内一致性」节（动手前的第 4 道审查）**：任何**玩法 / 内容**改动**不得只实现"用户明确说出的那一句"**，
     必须先自问"**这件事在真实世界里会连带发生什么？在修仙世界里会连带发生什么？**"，凡"不做就会让世界明显不自洽"的连带后果
     **算作本需求的一部分**。两关都必须过：**真实世界**（物理与常识自洽 —— 通道可通行、结构有支撑、重力 / 坠落 / 水 / 火按预期）与
     **修仙世界**（与 `docs/world-setting.md` 自洽 —— 灵气 / 法术 / 门派 / 异象如何改写物理）。附加内容分三级处理：
     **合理且不矛盾 ⇒ 直接做**（回复中对照"你要的效果 → 我们补了什么 → 依据"）；**可疑或代价大 ⇒ 提问**（做 / 不做 / 先做最小版 + 建议）；
     **需要新的世界观解释 ⇒ 提问**（六.9：设定只能来自项目所有者，AI 不得代拟）。**禁止**：只做字面要求、悄悄降级为「遗留」、借"世界一致性"之名搞范围膨胀。
     **允许分期，但 ① 分期方案要先问；② 首期必须做到"用户可感知的自洽"**（"具体逻辑后续再完善"只在首期已自洽的前提下成立）。
  3. **同步改造（口径不与旧条文冲突）**：适用范围新增"玩法 / 内容语义的改动"，并把"不适用"由"纯玩法数值调整"收紧为"**纯玩法数值微调**（不引入新语义、无连带后果）"；
     **第五节「范围控制」加"分界"**——§5 只管技术与引擎范围，本节只管玩法与内容的完备性，**互为硬边界**（本节不构成引入暂缓项的理由；§5 也不构成"把世界不自洽当范围之外"的理由）；
     六.5 总则加第 9 条（连带后果**先登记后动手**，降级 / 分期**须先确认**）；六.7 触发条件加"发现世界内连带后果"一行；
     六.9 触发条件加"玩法改动引入新的世界内解释 ⇒ 先登记、且**一律先提问**"一行；**DoD 改写 1 项 + 新增 1 项**；frontmatter `description` 补上这两条要求。
  4. **按新规立刻登记了本轮识别出的两个连带必做项**（`docs/game-design.md` §2.3 能力清单，状态 `未开始`）：
     **「进入可挖体积（洞口可通行）」**（洞是通道 ⇒ 角色必须能走进去：需体积物理 + 角色碰撞从"每 tile 一张高度场"改造为"高度场 + 区域体积" + 挖除后重建物理体，且依赖体积存档）
     与 **「结构塌陷（支撑缺失 ⇒ 上部倒塌）」**（首期 = 可感知的自洽"塌落成堆体 + 废墟"，完整受力与逐块倒塌后续分期）；阶段计划 §3「下一步」第 2 条与 M7 条目同步。
- 为什么：**你给的两条要求针对的正是本轮的真实失误** —— 上一轮我把"洞口走不进去"与"结构不会塌"这两类**连带后果**
  按"用户没提"处理，自行降级并只在 devlog 里留档。按新规，这两件事都**不该由我单方面决定**：
  前者应**提问或直接做**，后者至少应**在动手前把"做 / 不做 / 先做最小版"摆给你确认**。这两条规则的地位由此改变：
  ① **降级**从"事后留档"提升为"**事前确认**"；② **需求边界**从"用户说了什么"扩展为"**用户要的效果在世界里自洽所必需的一切**"，
  且把"不确定"的默认动作由"降级"改为"**提问**"。
- 验证：
  1. **规则成文且可勾选**：两条新规则都带**来源（2026-09-27 你的原话）**与**本项目真实案例**（"给山挖洞 ⇒ 洞要能进去"、"炸掉大楼一层 ⇒ 上半截要塌"），并已进 DoD 自检项；
     冲突面已显式处理（§5 分界 + 六.5 第 9 条），不存在"两条并存的有效口径"。
  2. **口径漂移自查**：全仓库检索确认没有与新规相反的旧措辞残留（旧条"不适用：纯玩法数值调整"已收紧，旧条"登记为后续任务"式措辞已改为"连带必做项 / 须先确认"）。
  3. **按新规当场执行**：① 两个连带必做项已**先登记**（`game-design.md`）再谈实现；② 它们的**范围与分期**在本次回复中以选项形式**向你确认**，不再自行降级。
  4. 文件逐字节校验：**纯 LF、无 BOM**。

## 2026-09-27  T28 碰撞接管（洞口可通行）+ T29 体积内塌落 —— 两条"世界内一致性"连带必做项落地

- 做了什么：
  1. **新增 [ADR 0012](adr/0012-collision-takeover-by-volumes.md)**：把 ADR 0011 只管**渲染**、不管**碰撞**的缺口补齐 —— **"谁来画"与"谁来挡"必须用同一份判据**（同一份 `DigRegionTable`），否则就会重现"洞口看得见、走不进去"。
  2. **T28 通用三角网静态碰撞体**（`engine/physics/physics_world.*`）：新增 `MeshDesc`（**局部坐标 + `double` 原点**，红线 6）、`AddMesh`、`UpdateMesh`；Jolt `MeshShape` 类型仍只出现在 `.cpp`；构造前**逐项校验**（空指针 / 0 三角形 / 索引越界 ⇒ 返回无效句柄并记日志）。
  3. **T28 `world/dig/volume_collision.*`**：与 `TerrainCollision` 同构的胶水层 —— 每个**有等值面**的体积块一个静态体，挖除 / 塌落后按脏块 `SetShape` 重建（网格变空则**移除**该体，避免留下隐形障碍）。
  4. **T28 `game/main.cpp` 接管判据**：**"该 tile 的地表网格已经没有任何面"** ⇒ 不为它建地表高度场碰撞体。这条判据与 ADR 0011 的四边形跳过**同源**（同一次 `BuildTerrainMesh`），因此不可能出现"渲染交给体积、碰撞却留在高度场"的漂移；地表爆破路径重建 tile 碰撞体时同样跳过被接管的 tile。
  5. **T29 `world/dig/volume_collapse.*` + `assets/config/collapse.toml` + `world/dig/collapse_table.*`**：爆炸挖除后做**一次**支撑检查 —— 支撑 = 「**载荷通路**（自区域底面沿本列连续实心 ⇒ 接地）+ **悬挑容差**（同层实心连通横向传播 `max_cantilever_blocks` 步）」；失去支撑的实心体**按连续段**沿本列下落到腔底（**质量守恒**）、堆顶按**确定性哈希**摊给相邻列成碎石。`DigVolumeWorld` 相应新增体素级 `ReadDensityRegion` / `WriteDensityRegion`（**含共享边界样本**，故块间不会出现缝隙）。
  6. **可挖区域扩到世界边缘的共享边界列**（`assets/config/dig_regions.toml`：`max` 由 `[127, 319, 127]` 改为 `[128, 287, 128]`，360 → **441 块**）：tile 覆盖世界列 `[-64, 128]`（**含两端**），区域若止于 `127`，每块 tile 最外一圈四边形仍会由地表绘制 ⇒ 该 tile 保留高度场碰撞 ⇒ "洞口可见但走不进去"重现。这是**做 T28 时才暴露出来的耦合**，已写进 ADR 0012 的「已知限制」。
  7. **可观测性**：启动日志新增「碰撞接管」一行（地表高度场体数 / 交出的 tile 数 / 体积体数）、「塌落规则已加载」一行；F1 面板新增 **「体积碰撞体」「累计塌落体素」** 两行（中英两表同步）；并在启动 2 秒后打一行 **「角色落地自检」**（脚底高度 / 是否着地）—— 否则"角色掉进地下"这类失败只会表现为画面异常。
- 为什么：
  1. **这是你 2026-09-27 的两条要求直接驱动的**：① 「降级必须先问」⇒ 我把「洞口可通行」与「结构塌陷」的范围与分期**先问后做**（你选：洞口可通行**只要"能走进去站在腔底"、暂不做体积存档**；塌陷**先做"体积内塌落"**）；② 「世界内一致性」⇒ "洞是通道""承重缺失就应塌"**属于需求的一部分**，不能按"你没提"处理。
  2. **3A 基线对照**：① **业界参照**：可破坏地形游戏的**碰撞**一律跟渲染同源 —— 《Teardown》用体素场同时驱动渲染与物理、《Deep Rock Galactic》用体素 + 破坏后重建碰撞、Unreal 的 Geometry Collection / Chaos 破坏体会**重建物理体**；本项目与之同构（Surface Nets 网格 → Jolt `MeshShape`，挖除后重建）。**塌落**这一侧，业界做法是**刚体碎块 + 受力模拟**（Unreal Chaos、Blender 的 rigid body fracture），本项目首期取"**载荷通路 + 悬挑跨度**"的体素级近似，属**显式降级**。② **本项目可判定判据**：角色停在**三角网面**上（单测：脚底 y = 面高 ±0.1、`onGround`、全程不穿透）；接管后**地表高度场碰撞体 = 0**；塌落**质量守恒**（实心体素数前后不变）、**稳定面不塌**（平地 0 移动）、**失去支撑必塌**（悬空石板整体落到腔底）。③ **降级**：见下条。
  3. **降级三点（显式记录）**：**程度** —— 塌落是**体素级近似**：单次判定**不迭代**（下落一层后新露出的悬空不再复核）、**无安息角 / 无碎块刚体**（堆面只做确定性摊开）、**不区分材质强度**；且**没有建造层**，故只能塌"体积里的岩体"（轰断立柱根部 ⇒ 上段整段落下），**还不能塌玩家盖的楼**。**原因** —— 完整受力 / 连锁需要把塌落区切成刚体碎块并接 Jolt 求解，是独立的一大块；而"玩家盖的楼"依赖 ADR 0004 层 ③（建造层）**尚未建立**。**备注 / 切换条件** —— 建造层落地后把判据换成"结构受力 / 连接性"并在 `collapse.toml` 里区分材质强度；连锁与安息角列为后续项（ADR 0012「后果」）。
  4. **记录一条被否决的设计（值得留档）**：T29 首版规则是"**只看向下一格是否实心**"（悬空结构的最底一层算失支撑）。实现后发现它**只掉一层**：薄板只掉一半、厚岩只掉表皮 —— 因为第二层立刻"站在刚掉下去的那一层上"。改用"**载荷通路 + 悬挑容差**"后，"薄板整体掉、立柱根部被轰断则上段整体落"才成立。该替代方案已写进 ADR 0012 的备选对比。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **235/235 passed**（基线 226 → **+9**：三角网体校验 / 重建 / 移除、**角色停在三角网面上**、`VolumeCollision` 同步与去重 → 4 项；塌落 5 项：平地稳定 / 悬空石板整体下落 + 质量守恒 + 堆在腔底 / 摊开守恒 / 关闭不动 / 配置表与默认值一致）。
   3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 108 file(s), 0 violation(s)`、`PASS`、退出码 0。
  4. **运行期冒烟**（前台启动，无 SDL 断言、无 ERROR / WARN），关键日志：
     - `可挖区域表已加载：… 共 441 个体积块（32³，密度数据约 15.50 MB）`、`区域 [test_world_all]… 世界 x∈[-64,160) y∈[0,288) z∈[-64,160)`
     - `塌落规则已加载（schema_version=1）：启用；悬挑上限 4.0 格…碎堆摊开 1 格；支撑检查邻域外扩 1 块`
     - `碰撞接管（ADR 0012）：地表高度场碰撞体 0 个；9/9 个 tile 的可见面已全由体积绘制（其高度场碰撞体已交出）；可挖体积三角网碰撞体 137 个`
     - `角色落地自检（T28 碰撞接管后）：脚底 (0.00, 120.00, -0.00)，着地=是` —— **角色站在体积面上，高度与体积表面一致**
     - `可挖体积网格已上传：137/441 个块有可见表面`
  5. 文件逐字节校验：**纯 LF、无 BOM**。
- 下一步 / 遗留：① **待人工目视验收**：⑨ **走进洞里并站在腔底**（不再被隐形高度场挡住）；⑩ **把立柱 / 山体根部轰断 ⇒ 上段整段落下并在腔底堆成碎石**。② **体积存档**（你 2026-09-27 明确"暂时不要求"）：不做则**重进游戏洞与碰撞体会一起消失**。③ **已知限制**：**部分覆盖**的 tile（区域边界横穿）仍保留高度场 ⇒ 该 tile 内的洞进不去；**启动开销** debug 下到主循环约 **8.2 秒**（441 块 SDF + 137 个体），优化方向 = 按需 / 就近建碰撞体与分块流式。④ **塌落**的连锁复核、安息角 / 碎块刚体、材质强度差异未做。⑤ **关于"要不要把高度场整体废弃"**（你的提问）：本轮**没有**动 ADR 0004 的分层表示；做完 T28 之后**渲染与碰撞的权威都已经在体积上**，高度场退为"**体积的初始化数据源**"（`InitFromHeightField`）与"区域外的兜底"。**要真正删掉高度场，前置条件是 ADR 0006 的「程序化规则」**（噪声直接产出三维密度）—— 否则体积没有数据来源。这属于**技术选型变更**，须按 SKILL §2.4 走 ADR + 四步流程后再动。⑥ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。⑦ 本批改动**尚未提交**。
- 下一步 / 遗留：① **等确认**：是否现在就做「进入可挖体积」（最影响自洽性）与「结构塌陷」首期，以及分期边界（见本次回复的提问）。
  ② **`docs/world-setting.md` 的相关条目仍是 `待补`**："被灵力打穿的山体是否渗灵 / 是否引动异象""塌陷是否伴生异象"等**只能由项目所有者提供**，
  在它落定前，连带内容一律**按"真实世界"这一关实现**并在文档里标注"修仙世界观解释待补"。③ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。④ 本批改动**尚未提交**。

---

## 2026-09-27  T30 定位并修掉"光球爆炸改变地形时的明显卡顿"（单次爆炸 196 → 39 ms，debug）

- 做了什么：
  1. **先量再改（分段计时）**：`Detonate` 的区域内分支改为用 `PhaseTimer` 分**六段**测 —— 挖除 / 网格化 / 塌落 / 塌落后网格化 / 网格上传 / 碰撞体重建，并**每次爆炸打一条 `VX_LOG_INFO`「爆炸耗时分解」**（含中心与半径、塌落邻域的采样数、失去支撑与移动的体素数、脏块数、各段与合计毫秒）。日志级别取 `Info` 而不是 `Debug`：它正是 T30 的交付物（下一次卡顿报告要能直接读数）。
  2. **两个可复跑的基准用例**（只打印、不断言时间，避免 CI 上产生 flaky 断言）：`tests/volume_collapse_test.cpp` 的 `VolumeCollapse.ProfileGameScaleExplosion` 按**真实玩法的规模**建场景（真实半径 6 格会波及的 3×9×3 块体积；球心在地表下 8 格 = "从山体侧面射入"；种子取 `CarveSphere` 的真实改动范围），跑 3 次取平均；`tests/volume_collision_test.cpp` 的 `VolumeCollision.ProfileBlockShapeRebuild` 量**单块三角网碰撞体的重建**（2649 顶点 / 2048 三角形 × 20 次）。
  3. **优化 ①（塌落邻域，语义不变）**：
     - `DigVolumeWorld` 新增每块 `BlockFill`（`Solid` / `Air` / `Mixed`，在 `RemeshDirtyBlocks`、`CarveSphere`、`WriteDensityRegion` 三处与密度同步维护；不存在的块按 `Air` 处理，与 `ReadDensityRegion` 的填充口径一致）⇒ 竖直范围从"整个体积的块范围"收紧为"向下到**最低的非全实心块**、向上到**最高的非全空块**"。
     - `CarveSphere` 新增出参 `VoxelBounds*`（被改动采样的世界 AABB = 球 + 过渡带的 AABB），`ApplyCollapse` 的入参从"被挖块列表"换成 `CollapseSeed`（那个 AABB）⇒ 水平范围从"被挖块 ± 1 块（32 格）"收紧为"**被改动采样范围 ± (悬挑 + 摊开 + 1) 格**"。`assets/config/collapse.toml` 的 `neighborhood_margin_blocks` 默认由 1 改为 0（语义变为"在派生范围之外再额外外扩的块数"）。
     - 顺带把 BFS 队列的预留从 `N/8` 改成 `N/2`，免掉多次扩容带来的整段拷贝。
  4. **优化 ②（碰撞体构建质量）**：`engine/physics/physics_world.cpp` 的 `MeshShapeSettings` 设 `mBuildQuality = FavorBuildSpeed`。理由：可挖块的形状**每次挖除 / 塌落都要整块重建**（Jolt 的 `MeshShape` 不可变），构建速度远比查询重要；静态地形查询的余量很大。（顺带试过 `mMaxTrianglesPerLeaf = 16`，被 Jolt 以 `Invalid max triangles per leaf` 拒绝 —— 上限即默认值 8，未采用。）
- 为什么：
  1. **数据（debug）**：优化前 —— **基准（新挖处）** = 挖除 0.22 + 网格化 6.38 + **塌落 173.81** + 塌落后网格化 0.02 ≈ **180 ms**，再加碰撞体重建 **16.03 ms/块**；**真实连射同一片山体（冒烟日志）最坏一次 281 ms**（塌落 174.09、邻域 **374.4 万**采样、网格化 42.8、碰撞体重建 52.6、上传 10.9，波及 8 块）—— 一段 16.7 ms 的固定步里塞进 17 帧的活，这就是"明显卡顿"。**关键事实：那些塌落的毫秒数里有 0 个体素移动** —— `ApplyCollapse` 在"本次根本不塌"的常态下也要全额付掉。
  2. **塌落为什么这么贵**：竖直邻域取整个体积（9 块 × 32 格 = 289 层）时，每次爆炸都要 ① 把整段地下实心复制进 `DensityRegion`、② 把上百万个"接地"体素**全部**塞进 BFS 队列（接地判据是"从底面起连续实心"，地下那几层全中招）、③ 再逐层扩散 4 邻域；而挖一个地表下的洞**本来只可能影响那一块**。真实连射场景还更糟，说明**只做竖直收紧不够**：① 水平仍按"被挖块 ± 1 块"，而半径 6 格的爆炸常跨 2×2 块 ⇒ 4×4 块 = 129×129 格；② 竖直的"最高非全空块"是在**整个水平范围**上取 max，于是范围里几十格外的山体 / 地标塔把上界一路抬到体积顶 ⇒ 129×129×225 ≈ **374 万**采样，比基准场景还多。两条合起来的教训：邻域必须围绕**被改动的采样**（而不是"被挖的整块"）来定。
  3. **为什么收紧是等价而不是降级**：
     - **竖直**：被排除的两种块 —— **整块实心**（内部没有空腔可供落点，且其上实心仍接在连续实心段上、不会失去支撑）与**整块空**（内部没有实心，不可能有东西塌）—— 都不参与判定；被排除段的紧邻侧保证"邻域底面之下全是整块实心 ⇒ 底面可等价视作地面"，与"从体积底面起算"逐体素同结论。
     - **水平**：判定只涉及三类位移 —— 纵向载荷通路（**同列**）、同层横向传播（≤ `max_cantilever_blocks` 步）、堆面摊开（≤ `pile_spread_blocks` 列，且要求目标列**已有本次的堆**）；故外扩 (悬挑 + 摊开 + 1) 格之外不可能被本次挖除影响。
     - **回归证据**：T29 的 5 项不变量测试（平地不塌 / 悬空石板整体下落 + 质量守恒 + 堆在腔底 / 摊开守恒 / 关闭不动）改后**全部照旧通过**。
  4. **3A 基线对照**：① **业界参照**：可破坏地形的性能热点从来是"**碰撞体重建**"与"**破坏后的物理/网格更新**"（《Teardown》把体素→碰撞体的重建摊到多帧与多线程；Unreal Chaos 的 Geometry Collection 用分级 LOD 与后台构建）。本项目当前是**单线程、当帧完成**（这是 T28"洞口立即能走进去"的直接代价），故先把**每段的常数**压下来，而不是引入异步 —— 异步重建属**新增架构**，须按 SKILL §2.4 走 ADR，未在本轮动。② **本项目可判定判据**：分段计时可在日志逐次读到；基准用例可复跑出前后数字；T29/T28 的既有不变量全绿。③ **降级**：无新增降级 —— 塌落判定结论与 T29 逐体素一致；碰撞体只是**构建质量**取舍（叶子树按"建得快"构，运行期查询稍慢，静态地形余量充足）。
- 验证：
  1. **基准（debug，同一台机、同一用例）**：
     - 塌落：邻域 **2719201 → 17457** 采样（-99.4%）；`VolumeCollapse.ProfileGameScaleExplosion` **塌落 173.81 → 1.93 ms**（-99%）、四段合计 **180.4 → 7.8 ms**。
     - 碰撞体：`VolumeCollision.ProfileBlockShapeRebuild` **16.03 → 7.84 ms/块**（-51%）。
  2. **真实连射同一片山体（冒烟日志，同一玩法前后对比）**：
     - 最坏一次（爆炸刚好落在块角、波及 **8 块**）：**281.37 → 71.14 ms**；塌落 **174.09 → 3.95 ms**、邻域 **3744225 → 54665**；网格化 42.82 → 21.78、碰撞体重建 52.55 → 37.10、上传 10.88 → 7.76（这三项是"每脏块"成本 × 块数）。
     - 波及 1 块的一次：**44.25 → 24.36 ms**；塌落 26.74 → 4.02 ms。
     - **结论**：塌落（原先占 60~90%）**已不再是瓶颈**；现在的瓶颈是**每脏块**的"三角网碰撞体重建 + 网格化 + GPU 上传"（debug 下 ≈ 16 ms/块）。
  3. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + `/WX`）。
  4. **测试**：`ctest --preset debug` → **237/237 passed**（基线 235 + 新增 2 项 T30 基准）。
  5. **门禁**：`check-banned-identifiers.ps1 -SelfTest` → `Self-test passed: 18 case(s)`；`-RepoRoot .` → `scanned 108 file(s), 0 violation(s)`，`PASS`，退出码 0。
- 下一步 / 遗留：① **待人工目视复验**：连射光球时的卡顿是否已消失（现在每次爆炸都会在日志里留下各段毫秒数，可直接贴回来）。② **仍有优化空间（已登记）**：瓶颈已是"每脏块"的三件事（三角网碰撞体重建 ≈ 9 ms/块、Surface Nets 网格化 ≈ 5.4 ms/块、GPU 网格上传 ≈ 1.9 ms/块，均 debug）——炸在块角上会波及 8 块 ⇒ 约 70 ms。方向：⑴ GPU 网格 buffer 复用（现在是 `ReleaseMesh` + `UploadMesh`，即销毁再重建）；⑵ Jolt 侧再看 `mActiveEdgeCosThresholdAngle` 等构建质量开关（需连带 `EnhancedInternalEdgeRemoval`，有 ghost collision 风险，未动）；⑶ **把"非中心块"的网格化 / 碰撞体重建 / 上传分摊到后续 1~2 帧** —— 这会改变 T28 的"洞口当帧可通行"语义（变成"被挖中心块当帧可通行、其余 1~2 帧内补上"），**需项目所有者确认后再做**。③ **Tracy 未接入**（V0.2 计划），本轮性能数据来自内置 `PhaseTimer` 与两个基准用例。④ 仍未闭环：**I1（CI 红）**、**T19 两处口径**。⑤ 本批改动（含 T28/T29/T30）**尚未提交**。

---

## 2026-09-27  可破坏元素规范（ADR 0013）：材质坚固度 × 伤害预算 + 固定器物几何不可变

- 做了什么：
  1. **先做合理性分析**（项目所有者要求"先分析、合理则写规范"）：确认三条要求整体合理且与业界一致（Minecraft 方块硬度 × 工具倍率 / 7 Days to Die 方块硬度 / UE Chaos 的 **Damage Threshold** / NVIDIA Blast 的 material toughness），但发现**四处口径无法直接施工**，逐条钉死后落盘：① "无法改变外形（无法被破坏）"与"破坏时可以切换预制外观"**互斥** ⇒ 拆成「几何可变性 / 状态可变性」两个正交维度（器物 = 几何不可变 + 状态可变，另设 `destructible` 开关区分"完全不可破坏"）；② "一个体积"**没有单位** ⇒ 钉死为 **格³**；③ 按字面（伤害 10 ÷ 泥 3 = 3 格³）与**现有手感差约 900 倍**（现为 r = 6 格 ≈ 904 格³）⇒ 引入**换算系数**（配置项）保留"伤害 10 / 泥 3 / 岩 5"的直觉数字：默认取"纯泥土 r ≈ 6 格"⇒ 系数 ≈ **271 格³/点**，则纯岩石 ⇒ `V = 542` ⇒ **r ≈ 5.06 格**；④ "变黑"是**表现**不是状态 ⇒ **状态与表现分离**。
  2. **新增 [ADR 0013](adr/0013-destructible-elements.md)**（权威层）：元素分类表、伤害结算规则（`V = 伤害 × 换算系数`，自爆心向外**逐格³ 扣减该格材质的坚固度**、耗尽即停 ⇒ 混合材质"软的先被挖掉"）、不可破坏材质（零挖除且**不扣预算**）、器物规则（几何不变 / `Intact → Broken` / 状态与表现分离 / 本阶段仅"变黑"占位 / 阈值驱动）、配置落点与校验、**6 条备选否决**、后果与切换条件。
  3. **新增执行层规范** `.trae/skills/voxel-engine-dev-standards/references/destructible-elements.md`：归属判据、结算的 4 条硬约束（逐格 / 确定性次序 / 整数点预算 / 不可破坏材质）、器物 4 条、配置与校验、**8 条必测判据**、禁止项速查。
  4. **SKILL.md 三处同步**：任务路由新增一行（可破坏元素 → 新 reference）；**唯一口径表新增「可破坏性模型」**；**DoD 新增一项**（归属正确 / 逐格且确定性 / 状态与表现分离 / 数值全配置驱动、代码无硬编码）。
  5. **内容基线同步**：`game-design.md` 新增需求 **G12** + 能力清单行；`engine-capabilities.md` 新增「可破坏性判定」能力行、并给"物件层"行补上链接；`world-setting.md` 三处（§1.6 口径链接、问 6 的"旋钮"说明、问 9 的器物开关）；`adr/README.md` 加 **0013**；`plans/v0.1.md` 登记 **T31**（规范已完成，**实现与数值待确认**）。
- 为什么：
  1. 这是项目所有者 2026-09-27 指定的方向；按 SKILL「需求受理」先判合理性再施工。**它会改变破坏的核心判据**（固定半径 → 伤害预算），属架构级改动 ⇒ 必须先 ADR（SKILL §2.4 四步流程 + §8 留档义务），本次未动代码。
  2. **为什么不把 `toughness` 塞进材质表的渲染路径**：`materials.toml` 的字段会被打包进片元着色器的 uniform 块，玩法数值进去就是口径漂移 ⇒ 三张表各司其职（材质表出**材质固有属性**、弹丸表出**伤害**、新 `destruction.toml` 出**全局口径**），启动期逐项校验、非法即抛（ADR 0005 口径）。
  3. **为什么必须逐格扣而不是"先算半径再挖球"**：后者在混合材质处**不会**出现"软的先被挖掉、硬的留在原地"，而那正是"不同元素可破坏性不同"最直观的可见效果。
  4. **为什么不现在就实现**：项目所有者明确"暂不改动"；且**两个数值**（换算系数、器物阈值）未定 —— 按 SKILL「降级必须先问」与"实现前先登记"，先落规范再等确认。
- 验证：
  1. **门禁**：`check-banned-identifiers.ps1 -SelfTest` → `Self-test passed: 18 case(s)`；`-RepoRoot .` → `scanned 108 file(s), 0 violation(s)`、`PASS`、退出码 0。
  2. **文件格式**：本轮全部改动文件（含新增的 ADR 与 reference）**纯 LF、无 BOM**（逐字节校验）。
  3. **索引一致性**：`docs/adr/README.md` 的编号连续（0011 → 0012 → **0013**）；交叉链接逐处核对（ADR ↔ reference ↔ `game-design` ↔ `engine-capabilities` ↔ `world-setting` ↔ `plans`）。
  4. **本轮无代码 / 配置改动** ⇒ 构建与测试结果与上一轮一致（**237/237**，见上一条）。
- 下一步 / 遗留：① **实现前需项目所有者确认两个数值**：伤害 → 体积的**换算系数**（默认取"泥土 r ≈ 6 格"⇒ ≈271 格³/点）与**器物破坏阈值**（默认"单发命中即破"）；② 实现内容：`materials.toml`(+`toughness`) / `projectiles.toml`(+`damage`) / 新增 `destruction.toml` / 把 `CarveSphere` 的判据从"半径"改为"逐格扣预算" / 器物的状态与"变黑"占位表现；③ **可破坏建造物（预制碎块 + 接合、真倒塌）**按 ADR 0013「后果」**另开 ADR**；④ 器物正式预制外观（切换条件 = 美术资源就绪）；⑤ 仍未闭环：**I1（CI 红）**、**T19 两处口径**；⑥ 本批改动（含 T28/T29/T30 与本轮文档）**尚未提交**。

---

## 2026-09-27  T32 缺陷修复：进入体积挖出的洞后视角退化为俯视（附第 7 轮另两条的判定与方案）

- 做了什么：
  1. **判真伪（确认为缺陷）**：**契约** —— `references/gameplay-v0.1.md` §3 与 `engine-capabilities.md` 都写明"第三人称相机的避障目的是**不穿地形**"；而"把相机顶到地表之上"改变的是**视线方向**，不是"不穿地形"。**机制链**（每步可指到代码）：① 爆炸在可挖区域内**只改体积密度、不改高度场**（`Detonate` 的区域内分支）；② `engine/render/camera.cpp` 的"离地间隙"安全网用 `ITerrainQuery::QueryHeight`（**地表高度场**）当"无限地板" ⇒ 站在洞里的角色（脚底低于旧地表）被 `eye.y = max(eye.y, 地表高度 + 间隙)` 顶到**旧地表之上**；③ `TerrainWorld::QueryObstruction` 也只看高度场 ⇒ 洞里**必然报告遮挡** ⇒ 跟随距离被压到 `kCameraMinDistance` ⇒ 相机"贴脸 + 抬高" ⇒ **视角固定为俯视**。
  2. **修复（三处，语义与 ADR 0011 / 0012 的「谁来画 / 谁来挡必须同源」一致）**：
     - `ITerrainQuery` 新增 **`IsSolid`**（点查询），文档写明实现**必须包含可挖体积**；
     - `camera.cpp` 的安全网改为"**仅当相机落在实心内**才沿 +Y 以 **0.25 格固定步长**顶出（上限 64 步）后再额外留 `groundClearance`"（固定步长 ⇒ 确定性，红线 7；只依赖一个点查询，不必让世界层再暴露"实心顶面"）；
     - `TerrainWorld::IsSolid` 只按地表高度场（**分层边界**，注释注明）；
     - `game/main.cpp` 新增**组合查询 `GameCameraQuery`**（**区域内以体积为准**、区域外回退地表），并替换**相机渲染**与**瞄准方向**两处查询源；其 `QueryObstruction` 用固定步长 0.5 格采样 ⇒ 洞内不再有"假遮挡"。
  3. **回归测试（修前确实失败）**：`tests/render_camera_test.cpp` 新增 `DoesNotLiftCameraOutOfExcavatedCave` 与桩 `CaveTerrain`（`QueryHeight` 恒报 0 = 高度场"不知道洞"，`IsSolid` 在洞内报"空"）。**把安全网临时回滚成旧实现后该测试 FAILED**（`eye.y` 被抬到 0.2 而非保持 -4.0），恢复修复后通过 —— 证据留档。三个既有桩补 `IsSolid`；`NeverDropsBelowGroundClearance` 的断言按新契约更新为"相机不在实心内 + 顶出后留间隙"。
  4. **另两条只做判定与登记（未动代码）**：
     - **"两座塔要各自成为整体、整体坠落并倾斜旋转"** ⇒ **引擎当前不具备该能力**：现有塌落模型是**逐列独立**（纵向载荷通路 + 悬挑容差），它只能产生"原地垂直下沉"或"局部陷落"，**在原理上**无法产生倾斜 / 旋转。机制解释了两个观察：细塔（`test_range.toml` 的 `landmark_pillar`，3×5 列）被打断后每列各自下落到同一高度 ⇒ **塔只是垂直变矮**；粗塔（`dig_hill`，21×21 列）因球半径 6 格（直径 12 < 21 格宽）**打不穿整个截面** ⇒ 侧壁仍接地 ⇒ 有时根本不塌。⇒ 需要的是**整体刚体化**（= 此前讨论的 B 路线），已登记待确认（能力缺口 + 候选方案）。
     - **"洞内底部是绿色"** ⇒ **确认为缺陷**：材质权重完全由**地表**规则给出（`materials.toml` 的高度带 × 坡度带）。洞底在 y ≈ 110、坡度 0 ⇒ 命中"草"（高度带 [0, 320] × 坡度带 [0, 0.45]）⇒ 绿色。⇒ 正解是让**体积内表面按"它切开的是什么材质"着色**，其前置是**体素级材质**——与 T31 的"材质坚固度"**共用同一前置**。已登记待确认。
- 为什么：
  1. **相机是"谁来挡"的第三个消费者**：渲染与碰撞已经同源（ADR 0011 / 0012），但相机仍在用**过期的数据源**（地表高度场）。凡是"决定相机能不能通过"的判据，都必须与"谁能挡"同源，否则一挖洞就会立刻暴露。
  2. **安全网不能靠"地表高度"兜底**：`QueryHeight` 的语义是"该列地表高度"，它**不包含**"这里已经被挖空了"这一事实。改用点查询 `IsSolid` 后，兜底逻辑与"哪里真有东西"绑定，且**不需要**世界层新增"实心顶面高度"这类专门接口。
  3. **为什么另两条不当场实现**：整体刚体化会改变塌落的**架构**（体素↔刚体双向转换、落定回写、预算与存档前置），洞内材质则需要**体素级材质**这一新数据通道 —— 两者都属"须先开 ADR / 先定数值"的改动，按 SKILL「降级必须先问」与"实现前先登记"，本次先判定 + 登记 + 给方案。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **238/238 passed**（基线 237 + 新增 1 项相机回归）；相机套件 **10/10**。
  3. **修前失败证据**：临时回滚安全网逻辑 → `ThirdPersonCamera.DoesNotLiftCameraOutOfExcavatedCave` **FAILED**；恢复修复 → 通过。
  4. **门禁**：`scanned 108 file(s), 0 violation(s)`、`PASS`。
- 下一步 / 遗留：① **T32 已闭环**（待人工复验：走进洞里，视角应保持水平跟随）；② **待确认的两条**（见本次回复的提问）：**塔的整体性与刚体倒塌**（架构级，需开 ADR；并需定"塔能否被挖穿"——这决定它走"地形体量"还是"器物"）与**洞内表面材质**（前置 = 体素级材质，与 T31 合并做最省）；③ 仍未闭环：**I1（CI 红）**、**T19 两处口径**；④ 本批改动**尚未提交**。

---

## 2026-09-27  第 7 轮另两条的决策落盘：ADR 0014（体素级材质）+ ADR 0015（结构单元与刚体化倒塌）

- 做了什么：
  1. 项目所有者对第 7 轮另两条做出选择：**塔的整体性 → ② 显式结构单元**（**不是**"器物 / 预制件"路线 ⇒ **塔仍必须能被挖穿**）；**洞内材质 → 继承地表材质**。按 SKILL §8「方案与方向的留档义务」，**先落盘再动手**。
  2. **新增 [ADR 0014](adr/0014-voxel-material-index.md)：体素级材质** —— 体积块新增 33³ `uint8` 材质槽位（441 块口径 **+15.5 MB**），来源 = **继承该列地表的 splat 主槽位 + 表层→次表层映射**（`grass` / `sand` ⇒ `dirt`；`rock` / `dirt` 保持）；体积网格走**独立的材质路径**（**不再**参与"高度带 × 坡度带"的 splat）⇒ 一次投入同时解决「洞内表面外观」（T34）与「逐格坚固度」（T31）。备选（几何规则推导 / 显式体素材质数据 / 洞内固定用岩）逐条记明未采纳理由与**切换条件**。
  3. **新增 [ADR 0015](adr/0015-structure-units-and-rigid-collapse.md)：结构单元与刚体化倒塌** —— 体积体素新增 **`structureId`**（`0` = 普通地形；**只对被打标的块分配**，避免约 62 MB 无谓开销）；首期由**地图预设 `[[structure]]` 段**声明（测试地图的两塔各一个单元）；**触发** = "单元内**全部**体素失支撑"（仍用 ADR 0012 的"载荷通路 + 悬挑"逐列判定后**按单元聚合**；只要还有任一列接地就不动作）；**流程** = 抽空该单元的体积体素 → 转 Jolt **凸包动态刚体**（`MeshShape` 因 `MustBeStatic()` 不能做动态体）→ 交给物理求解（**倾斜 / 旋转 / 与地形和其它刚体碰撞**）→ 落定后**体素化回写成地形**（残骸可继续挖、可站上去）；**预算**（活跃单元 ≤ 4 / 单单元体素 ≤ 20000 / 回写每帧 ≤ 1）与确定性口径（固定步、`FavorBuildSpeed`、禁止随机）写进 ADR。**未打标区域仍走 ADR 0012 的逐列塌落** —— 本 ADR 不改普通地形的行为。
  4. `plans/v0.1.md` 的 **T33 / T34 状态**由"待确认"改为"**已确认（方案已定、待开工）**"；`adr/README.md` 增补 **0014 / 0015**（编号连续）；`game-design.md` 两行的状态与方案同步。
- 为什么：
  1. 这两条都属**架构级**（新增一条数据通道；体素 ↔ 刚体双向转换），SKILL 要求"选定技术方案先写 ADR 再动手"，故本轮只落盘、不写实现代码。
  2. 项目所有者选"**显式结构单元**"而非"器物 / 预制件"，含义是**塔必须仍可被挖穿** ⇒ 不能靠"实体天然是整体"，必须让**体素世界自己表达"整体"**（`structureId`）。这也是 ADR 0015 把"隐式连通分量"保留为辅助判据、而非唯一方案的原因。
  3. `ADR 0014` 与 `ADR 0013` 的**共同前置**是同一份"体素级材质"：一次投入解决两件事（洞内外观 + 逐格坚固度），故实施顺序定为 **T31 + T34 合并开工**。
- 验证：`adr/README.md` 编号连续（0012 → 0013 → 0014 → 0015）且 **0013 行未被覆盖**；门禁 `scanned 108 file(s), 0 violation(s)`、`PASS`；两份新增 ADR **纯 LF、无 BOM**；本条已**移正到文末**（首次追加误插到文中，已修正）。**本节无代码改动**；上一节的 T32 修复已单独验证（**238/238**）。
- 下一步 / 遗留：① **T31 + T34 合并开工**（体素级材质为共同前置）：`materials.toml`(+`toughness`) / `projectiles.toml`(+`damage`) / 新增 `destruction.toml` / `dig_volume` 材质数组 + 网格材质槽位 + `mesh.frag` 分支 + **逐格扣预算**的挖除 —— **数值待确认**（换算系数默认 **271**、器物阈值默认 **0 = 单发即破**）；② **T33 开工前需定**：落定判定阈值（建议"速度 < 0.2 格/秒 持续 30 固定步"）、活跃单元上限（建议 4）、**是否允许"倒塌中的结构再被光球打中"**（首期建议**不允许**）；③ T33 若要**跨会话保留**倒塌结果 ⇒ 需先做**体积存档**（已登记的前置项）；④ 仍未闭环：**I1（CI 红）**、**T19 两处口径**；⑤ 本批改动**尚未提交**。

---

## 2026-09-27  T34 落地：洞内表面按"被切开的材质"着色（ADR 0014 实现）

- 做了什么：
  1. **材质来源 = 纯函数（不落存储）**：`TerrainWorld::QueryDigMaterialSlot(x, z)` = 该列地表 splat **主槽位**（坡度用中心差分，与地表着色同口径）经 `MaterialLayer::subsurfaceSlot` 的**表层 → 次表层**映射；`DigVolumeWorld::SampleMaterialSlot` 委托它（`worldY` 保留在签名里供将来按深度分层）。**实现期修正了 ADR 0014**：初稿计划给每块存 33³ 材质数组（441 块 ≈ +15.5 MB），但"首期不做深度分层 + 挖除不删材质 + 塌落不搬材质"⇒ 任意高度的材质**恒等于**该列材质 ⇒ 存储冗余 ⇒ 改为纯函数、**零额外内存**，并**直接消灭**了"塌落要不要搬材质 / `DensityRegion` 要不要带材质"这一整类一致性问题。
  2. **着色通路（顶点携带槽位覆盖）**：`MeshVertex` 新增 `float material`（`kNoMaterialOverride = -1`，地表网格填它 ⇒ 完全不受影响）；`IVolumeSampler::SampleMaterial` 新增为**非纯虚默认实现**（返回"未指定"）⇒ 既有测试桩与离线调用方**零改动**；`BuildVolumeMesh` 取该 cell **实体侧**（密度 < 0）各角材质的**众数**写入顶点；顶点属性加 `location 2`；`mesh.vert` / `mesh.frag` 用 **`flat`** 传递（离散属性不插值），片元遇槽位 `>= 0` 时**直接令该槽位权重为 1**，否则照旧 `computeWeights()`。
  3. **配置**：`materials.toml` 每层新增**可选** `subsurface`（草 / 沙 ⇒ 土；岩 / 土 ⇒ 自身）。因为是可选字段（缺省 = 自身、旧文件照常加载），**`schema_version` 保持 4**（与项目"同一版本内落地、不重复升版"的先例一致）；`LoadFromFile` 用两阶段解析（先收集名字、全部层解析完再映射为槽位号），未知名字**抛异常**、不静默回退。
- 为什么：材质槽位必须**逐面**给出（同一个块内不同列可能材质不同 ⇒ 不能用逐网格的 uniform 覆盖），故走顶点属性 + `flat` —— 这是既"逐面精确"又不污染地表路径的唯一做法。
- 验证：
  1. **构建**：零错误零警告（`/W4` + `/WX`）。
  2. **测试**：`ctest` **241/241**；新增 3 项 —— `VolumeMesher.VerticesCarrySolidSideMaterialSlot`（顶点必须携带实体侧槽位）、`VolumeMesher.VerticesFallBackToNoMaterialOverride`（未指定 ⇒ 回落为按高度/坡度算）、`DigVolume.MaterialSlotInheritsSurfaceThenMapsToSubsurface`（**草地 ⇒ 主槽位草 ⇒ 映射后为土**，钉住人工实测第 7 轮的缺陷）。
  3. **门禁**：`scanned 108 file(s), 0 violation(s)`、`PASS`。
  4. **冒烟**：启动无 shader / SDL 报错（`flat` 与材质覆盖分支在 DXIL 路径可用）、137 个体积块上传、角色正常落地（脚底 120.00，着地=是）。
- 下一步 / 遗留：
  1. **T31 的坚固度 / 伤害预算尚未实现**（`materials.toml` 的 `toughness`、`projectiles.toml` 的 `damage`、新增 `destruction.toml`、`CarveSphere` 改为**逐格扣预算**）—— 其**共同前置「体素级材质」已随本次完成**；**器物部分**仍待层 ③。
  2. **T33（结构单元与刚体化倒塌）待开工**（方案见 ADR 0015；需先定落定阈值 / 活跃单元上限 / 是否允许二次打击）。
  3. **人工复验**：往地面或山体射光球挖洞，内壁与洞底应呈**土 / 岩**（不再是绿色）。
  4. 仍未闭环：**I1（CI 红）**、**T19 两处口径**；本批改动**尚未提交**。

---

## 2026-09-27  M9：把"禁止冻结画面 / 重活必须离开渲染帧"升格为无条件硬规则（+ 定位"走动就卡"的根因）

- 做了什么：
  1. **M9（规范）**：项目所有者提出"任何让画面冻结的行为都是严格禁止的；所有开销都要后台执行完再呈现"。判定 **合理**，写入 SKILL 第四节新条「**所有重活都必须离开渲染帧**」：① **无条件禁止冻结**（任何一帧都不得因为"还在算 / 还在等"而停止出帧或停止响应输入，**无例外**）；② **重活三选一，按优先级**：**消除**（设计上别让它成为重活）> **下沉 worker** > **按帧预算切分**；③ 必须当帧 / 不可切分的工作不是"允许冻结"，而是**必须把单帧成本压进预算**，压不进就**重新设计**优先；④ **例外与澄清**：GPU 上传**不得**离开渲染线程（SDL_gpu 命令缓冲单线程）⇒ 准确表述是「渲染帧内只做**必须当帧且成本有上界**的事」，"一切开销都后台"不能按字面执行；⑤ **设计期义务**：新路径必须回答"最贵一步在哪个线程 / 单帧最坏多少毫秒"并写进计划条目。DoD 增一条勾选。
  2. **定位"仅在地面走动就卡"（用 T38 的观测 + 代码核对，不靠猜）**：`main.cpp` 的**渲染原点重定基**在 `distance(renderOrigin, 焦点) > 24 格` 时触发 ⇒ 行走 9 格/秒 **每约 2.7 秒一次**（冲刺约 1.2 秒），而该分支会 `for (all tileCoords / all volumeCoords) UploadMesh(...)` —— **137 个网格的阻塞式重传**（SDL_gpu 上传阻塞到 GPU 完成），debug 下合计 **≈70~140 ms** 当帧停顿。**静止不动永不触发**，与"只有走动才卡"完全吻合。**已排除**：每帧 uniform 重建、`MaxSurfaceHeightBlocks`、角色/光球顶点就地刷新、物理步进（都随帧发生、与走不走无关，且亚毫秒）。
  3. **T41（方案 A，待开工）**：项目所有者选定"**每网格自带原点**"⇒ 顶点保持块内局部坐标、每个网格带自身世界原点，绘制时偏移 = `网格原点 − 渲染原点` ⇒ **重定基变成一次 uniform 更新、零重传**。已写进阶段计划，含落点、判据与"下一轮先做一次最小绑定验证再改 shader"的前置说明。
- 为什么：
  1. 这条规范是原「不冻结画面」的**加强版**：原条只管"加载 / 长任务"的形态，本条覆盖**全部路径**，并把"消除"提到首选（T41 正是"本该消除、却被写成重活"的实例）。
  2. 写清 3 处边界是因为"一切开销都后台"按字面执行会产生**违规实现**（把 GPU 上传挪到 worker、把必须原子的整批更新切成两帧导致几何错位）。
- 验证：
  1. **本轮只有规范与文档改动**（SKILL 第四节 + DoD、`plans/v0.1.md` 的 M9 / T41 行、本条 devlog）；代码与测试未动。
  2. **门禁**：见本批末次运行（`0 violation(s)`）。
- 下一步 / 遗留：
  1. **T41 是下一轮第一个任务**（方案 A 落地；前置 = 验证 SDL_gpu 顶点 uniform 绑定约定，**未验证不得改 shader**）。
  2. **T40（阴影按级联剔除）待开工**，开工前需项目所有者在两条路里选一条（过渡方案 / 完整方案）。
  3. **人工复验 T41 的判据**：走动 30 秒，日志中 `渲染原点重定基到 …` 与 `帧尖峰 …ms` **都不应再出现**（或重定基仍在、但不再有重传导致的尖峰）。
  4. 仍未闭环：**I1（CI 红）**、**T19 两处口径**；本批改动**尚未提交**。

---

## 2026-09-27  启动加载无感（T36 / SKILL「不冻结画面」）：先给画面、再给结果

- 做了什么：
  1. **SKILL 新增硬规则「不冻结画面（"先给画面，再给结果"）」**：任何**一次性重计算**（经验阈值 > 3 ms）都不得让当前视角停下等待 —— ① 画面尽早出现（能立刻显示的部分照常显示，**不得**黑屏或冻住）；② 长任务**按帧切分**（预算队列）或**下沉工作线程**（CPU 可并行交给 enkiTS，GPU 上传必须留主线程）；③ **进度必须可见**；④ 切分只允许改变"何时可见"、**不得改变结果**（红线 7 / 11）；⑤ 允许的语义放松要登记并先确认，而"当前视角被冻结"是**无条件禁止**。禁止项：渲染帧内同步跑完超预算任务 / 用"先黑屏再显示"掩盖加载时间 / 把 GPU 上传挪到工作线程。
  2. **T36 落地（启动加载）**：把**窗口 / 渲染器 / ImGui 面板 / 清屏色 / 输入绑定**全部提到任何长任务之前 —— 启动后立刻进入 `LoadingScreen` 循环（出帧 + 阶段文字 + 总进度条 + 处理窗口事件）。长任务逐项改为**分片**：材质贴图 `MaterialTextureBuilder`（分步到**像素行**）、可挖体积 `BeginInitFromHeightField` / `StepInitFromHeightField`（**步骤数 = 2 × 块数**：先逐块填密度、再逐块网格化）、地形 tile 逐块 `LoadTile`、碰撞体逐块 `SyncTile` / `SyncBlock`、网格逐批 `UploadMesh`。每帧 8 ms 预算（`kLoadWorkBudgetMs`），总进度按阶段权重折算成百分比。
  3. **两处实现要点（不是可选优化，是正确性/可行性的前提）**：
     - **可挖体积必须"两轮"**：分步若"填一块密度就立刻网格化那一块"，网格化读到的邻块是**尚未填充**的回退值（未取整的高度场推导值）⇒ 与一次性初始化**不等价**、并在块边界留下发丝级差异。故步序固定为"全部密度先就位，再逐块网格化"；新增测试 `DigVolume.SteppedInitMatchesOneShotInitExactly` 用 2×1×2 = 4 块（**含共享边界采样**）逐位比对密度 / 填充分类 / 网格顶点与索引。
     - **`FastNoiseLite` 不能进公共头**：`world/CMakeLists.txt` 把它列为 **PRIVATE** 依赖，而分步生成器要把噪声实例作为状态持有 ⇒ 用 **pimpl**（`struct Impl` 定义在 .cpp）把实现细节留在 world 内部，不把第三方头泄漏给 game / tests。
  4. **加载期呈现模式**：`SetVSync(false)` + 关掉限帧。理由：邮箱/垂直同步下每次呈现都要等一个刷新间隔，若按 8 ms 预算分片，7 s 的活会被"等待"放大成十几秒 —— 加载期以工作量决定节奏，**世界就绪后**再由 `ApplyFrameRateCap` 恢复玩家设置。
  5. **T35 记录（不改动）**：把"看地面不卡、看山卡"收敛到 `mesh.frag` 的三平面投影（陡壁触发，albedo / normal 各 3 次采样，逐层生效 ⇒ 采样/带宽约 ×1.8，**全在 GPU 侧**，故 CPU 三相看不到）；候选优化 A~D 与自适应降档已列进 `plans/v0.1.md`，**按项目所有者决定先记录**。
  6. **T37（运行时破坏分帧）登记为待开工**：帧预算队列（每帧 ≤2~3 ms），语义按项目所有者确认 = **接受"全部脏块分帧"**（洞口/塌落 1~N 帧内补齐）。
- 为什么：
  1. 加载期旧行为是**全程阻塞**（约 7.3 s 画面不动）：根因不是"算得慢"，而是"**算的时候不给画面**" —— 窗口、渲染器、面板这些出帧所需的资源排在所有重计算之后。把资源前置 + 把长任务切分，是唯一能同时满足"窗口尽早可见"和"进度可信"的做法。
  2. 分片**只改可见时机、不改结果**：故两种分步实现都用"与一次性路径逐位一致"的测试钉死，而不是靠"看起来一样"。
- 验证：
  1. **构建**：零错误零警告（`/W4` + `/WX`）。
  2. **测试**：`ctest` **243** 项、**242 通过**；新增 2 项（`DigVolume.SteppedInitMatchesOneShotInitExactly`、`TerrainMaterialTexture.SteppedBuilderMatchesOneShotByteForByte` —— 后者断言分步产物与 `GenerateMaterialTextures` **逐字节相同**、且进度单调不减）。
  3. **门禁**：`scanned 108 file(s), 0 violation(s)`、`PASS`。
  4. **冒烟（debug，360 Hz，mailbox；两次实测 7.94 s / 8.56 s）**：**首帧 0.36 s** 出现加载画面（旧版要黑屏到 7.3 s）→ 材质贴图 2.4~2.6 s → tile 2.7 s → 可挖体积 6.3~6.8 s → 碰撞接管 7.4~8.0 s → 网格上传 7.9~8.5 s → 世界就绪 **7.94~8.56 s**（旧版 7.31 s ⇒ 总时长约 **+9%~17%**）；随后日志 `帧率上限 → 360（垂直同步…）` 证明设置已恢复，爆炸 / 落体自检（脚底 120.00，着地=是）/ F1 面板 / 渲染原点重定基均如常。加载画面实测约 **190 FPS**（8 ms 预算 + 无垂直同步等待）。
- 下一步 / 遗留：
  1. **T37 待开工**（运行时破坏分帧；语义已确认）；**T35 先记录不改动**（项目所有者决定）。
  2. **1 项测试红且与本轮无关**：`TerrainMaterial.LoadsCommittedConfig` 断言提交态 `[triplanar] enabled = true`，而工作区里 `assets/config/materials.toml` 被上一轮改成 `false`（**未提交的本地验证改动**）—— 需要项目所有者决定"回滚该行"还是"改测试口径"。
  3. **总时长仍有 +9%~17% 的代价**（加载期出帧的开销）。若要把总时长也压回：把每批行数 / 块数调大（现为贴图 6 行、体积 1 步、上传 4 块）或把预算提到 12 ms —— 属**可调参数**，不影响正确性。
  4. 仍未闭环：**I1（CI 红）**、**T19 两处口径**；本批改动**尚未提交**。

---

## 2026-09-27  M8：把「卡顿消除」写成施工标准（SKILL + 新 reference）

- 做了什么：
  1. **新增 [`references/performance-and-hitches.md`](../.trae/skills/voxel-engine-dev-standards/references/performance-and-hitches.md)**（本技能首份专讲性能的引用）：
     ① **三类卡顿分类与判据** —— 尖峰型 hitch（单帧 > 2× 帧预算）/ 持续型掉帧（P95/P99 > 1.5× 均值，且随视角或位置变化）/ 加载型冻结（> 100 ms 无出帧窗口）；总判据 = **P99 ≤ 2× 帧预算且无 > 50 ms 单帧**，并明确"**只看平均帧时间不算验证**"。
     ② **四层手段 + 逐条业界参照** —— 不让重活落在渲染帧里（UE5 Task Graph 的 time-slicing、Unity Job System + Burst、Naughty Dog / Insomniac 的 hitch-free 预算表）；流式与增量（MC 式提前预取、Horizon 的 streaming budget、id Tech 的 page residency）；提交侧可控（HZB 遮挡剔除、GPU-driven + indirect draw、每帧一次 storage 上传、UE 的 PSO cache）；帧节奏（vsync / mailbox / immediate、三重缓冲 + CPU 提前一帧，并把"等交换链"列为**必须可观测**的项）。
     ③ **观测义务** —— 帧尖峰打点（三相 CPU + draw call + 提交网格数 + 是否在等交换链）、P50/P95/P99、Tracy、RenderDoc/PIX；**禁止"感觉更快了""应该就是这个原因"**。
     ④ **自查清单 + 本项目伪优化反例** —— 七条"新增重活"必查项（是否与**总量**成正比 / 是否有逐物体 uniform 推送 / 最坏单帧是否实测 / 是否"首次命中才创建" / 是否同步等 GPU / 是否随视线变化 / 长任务是否切分）；反例把本项目踩过的坑写死（提帧率上限无改善、关 F1 面板无改善、只看三相把 GPU 侧成本漏掉）。
  2. **`SKILL.md` 三处**：任务路由新增一行（性能优化 / 卡顿定位与消除 → 新 reference）；第四节新增「**卡顿消除（hitch-free）——业界标准与硬规则**」（三类卡顿 + 可判定判据 + **五条硬规则** + 禁止项 + 与「降级必须先问」的衔接）；DoD 新增一项勾选（含"未实测不得声称已优化"）。
  3. **`engine-capabilities.md`**：登记「**帧尖峰打点（hitch 观测）**」为 `未开始`（标准已要求、实现未做），并把「卡顿消除施工标准」作为规范行登记（**注明"这是规范、不是引擎能力"**）。
  4. **`plans/v0.1.md` 新增 M8 行**（规范类任务），其中登记**已知缺口**供后续按标准逐项消除：T37 运行期破坏分帧、视锥剔除 + uniform 合并、T35 三平面降档、运行期"首次命中才创建"类资源。
- 为什么：
  1. 项目所有者要求"**把处理卡顿的性能优化业界标准规范加入 skill，以后都按这个标准来**"，且明确**本轮不针对当前项目的卡顿做优化** —— 所以本轮只立标准、不改引擎。
  2. 立标准的目的是消除两类反复出现的问题：**凭直觉优化**（本项目真实出现过"先怀疑 CPU 三相、实测正常"的误判）与**用障眼法结案**（提帧率上限 / 降画质 / 关面板都改变不了单帧工作量）。故标准里把"**先打点、再改**"和"判据必须可量化"写成硬规则。
  3. 与既有节划清边界，避免两处并存的口径：与「不冻结画面」是"通用手段 / 加载长任务形态"的分工（叠加适用，不互相替代）；预算**数字**仍以 ADR 0008 与第四节为准，新文件**不另立数字**；达不到判据时走「降级必须先问」。
- 验证：
  1. **本轮无代码改动**，故不涉及构建与单元测试（`ctest` 仍为 **243 项 / 242 通过**，唯一红项是 T36 记的那条未提交本地配置）。
  2. **门禁**：`scanned 108 file(s), 0 violation(s)`、`PASS`（新增 reference 不属扫描目录，计数不变）。
  3. **一致性自查**：新 reference 的链接指向 `SKILL.md` 第四节与 `concurrency.md`；`SKILL.md` 的 DoD 项与 reference §3 自查清单逐条对应；`engine-capabilities.md` 的"未开始"行给出落点与判据。
- 下一步 / 遗留：
  1. **按新标准执行的下一步建议**（未获开工指令前不动）：① 落「帧尖峰打点」（零语义变更，是所有后续优化的前提）；② T37 运行期破坏分帧；③ 视锥剔除 + uniform 合并；④ T35 三平面降档。
  2. 仍未闭环：**I1（CI 红）**、**T19 两处口径**、`TerrainMaterial.LoadsCommittedConfig`（本地 `triplanar = false` 所致）；本批改动**尚未提交**。

---

## 2026-09-27  T37~T39 落地：按 M8 标准解决卡顿（破坏分帧 / 打点 / 剔除去重）+ T35 降档

- 做了什么（严格按 M8 标准的顺序：**先打点 → 再改**）：
  1. **T38 帧尖峰打点（观测先行）**：`RenderStats` 新增 **`swapchainWaitMs`**（在 `SDL_WaitAndAcquireGPUSwapchainTexture` 前后用 `vx::Clock` 计量）—— 此前这段"等交换链"的等待混在"渲染提交"里，**会把"在空等 GPU"误判成"CPU 忙"**；`main.cpp` 在帧 > **33 ms**（2 × 帧预算）时打 WARN：逻辑 / UI / 渲染提交 + **draw call** + **提交网格数** + **等交换链** + 固定步数 + **主要受限在谁**，节流 200 ms；F1 面板补 **P99** 与「等交换链」两行（新增 `UiLabel::SwapchainWait`）。
  2. **T37 运行时破坏分帧**：新增 `game/destruction_queue.hpp`（`PendingDestruction`：入队即**升序去重**；**先全部"重网格 + 上传"、再全部"重建碰撞体"**，避免"物理已通、画面没洞"的中间态）+ `main.cpp` 的 `DestructionProcessor`（每帧 `kDestructionBudgetMs = 3 ms`，**至少一个单位**保证进度）。`Detonate` 当帧只做**挖除 + 塌落**（毫秒级），其余入队 —— 日志拆成两段可核验："爆炸（当帧 = 挖除 + 塌落）… ⇒ 入队 N 块" 与队列排空的 DEBUG。新增 `DigVolumeWorld::RemeshBlock`（单块重网格）。**既有判据全部保留**（被体积接管的 tile 不重建高度场碰撞体、体积块按 `SyncBlock` 重建）。
  3. **T39 提交侧**：新增 `engine/render/frustum.*`（Gribb–Hartmann 平面提取 + AABB 保守相交，**7 项单测**）+ `main.cpp` 每帧剔除（上传时算一次世界 AABB）；`DrawMeshes` 增加**推送去重**（`EmissivePushState`）⇒ 逐物体 uniform 推送从 **O(网格数)** 降到 **O(值变化次数)**（≈ 2 次/帧），绘制结果逐像素不变。
  4. **T35 降档（项目所有者选定候选 A）**：`materials.toml` 恢复 `[triplanar] enabled = true`（顺带消除那条红测试）；`mesh.frag` 在**原有三轴权重（同一公式）**上把权重 < 0.02 的轴置零、剩余轴重新归一化 ⇒ 垂直壁采样 3 → 1 次、斜壁 3 → 2 次，外观差异仅来自被丢弃的近零贡献。
- 为什么：
  1. M8 标准把"**观测先于结论**"写成硬规则 5，且本项目**已经吃过一次亏**（把 GPU 侧三平面成本判到 CPU 三相上）—— 故本轮第一步不是优化而是打点。
  2. T37 的取舍（**每帧至少一个单位，即使单件超 3 ms 预算**）是刻意的：一个体积块的重网格 + 上传实测 5~9 ms、碰撞体重建约 9 ms，**让出本帧只会让进度停住**；最坏帧多花约 9 ms，仍在 60 Hz 的一帧（16.7 ms）之内，不构成冻结。
- 验证：
  1. **构建**：零错误零警告（`/W4` + `/WX`）；`mesh.frag` 已重新产出 **`.spv` + `.dxil` 双格式**（13:26:43，晚于源码修改时间）。
  2. **测试**：`ctest` **254/254 全绿**（243 → 254：`frustum_test` 7 项 + `destruction_queue_test` 4 项 + 此前那条红测试随 `triplanar` 恢复而转绿）。
  3. **冒烟（debug，360 Hz，mailbox）**：启动无 WARN / ERROR；新增一条可核验的剔除日志 ——
     `首帧视锥剔除（T39）：地表 tile 0/9、可挖体积块 133/441 通过（含阴影扫掠余量）；本帧提交网格 134 个`。
     tile 为 0 是**正确**的（9/9 地表可见面已全由体积接管 ⇒ 本就没有网格）；**体积块 133/137 通过 ⇒ 本世界内剔除只去掉约 3%**。
  4. **门禁**：见本批末次运行（`0 violation(s)`）。
- 下一步 / 遗留（**重要：一条实测结论 + 一条需确认项**）：
  1. **T39 的效能限制（已定位，非实现错误）**：为了**不丢阴影**，剔除判定必须用"物体 ∪ 其影子落点"（沿太阳方向扫掠）；本图太阳仰角较低（`dir.y = 0.80`）而体积块位于 y ≈ 120~200 ⇒ 扫掠余量 **150~250 格**，把小世界（224 格宽）几乎全罩住。**去掉余量会重现 B6/B8（阴影随视角消失）**，故不能去掉。
  2. **T40（新增，待开工）**：提交侧的真正大头是**阴影通道按 3 级级联各画一遍全部网格**（3 × 137 = 411 draw call，占每帧约 3/4）。开工前需确认走哪条路：① 过渡方案"主通道剔除 + 阴影用完整列表"（零阴影风险，只省主通道）；② 完整方案"按级联光空间盒剔除"（效果最好，须把级联盒交给 CPU 侧，属引擎改动）。
  3. **人工复验（需项目所有者操作）**：① 旋转镜头看山 —— 陡壁处的帧时间应低于改前（判据：F1 面板 P99，且不应出现「帧尖峰」WARN）；② 发射光球 —— **不应再出现明显顿卡**，洞口 / 塌落会在数帧内补齐（观感：裂口逐块"长出来"）；③ 洞口与洞底外观应仍为土 / 岩（三平面降档不得改变洞内观感）。
  4. 仍未闭环：**I1（CI 红）**、**T19 两处口径**；本批改动**尚未提交**。

---

## 2026-09-27  T41 落地：**每网格自带原点** —— 消除"走动时因渲染原点重定基触发的整世界重传"

- 做了什么：
  1. **Shader（2 个）**：`assets/shaders/mesh.vert` / `shadow.vert` 各加一个
     `layout(set = 1, binding = 0, std140) uniform MeshOffsetBlock { vec4 meshOffset; }`，
     顶点做 `局部坐标 + meshOffset.xyz` 后再乘（相机 / 光空间）矩阵；`mesh.frag` **语义不变**
     （它仍按"渲染相对 + 渲染原点"还原世界坐标，只是"渲染相对"现在由**顶点阶段**补齐）。
     创建 Shader 时两个**顶点**阶段的 `num_uniform_buffers` 由 **0 → 1**。
  2. **引擎（`engine/render/mesh_renderer.*`）**：`MeshResources` 增 `origin[3]`；`UploadMesh(mesh, origin, emissive)`；
     `UpdateMeshVertices(handle, vertices, origin)`（**同时更新登记的原点**）；新增 `SetRenderOrigin(vec3)`；
     `DrawMeshes` 在绘制每个网格前推 `网格原点 − 渲染原点`（`SDL_PushGPUVertexUniformData(cmd, 0, ...)`，16 字节），
     **主通道与阴影通道各持一份推送去重状态**（与 T39 的自发光去重同构）。
  3. **游戏层（`game/main.cpp`）**：tile / 体积块上传改为**直接上传网格局部顶点 + 网格自身原点**
     （删除 `BuildRenderMesh` 与体积的两处顶点改写循环）；角色 / 光球仍每帧烘焙"渲染相对"顶点，
     但原点传**当前渲染原点**（偏移恒 0）；**重定基块删掉两轮 `for (all …) UploadMesh(...)`**，
     只更新 `renderOrigin` 并打一行日志；每帧 `renderer.SetRenderOrigin(glm::vec3(renderOrigin))`。
- 为什么：
  1. 这是 SKILL 第四节 M9「所有重活都必须离开渲染帧」的 **① 消除**类落地：**渲染原点重定基本不必是重活** ——
     只要"这块网格在世界哪里"不烘进顶点、而是随网格登记、绘制时推一次，重定基就退化为一次常量写入
     （旧做法在重定基那一帧对 137 个网格逐个 `ReleaseMesh` + `UploadMesh`，每次阻塞到 GPU 完成，debug 下 ≈70~140 ms）。
  2. **绑定约定不靠猜**：读 `build/debug/vcpkg_installed/x64-windows/include/SDL3/SDL_gpu.h`（`SDL_CreateGPUShader` 的文档段）确认 ——
     顶点阶段 **set 0 = 采样 / 存储纹理 / 只读存储缓冲**、**set 1 = uniform 块**（片元阶段为 set 2 / set 3），
     块在 set 内的序号即 `SDL_PushGPU{Vertex,Fragment}UniformData` 的 `slot`，且声明个数**必须**与
     `num_uniform_buffers` 一致。该约定已写入 `references/meshing-and-render.md` §6（T27 与 T41 两次用到同一处文档），下次不必再猜。
  3. **等价性**：旧 = `float(网格原点 + 局部 − 渲染原点)`，新 = `float(局部) + float(网格原点 − 渲染原点)`；
     网格原点与渲染原点都是整数（取整），减法精确 ⇒ 两者数学同值，差异只来自浮点结合次序
     （**精度优于**改前：顶点只承载块内小数值）。故"洞 / 塔 / 主角 / 光球位置不变"。
  4. **关于"逐物体 uniform 推送"（M8 自查项）**：这里的推送是 **16 字节的 `SDL_PushGPUVertexUniformData`** ——
     **不创建 / 不拷贝 GPU 缓冲、不等 GPU**，与"每次上传都阻塞到 GPU 完成"的旧路径性质完全不同，且与 T39 一样做了去重。
     **每帧推送次数 ≈ 提交网格数 ×（1 主通道 + 3 级联）**（本场景约 550 次 × 16 B < 9 KB/帧），
     是"把整世界顶点变成局部坐标"的固有代价。
     **同时澄清了一处冲突的口径**：原规则（SKILL 第四节硬规则 3 与
     `references/performance-and-hitches.md` §1.3 第 2 条 / §3 自查项 / §6 禁止项）把"逐物体 uniform 推送"一律
     **视为缺陷 / 禁止**，与本方案（项目所有者选定的"每网格自带原点 ⇒ 逐网格推偏移"）**直接冲突**。
     已按"**单一结论、不留两处矛盾**"收紧判据为：**逐物体上传 / 逐物体创建资源 / 提交量随"世界总量"增长**才算违规，
     并明文写出逐物体**小常量推送**的三个条件（① 是推送不是上传；② 上界 = 剔除后的可见网格数；③ 相同值去重）。
     这是**对齐已定设计**的口径澄清，不是放宽标准（能用 storage buffer 一次上传 + 索引时仍优先那条路）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + `/WX`）；
     `mesh.vert` / `shadow.vert` 已重新产出 **SPIR-V + DXIL 双格式**。
  2. **测试**：`ctest --preset debug` → **254/254 passed**（无回归）。
  3. **冒烟（debug，360 Hz，mailbox；前台运行约 50 秒）**：启动无 ERROR、无 SDL 断言；
     **发生 3 次重定基** —— `渲染原点重定基到 (-1, 120, 24) / (8, 120, 1) / (1, 121, -23)（T41：只更新 uniform，零重传）`，
     **均无同帧「帧尖峰」WARN**。同批日志里的 3 条 WARN 都是**别的原因**：`25.982`（逻辑 1.33 ms、帧 37.3 ms，**该帧未重定基**）、
     `31.110`（**逻辑 21.95 ms = 当帧塌落 15.60 ms**，T30 / T37 已登记的破坏开销）、
     `36.480`（等交换链 1.17 ms，且恰在**窗口失焦**那一帧 —— 日志同帧有 `鼠标捕获：关（窗口失焦）`）。
  4. **几何的自动化核对（本环境无目视能力，故用屏幕捕获 + 统计）**：游戏运行时截屏，画面自上而下为
     **天空 (201,215,226) → 雾 (140,142,153) → 中下部草地 / 泥土 (28,51,49) / (88,63,47)**；
     与"桌面"截屏的平均逐像素差 **106/255**（证明截到的确实是游戏画面，而非桌面）。
     若顶点偏移未被应用（网格堆在局部原点），画面会几乎只剩天空 —— 实测不是 ⇒ **排除"整体错位"**。
     像素级一致性仍须人工目视。
  5. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 113 file(s), 0 violation(s)`、`PASS`、退出码 0。
- 下一步 / 遗留：
  1. **待人工目视（须项目所有者）**：走动 30 秒确认**不再周期性卡顿**，并核对**洞 / 地标塔 / 主角 / 光球**的位置与改前一致。
  2. **T40 待开工**，开工前须先确认走"过渡方案"还是"完整方案"（见 `plans/v0.1.md` §3「下一步」第 7 条）。
  3. 仍未闭环：**I1（CI 红）**、**T19 两处口径**；本批改动（T36~T39 + M8 / M9 + T35 + **T41**）**尚未提交**。

## 2026-09-27  T33 落地：**统一连通分量刚体化** —— 删除"逐列独立下落"

- 背景（项目所有者的判定）：上一版模型是"**逐列独立 + 悬挑容差**"，**原理上**只能产生"原地垂直下沉 / 局部陷落"，
  产生不了**倾斜与旋转**。所有者明确："逐列独立 + 悬挑容差明显不符合真实世界，按登记的开发方向做整体刚体化，
  **严禁**现在这样逐列独立的不符合真实世界的效果"。⇒ 先改 [ADR 0015](adr/0015-structure-units-and-rigid-collapse.md)
  （覆盖范围与兜底经所有者确认：**统一连通分量刚体化** + **超大分量不设上限、继续刚体化**），再动代码。
- 做了什么：
  1. **`world/dig/volume_collapse.*`（重写）**：保留**纵向载荷通路**作为"支撑判定"（它判的是"有没有支撑"，
     不再是下落模型），其后按 **6 邻域连通分量**分组（确定序 x → y → z）⇒ 每个分量 = **一个倒塌整体**
     （体素列表 / 包围盒 / 质心 / 逐列 8 角点的凸包点集 / 材质槽位）；按体素数降序取前 `max_units` 个**抽出**
     （原处清空 + `WriteDensityRegion`），其余保持原状并计入 `skipped_units`；**删除**逐列下落与 `pile_spread_blocks`。
     回写下沉到 world 层 `WritebackCollapseUnit`（按落定位姿逐体素中心旋转落位到最近格），使其**脱离 GPU / Jolt 可单测**。
  2. **`engine/physics/physics_world.*`**：新增 `SetGravity`、`AddDynamicConvexHull`（`ConvexHullShape` + 质量 +
     摩擦 + 初始角速度）、`GetRigidBodyState`（位姿 + 线 / 角速度）、`RemoveBody` 通用。Jolt 的位置存**质心**，
     本类对外以**局部原点**为准（`bodyComLocals` 折算）⇒ 渲染与体素化回写用同一套坐标。
  3. **`game/rigid_collapse.*`（新）+ `game/main.cpp`**：先建**退化网格池**（`max_units` 个槽位，每槽容量 98304 顶点，
     索引模式固定），`Spawn` 时把该整体的**外表面**写进槽位（局部顶点，只写一次）；每个固定步 `physics.Update` +
     `Step` 做落定检测；每帧 `SyncRender` 只推 **64 B 的 `modelToRender`**（**不重烘焙顶点**）；落定即 `Writeback`
     （体素化回写 + 释放刚体 + 清空槽位），重网格 / 碰撞体重建走 T37 的延后队列。
  4. **`assets/shaders/mesh.vert` / `shadow.vert`**：逐网格 uniform 由 `vec4` 偏移**泛化为 `mat4 modelToRender`**
     （平移 × 旋转）—— 这是"倒塌刚体每帧翻滚但 CPU 不重烘焙顶点"的前提；法线用 `mat3(modelToRender)` 变换。
  5. **`assets/config/collapse.toml`（`schema_version = 2`）**：删 `pile_spread_blocks`，新增
     `mass_per_voxel` / `friction` / `settle_linear_speed` / `settle_angular_speed` / `settle_steps` /
     `initial_tilt_speed` / `max_active_units`（全部非法即抛）。
- 为什么：
  1. 真实世界里"上面那一块"是**一个整体**：重心一旦越出支撑面就**倾倒 + 翻滚**，落定姿态与原来毫无关系。
     逐列模型**在数学上**没有自由度表达这件事，所以这是一次**方向性替换**，不是调参。
  2. 初始不对称用**确定性哈希**给出的初始角速度表达（红线 7：禁止随机数）；真实失稳总是"一侧先屈服"，
     若初速恒为零，细长的塔只会原地垂直落下。
  3. 落定判据用"连续 N 步线 / 角速度都低于阈值"（Jolt 的默认阻尼与休眠保留）：落地后更快静止，
     避免"看着停了却一直不算落定"。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **260/260 passed**。新增：`volume_collapse_test.cpp` 9 项
     （稳定面不塌 / 悬空石板成**一个**分量并抽出 / 按原姿态回写守恒 / **90° 落定的回写形状显著不同** /
     分组确定性 / `enabled=false` 不动 / 超上限保持原状 / 游戏尺度性能 + 配置表）+ `physics_test.cpp` 3 项
     （动态凸包下落并停在地面 / **给定初始角速度必然转动**（1 秒 57°，判据 ≥ 30°）+ 重力确实是 `SetGravity` 的值 /
     非法参数拒绝）。
  3. **冒烟（Debug，前台，真实操作）**：向测试地图的**地标塔基座**（3×5 列、高出高台 130 格）发射光球 ⇒
     `失去支撑 1815 体素、整体 1 个、已刚体化 1 个` ⇒ `倒塌落定（T33）：回写体素 1815 个（丢弃 0 个）；倾角 88°、落点 (51.7, 132.9, 4.3)`
     —— **整座塔作为一个刚体倾倒并翻滚后横躺**（同批另有倾角 82° / 90° / 77° 的整体）。
     回写后残骸成为地形（可站、可继续挖），无 ERROR、无断言。
  4. **无卡顿判据**：最坏单帧 **46.0 ms**（判据 < 50 ms）；帧尖峰 33~46 ms，其中「倒塌」段 Debug **5.9~25.2 ms**
     （= 支撑检查 + 分组 + 抽出 + 刚体化 + 网格写入，全部在当帧主线程）。
  5. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 115 file(s), 0 violation(s)`、`PASS`、退出码 0。
- 下一步 / 遗留（**已如实登记**）：
  1. **凸包填平**（ADR 0015 后果 1）的实测表现：若被抽出的是**凹形整体**（例："塔 + 平台板"在体素上连成一体），
     其凸包**大于**被挖空的体积 ⇒ 抽出处立即与周围地形重叠，Jolt 顶出后容易被**卡住**，落定倾角可能只有 **1~5°**
     （"干净切在塔基"时实测 **88°**）。这是"凸包"这一选择的固有代价，**凹形分解**（多凸包复合）未做。
  2. 「倒塌」段里**与体素数无关的固定开销 ≈ 8 ms**：每槽按**整容量**补齐顶点并上传 **2.62 MB**。
     可优化为"只上传用到的前缀 + 按索引数绘制"（`mesh_renderer` 增"本网格绘制索引数"）。
  3. **阶段 2**（`structureId` 强制合并 + `[[structure]]` 声明）仍是切换条件触发式：需要"同一结构被挖成两半仍整体倒"时再做。
  4. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + **T33**）**尚未提交**。

## 2026-09-27  T42 落地：**倒塌整体的外观口径统一**（掉落中不再是方块棱角、落地后不再换一套外观）

- 背景（项目所有者实测）："当前物体掉落时**棱角明显**，落地后**外观又发生了变化**"。判定为**确认为缺陷**
  （契约 = 同一物体在"静止 / 运动中 / 落定"三态之间不应切换表示口径；机制 = 三处口径不统一，见下），
  按「降级必须先问」先给出方案与三个选择，所有者选定：**A′ 与地形同源** + **顺带做"按用量上传 / 按索引数绘制"**
  + **本轮连体素级材质一起做**（方案与 3A 三问见 `docs/plans/v0.1.md` 的 T42 行）。
- 根因（三处口径不统一，均定位到代码）：
  1. 几何与法线：倒塌网格是**逐体素的轴对方块面 + 面法线**（`game/rigid_collapse.cpp` 的 `kFaces`），
     而地形 / 洞是 **Surface Nets 等值面 + 密度梯度法线**（`world/dig/volume_mesher.*`）；
  2. 材质：掉落中整块刚体只有**一个**槽位、且取自**质心所在列**，落地后改由**逐 cell / 落点所在列**派生 ⇒ 颜色变；
  3. 落定即体素化回写 ⇒ 几何被**重新离散化**。附带发现一处语义错：`materialSlot == kNoMaterialSlot(0xFF)`
     被写成顶点 `material = 255.0F`，而片元口径是"**< 0 = 未指定**"（此前靠"越界即回落"侥幸不炸）。
- 做了什么：
  1. **`world/dig/volume_mesher.*`**：`BuildVolumeMesh` 泛化为 **`BuildRegionMesh(sampler, sizeX, sizeY, sizeZ)`**
     （同一套数学、同一发射顺序；原入口改为 32³ 的薄包装）。
  2. **`world/dig/volume_collapse.*`**：`CollapseUnit` 增 **体素补丁**（`patchBounds` = 体素包围盒 ±1 格采样、
     密度 + 有效材质；**抽出前**抓取，是"它原本的样子"的证据）与 **`BuildCollapseUnitMesh`**（用同一份
     Surface Nets 网格化补丁，顶点平移到"相对质心" ⇒ 与凸包同一坐标系）；删除 `materialSlot`（不再需要单槽位）。
  3. **`world/dig/dig_volume.*`**：**体素级材质持久化**（ADR 0014 的切换条件被触发）—— `VolumeBlock::material`
     （33³、`0xFF` = 未写入、**懒分配**）、`ReadMaterialRegion` / `SetMaterialSlot` / `MaterialBytes`；
     `SampleMaterialSlot` 改为"**已写入优先、否则回落列派生**"（未发生倒塌时逐位不变）。
  4. **回写搬材质**：`WritebackCollapseUnit` 把每个体素**原本的材质**写到落点（否则岩体落地按落点那一列变泥土）。
  5. **`engine/render/mesh_renderer.*`**：新增 **`UpdateMeshGeometry`**（顶点与索引**都可变**、**只上传用到的前缀**、
     空网格 = 不可见 ⇒ **零上传零绘制**）、`MeshResources::usedIndexCount`（绘制与统计都按实际绘制量）、
     抽出 `EnsureStagingBuffer`（顶点 / 索引两条常驻暂存缓冲，容量够不重分配）。
  6. **`game/rigid_collapse.*`**：槽位改用 `UpdateMeshGeometry`，几何来自 `BuildCollapseUnitMesh`；
     超容量时**按整个四边形**截断（只影响外观）；`Init` 的槽位建成"满容量 + 不可见"；
     删掉 `kFaces` / `BuildPaddedMesh` 那套方块面构造与"补退化顶点"的填充。
- 为什么：
  1. 业界 / 3A 同类做法（Teardown / NVIDIA Blast / Minecraft 类活塞推动）都**不换表示口径**：碎块沿用同一套
     网格化与材质，落地后写回同一份世界数据。A′ 直接复用 `BuildRegionMesh` ⇒ **抽出来的就是它原本那一片等值面**，
     连数值都一样（单测：子区域 ≡ 整块，4 ULP 内；整块尺寸下**逐位相同**）。
  2. **不新增"第二套渲染口径"**：顶点仍是"网格局部坐标 + 逐网格 `mat4`"（T41 / T33 的既有机制），
     所以这次改动**没有**新增每帧工作，反而**减少了**上传量（塔的等值面只有几千个四边形，
     而旧实现每槽固定补满 98304 顶点 + 上传 2.62 MB）。
  3. 材质必须**持久化**才能"落地后不变"：ADR 0014 当初把"塌落搬材质"列为切换条件，本次正是该条件被触发。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **267/267 passed**（新增 7 项：`VolumeMesher` 的
     区域入口 ≡ 整块**逐位相同** / 子区域与整块在**世界空间一致**（4 ULP，面积一致）/ 区域入口的材质兜底；
     `DigVolume` 的**持久化材质优先 + 懒分配零内存**；`VolumeCollapseRigid` 的**补丁 = 抽出前的密度与材质**
     + 网格索引合法 + 顶点材质、**回写把材质搬到落点**、**整体网格 = 原地形网格的子集**（逐三角形比世界坐标）。
     **回退验证**：新增用例在旧实现下必然失败（旧实现是方块面 + 单槽位材质）。
  3. **冒烟（Debug，真实操作，静置测量）**：向地标塔基座发射 ⇒ `失去支撑 1835 体素、整体 1 个、已刚体化 1 个` ⇒
     `倒塌落定：回写体素 1834 个（丢弃 1 个）；倾角 68°、落点 (79.3, 113.8, 17.9)`；
     **体素材质共 0.24 MB（懒分配）**、启动时为 **0.00 MB**；**无 ERROR、无 SDL 校验报错**
     （证明"按用量部分上传"这条路径被 SDL 接受）。
  4. **无卡顿判据**：静置测量下帧尖峰 **33.8~43.7 ms**（阈值 33 ms，判据 < 50 ms 满足）；
     "倒塌"段 14.9~23.0 ms（含补丁抓取 + 区域网格化 + 刚体化 + 前缀上传）。
  5. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 115 file(s), 0 violation(s)`、`PASS`。
- 下一步 / 遗留：
  1. **待人工目视（须项目所有者）**：把地标塔 / 立柱根部轰断，确认**掉落中不再是方块棱角**、
     **落地后颜色与掉落中一致**（本环境无法用截图隔离该判据：挖除与抽出**同帧**发生，洞口本身也在变，
     故改用上文的"网格 = 原地形子集"单测作为可复现判据）。截图存于 `%TEMP%\vx_t42_*.png`。
  2. **一次 61.5 ms 帧尖峰待复核**：出现在"我自动化输入 + 截图"的那一帧，三相 CPU 合计仅 5.9 ms
     （未计时段 = 输入 / 限帧 / 面板）；静置复跑**未复现**（最大 43.7 ms）⇒ 登记为待复核，不当作已排除。
  3. **残余差异**：落定回写按"最近格"离散化 ⇒ 形状与掉落中最多差 ~1 格（机制性，ADR 0015 后果 3 已登记）。
  4. **未做**：深度分层（浅土深岩）/ 矿脉 —— 数据通道已就绪（`VolumeBlock::material`），规则属生成侧后续。
  5. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + T33 + **T42**）**尚未提交**。

## 2026-09-27  T43 落地：**倒塌的真实感**（爆心冲量 / 材质化物理参数 / 小碎片清除）

- 背景（项目所有者提问）："**标准的三A大作如何让物体掉落更真实的**"。给出业界对照后按「降级必须先问」
  提问，所有者选定四项：**爆炸冲量驱动**、**材质化物理参数**、**落地摔断（二次碎裂）**、
  **爆炸后的浮空小碎片可直接删除（"当炸没了"），但不可破坏的物体不能被误删**；
  二次确认后：① "不可破坏"**本轮只加清除守卫**（完整语义待 [ADR 0013](../adr/0013-destructible-elements.md) / T31）；
  ② **落地摔断本轮不做**（登记为 **T44**，前置 = T31 的 `toughness`）。决策与备选见
  [ADR 0016](../adr/0016-collapse-realism-impulse-material-debris.md)。
- 现状差距（开工前逐一在代码里定位）：
  1. `RigidCollapseRuntime::Spawn` 的**初线速度恒为 0**，只有一个由坐标哈希产生的**人工小角速度**
     （`initial_tilt_speed = 0.9`）⇒ 没有"**被炸飞**"，碎块都是垂直掉的；
  2. `collapse.toml` 的 `mass_per_voxel = 2.4` / `friction = 0.6` 是**全局一套**、且 `mRestitution = 0` **硬编码**
     ⇒ 石 / 土 / 草 / 沙 一样重、一样滑、都不回弹（与现实明显不符）；
  3. 失去支撑的**碎渣**（几个到几十个体素）也各自变刚体 ⇒ 占满 `max_active_units` 名额、逐个触发回写重网格，
     而"炸碎"在观感上应当表现为**消失**。
- 做了什么：
  1. **`assets/config/collapse.toml`（`schema_version` 2 → 3）**：**删除** `mass_per_voxel` / `friction`
     （改由材质表驱动 ⇒ 避免两处事实来源漂移），**新增** `impulse_speed = 8.0`（格/秒；0 = 关闭冲量）
     与 `debris_delete_max_voxels = 24`（体素数；0 = 关闭清除）。
  2. **`assets/config/materials.toml`（每层四个新字段，`schema_version` 仍为 4）**：
     `density` / `friction` / `restitution` / `indestructible`（草 1.3 / 0.75 / 0.02；土 1.5 / 0.60 / 0.02；
     岩 2.6 / 0.70 / 0.12；沙 1.6 / 0.50 / 0.05；均 `indestructible = false`）。
     **不升版本**：四个字段**可选**、缺省值等价于引入前的全局口径（岩 ≈ 2.4 / 摩擦 0.6 / 弹性 0）⇒
     **非破坏性变更**，口径与既有 `subsurface` 一致（旧文件不改也能加载）。
  3. **`world/dig/volume_collapse.*`**：`CollapseSeed` 增 **爆心 + 半径**；新增纯函数
     `ComputeUnitPhysics`（**无 GPU / Jolt 依赖 ⇒ 可脱离引擎单测**）：
     逐体素 `v(p) = dir · impulse_speed · clamp(1 − d/R, 0, 1)` ⇒ `V = Σv/N`、
     `ω = Σ(r×v)/Σ|r|²`（**球体解近似**，避免把形状 / 质量属性拉回 world 层）；
     质量 = `Σ density`（逐体素 ⇒ **混合材质的分量质量正确**）、摩擦 / 弹性 = **多数材质**（同票取更小槽位 ⇒ 确定）；
     另加**小碎片清除** + `ContainsIndestructibleMaterial` 守卫；`CollapsePlan` 增 `deletedUnits` / `deletedVoxels`。
  4. **`engine/physics/physics_world.*`**：`ConvexHullDesc` 增 `restitution` / `linearVelocity` / `angularVelocity`
     （Jolt 的 `mRestitution` / `mLinearVelocity` / `mAngularVelocity`；`mOverrideMassProperties = CalculateInertia` 不变）。
  5. **`game/rigid_collapse.cpp`**：初始状态改为"**有冲量用冲量、没有才退回人工倾斜**"，
     质量 / 摩擦 / 弹性取自 `CollapseUnit`（即材质表）。
  6. **`game/main.cpp`**：把**爆心（弹丸命中点）与爆炸半径**填进 `CollapseSeed`；爆炸日志加
     "**清除小碎片 N 个 / M 体素**"。
- 为什么：
  1. 业界（GTA / RDR2 / Teardown / UE Chaos）让"掉落真实"的三个手段正是这三件：**冲量驱动**（不是让物体自己掉）、
     **材质化质量与恢复系数**（木 vs 石 vs 沙的落点表现不同）、**小碎片不参与刚体模拟**（否则既贵又假）。
  2. 冲量用**爆心 + 半径的线性衰减**而不是"给个随机推力"：它同时给出**方向**（背离爆心）与**大小**（近快远慢），
     且**完全确定性**（同输入 ⇒ 逐位相同，红线 7）；角速度由**冲量在分量上的分布不均**自然产生 ⇒
     "被炸飞 + 翻滚"来自同一个物理量，而不是再叠一个人工旋转。
  3. 质量 / 摩擦 / 弹性放**材质表**而不是 `collapse.toml` 的全局常数：石 / 土 / 草 / 沙 本来就不同，
     且 T31（坚固度）将来也落在同一个文件里 ⇒ 单一事实来源。
  4. 小碎片清除是**语义**选择而非优化：被炸碎的碎渣"当炸没了"比"悬在空中排队等刚体名额"更自洽
     （ADR 0016 已写明它**覆盖** ADR 0015 后果 3 的"质量守恒"口径：清除是**有意的质量不守恒**）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **274/274 passed**（新增 **7** 项，全部为可判定判据）：
     `VolumeCollapseImpulse` 的**方向朝外 + 随距离衰减**（同半径下更远 ⇒ |V| 更小）、
     **确定性 + 有限性**（同输入两次 `V` / `ω` 逐位相同，且非平凡）、
     **无冲量来源时绝不假装有冲量**（`impulse_speed = 0` / 无爆心 / 整段在半径之外 ⇒ 三种情形都 `hasImpulse = false`）；
     `VolumeCollapseMaterial` 的**质量 = Σ 逐体素密度 + 摩擦 / 弹性按材质**、
     **混合分量取多数材质、同票取更小槽位**（且质量仍逐体素累加）；
     `VolumeCollapseDebris` 的**小碎片被清除**（阈值边界：= 阈值清除、阈值 − 1 保留；不生成刚体、不回写、计数）、
     **含 `indestructible` 的分量不被清除**（同场景换成可破坏材质 ⇒ 立刻被清除，对照可证伪）。
     另按 `collapse.toml` 的新字段更新了配置表测试（`impulse_speed` / `debris_delete_max_voxels`）。
  3. **冒烟（Debug，前台，真实操作 —— 自动化鼠标 / 键盘注入后人工可复现）**：先向左上注入视角位移
     （yaw → ≈90°、指向地标塔），再**按住 W 走到塔基旁**，然后按住左键连发：
     `失去支撑 2227 体素、整体 2 个、已刚体化 2 个、**清除小碎片 3 个 / 11 体素**` ⇒
     塔（3×5×138 格 = **2072 体素**）整体倾倒，`倒塌落定：回写体素 2072 个（丢弃 0 个）；**倾角 74°**、
     落点 (53.9, 134.8, **68.0**)`（同批另有 12° / 66° / 33° 的中小整体）⇒
     **"被炸飞 + 翻滚 + 远抛"在真实玩法里成立**；**无 ERROR、无 WARN（除帧尖峰）、无 SDL 报错**。
  4. **门禁**：`check-banned-identifiers.ps1` → `scanned 115 file(s), 0 violation(s)`、`PASS`。
  5. **帧时间（如实记录）**：4 条帧尖峰 WARN → 34.3 / 33.1 / 42.6 / **61.4** ms。
     其中 42.6 ms 那条的"逻辑 49.77 ms"即**当帧爆炸**（挖除 + 支撑检查 + 抽出 + 刚体化，日志里
     "倒塌 25.50 ms"）；61.4 ms 那条**紧跟在 2072 体素的回写之后**（逻辑 19.95 + UI 0.18 + 提交 2.05 = 22.2 ms，
     **≈39 ms 落在三相之外的未计时区**）—— 与本轮 T42 遗留的"61.5 ms 待复核"**同一签名**，
     故可判定：**不是 T43 新增的 CPU 成本**，而是**打点覆盖缺口 + 本机配置下的呈现 / GPU 侧耗时**。
- 下一步 / 遗留：
  1. **观测缺口（新登记）**：T38 的尖峰日志只覆盖"逻辑 / UI / 渲染提交"，**角色与光球的逐帧顶点上传、
     uniform 构建、限帧睡眠与呈现等待都在三相之外** ⇒ 61.4 ms 里那 ≈39 ms 归属不明。
     下一步：把"未计时区"也纳入打点（或至少按段细分），否则"主要受限在谁"的结论会再次误导。
  2. **本机的帧预算背景**：`2560×1440` + **MSAA 4×** + Debug 构建下，**静置**也会出现 33~44 ms 的尖峰
     （T42 冒烟同口径数据）⇒ 判据"无 > 50 ms 单帧"在**本机 Debug 配置**下不具区分度；
     评判 CPU 侧改动应看"逻辑 / 倒塌段的毫秒数"（本轮 ≤ 25.5 ms 且集中在爆炸当帧）。
  3. **待人工目视（须项目所有者）**：塔被轰断后**碎块沿爆心方向飞出并翻滚**、落地前不再"直上直下"；
     小碎渣**直接消失**而不是悬空排队；不同材质落点表现不同（岩略回弹、土 / 草几乎不回弹）。
  4. **T44（落地摔断 / 二次碎裂）**：前置 = T31 的 `toughness`，未开工（见 ADR 0016 的「本轮不做」）。
  5. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + T33 + T42 + **T43**）**尚未提交**。

## 2026-09-27  T46 落地：**落地后的表示按材质分流**（岩石不变形 / 泥土融合且不悬空）

- 背景（项目所有者实测）："**当前物体下落时和落到地面以后长得不一样** —— 这个有好用的解决方案吗"。
  判定为**确认为缺陷**（契约 = 同一物体在"运动中 / 落定"两态之间不得无理由改变表示口径），
  并按「降级必须先问」三问三答定口径：**① 光球撞上就惰性回写**；**② 永久保留 + 扩池到 16 槽**；
  **③ 回写后按支撑检查沉降**。总口径（所有者原话）："**岩石落地后不允许发生任何形状变化，
  泥土可以和地面融为一体，泥土要掉到地上不能出现悬空的泥土**"。决策见
  [ADR 0017](../adr/0017-landing-by-material-rigid-vs-granular.md)（**修订** ADR 0015 决策三·5）。
- 现状（三处机制，全部定位到代码）：
  1. **过渡空窗 + 逐块长出**：落定当帧 `Writeback` 立即 `HideSlot`，而承载残骸的脏块只入队，
     由延后队列按"每帧 3 ms / 至少 1 块"推进 ⇒ 一次 20 块的落定要 **≈20 帧（0.3~0.6 s）** 才补齐；
  2. **旋转体的体素化量化 + 与地面熔成一片**：回写是"体素中心 → 最近整数格"的硬量化，
     掉落中是独立网格 + 随姿态旋转的法线，落地后成台阶近似、并与地面共用同一张等值面；
  3. **边缘材质众数投票**：重网格时 cell 材质取"实体侧 8 角众数" ⇒ 交界处可能被地面材质投赢。
- 做了什么：
  1. **`materials.toml` 每层新增 `rigid_debris`**（可选、缺省 false ⇒ **不升 `schema_version`**）：
   **岩 = true**（刚性）、草 / 土 / 沙 = false（散体）。
  2. **`world/dig/volume_collapse.*`**：`CollapseUnit::rigidDebris` 由**多数材质**决定（与摩擦 / 弹性同源，
   同票取更小槽位 ⇒ 确定）；新增 `hullPoints` 的**局部 AABB** 与两个纯函数
   `LocalAabbContainsPoint`（光球命中判据）/ `UnitWorldAabb`（唤醒判据）；回写对**散体**做**接地沉降**
   （下方为空则沿本列下落，最多 32 格；走满上限记 `stuckVoxels` 并由调用方告警），
   `CollapseWriteback` 增 `settledVoxels` / `stuckVoxels`。
  3. **`game/rigid_collapse.*`**：刚性整体落定 ⇒ **不回写、保留刚体 + 网格 + 位姿**（`retained`），
   形状与外观保持不变；新增 `ContainsRetainedPoint` / `RetireRetainedAt`（光球命中）/ `RetireOldestRetained`
   （池满腾位）/ `AwakenIntersecting`（块碰撞体重建后唤醒相交残骸）；池 4 → **16 槽**。
  4. **`engine/physics/physics_world.*`**：新增 `ActivateBody`（Jolt 不会因"脚下静态形状被改写"自动唤醒休眠体）。
  5. **`game/main.cpp`**：网格池 16 槽；**光球查询把"保留中的岩石残骸"算作实心**（否则会穿过它），
   命中后**先惰性体素化**再爆炸（⇒ 岩石仍可挖）；每次爆炸的抽出上限仍由 `max_active_units` 控制；
   爆炸 / 落定日志增"保留中岩石残骸 / 自由槽位""接地沉降 N 个"。
- 为什么：
  1. 业界同类做法正是**按材质分流**：Teardown 的大块碎体落地后保留为几何体、碎渣才回写体素；
     UE Chaos / Blast 的碎块**始终是几何体**。而"岩石落地不变形"在体素世界里只有一条路：**不体素化**。
  2. 三个连带问题必须一起解，否则世界不自洽（「世界内一致性」）：
     ① 光球会穿过"看着是实心"的岩石 ⇒ 命中判定必须认得保留残骸；
     ② 岩石被挖掉支撑后**必须还会掉** ⇒ 需要显式唤醒（Jolt 的休眠体不会自己醒）；
     ③ 保留 = 不释放网格槽位 ⇒ 池会耗尽（"炸了不塌"）⇒ 扩池 + 池满按**最旧优先**腾位（确定序）。
  3. **散体沉降**用的是"沿本列下落到有支撑"（与 T33 的支撑判据同源），而不是重新做物理模拟：
     落定时刚体已被判为静止，落点与真实支撑的差通常 ≤ 2 格，一次自下而上的扫描即收敛且确定。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0，**零错误零警告**。
  2. **测试**：`ctest --preset debug` → **279/279 passed**（新增 5 项）。**新测试抓到一个真 bug**：
     沉降"边下落边腾空"最初写成"落完再腾空"，导致列里留下**一串本应为空的实心格**（假支撑）⇒
     同列上层体素停在半空。用例 `GranularWritebackSinksVoxelsOntoSupport` 断言
     "区域内不存在下方为空的实心体素" + 逐高度计数（105/106 = 64、107~110 = 0）后暴露并修复。
  3. **冒烟（Debug，前台，真实操作 —— 自动化鼠标 / 键盘注入）**：走到塔基旁连发 ⇒
     **`岩石残骸保留（T46）：体素 2057 个、倾角 6°`（**不回写**）**，后续又保留了 436 / 28 / 772 / 706 体素的岩块
     （倾角 4° / 32° / 36° / 15°）⇒ **岩石残骸保住形状**；3 次落定回写含**接地沉降 48 / 36 / 51 个**（散体）；
     `岩石残骸被唤醒（T46）…` 共 **61** 条（块碰撞体重建 ⇒ 唤醒）；**池 16 槽**（启动日志）、
     `保留中岩石残骸 1 个 / 自由槽位 14~15`；**无 ERROR、无 SDL 报错**。
  4. **门禁**：`check-banned-identifiers.ps1` → `scanned 115 file(s), 0 violation(s)`、`PASS`。
  5. **帧时间（如实记录）**：12 条尖峰（33.4 ~ **53.5** ms）。53.5 ms 那条是**爆炸当帧**
     （逻辑 49.04 + UI 0.18 + 提交 0.68 ms，≈全部在 CPU 侧；与 T43 实测的"逻辑 49.77 ms"同一量级）
     ⇒ **判据"无 > 50 ms 单帧"本轮未满足**，根因是 T33/T37 的既有成本（大分量抽出 + 刚体化 + 延后队列推进），
     与 T46 无关（T46 每帧只多一次 OBB 点测试与一次 ≤16 体素的相交判定）。
- 下一步 / 遗留：
  1. **待人工目视 / 未端到端验证**：① **光球命中保留残骸 ⇒ 惰性回写**这条路径本轮冒烟**未实际命中**
     （多次瞄准都先打到地形或落空；判据由单测 `RetainedPointTestFollowsPose` + 代码路径覆盖）⇒
     人工验收步骤：对着地上的岩石残骸连发，应看到 `光球命中岩石残骸 ⇒ 惰性体素化（T46）` 并随后被挖掉。
  2. **池满腾位未触发**（本轮最多同时保留 6 个，池有 16 槽）⇒ 人工可用"连续炸 16 处不同结构"触发。
  3. **已知代价**：① 凸包填平的后果**被永久化**（岩石残骸是等值面外观 + 凸包物理，不会再被回写修正）；
     ② 单槽容量上限导致的外观截断**被永久化**；③ 网格池 16 槽 ≈ **+51 MB 显存**
     （启动日志实测"16 个槽位 × 每槽约 3.18 MB"；加上材质 / 阴影 / MSAA 后总显存记账见启动行）；
     ④ 岩石残骸的**支撑消失**靠"唤醒"兜底，而不是统一进入支撑检查。
  4. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + T33 + T42 + T43 + **T46**）**尚未提交**。

## 2026-09-27  T46 缺陷修复：**分流判据改按"表面材质" + 碎块外观网格自闭合**

- 背景（项目所有者实测两条，均判定为**确认为缺陷**）：
  ① "在之前的岩石掉落逻辑修改后，当前**白色的岩石掉落后效果和褐色的泥土表现状态一样**，这是 bug 吗"；
  ② "掉落的物体**部分面没有颜色**，从部分角度看过去是**透明的，但是物理碰撞真实存在**"。
- 判定依据（契约 + 机制）：
  1. 契约 = 项目所有者已定的硬约束"**岩石落地后不允许任何形状变化**" + [ADR 0017](../adr/0017-landing-by-material-rigid-vs-granular.md) 决策一；
     机制 = 分流判据取的是"**全体体素的多数材质**"，而陡壁**内部**体素是按该列地表"表层 → 次表层"**派生**的
     （[ADR 0014](../adr/0014-voxel-material-index.md)）⇒ 一块"看着是白岩"的碎块被判成**散体** ⇒ 走了泥土那条路
     （体素化回写 + 与地面量化融合）⇒ 表现与泥土一致。
  2. 契约 = 同一物体在"掉落中 / 落定"两态的外观网格**必须完整闭合**（渲染与碰撞同源，不许"看着透、走不过去"）；
     机制 = Surface Nets 的发射规则是"**每条网格棱由 u/v 下侧的 cell 发射一次**"，
     **相邻块会替对方补上区域边界那圈四边形**；而**独立碎块没有邻居** ⇒ 边界圈的面**整块缺失**
     （实测：四周皆空气的悬空石板有 **36 条边界边**，三角形 192 个）⇒ 从外侧能看穿，但凸包碰撞体完整。
- 做了什么：
  1. **`world/dig/volume_collapse.{hpp,cpp}`**：
     - `CollapseUnit` 增 `surfaceMaterialCounts[kMaterialSlotCount]`（**表面 cell 材质直方图**）与
       `patchIsUnit`（补丁**归属掩码**）；
     - 新增 `CountUnitSurfaceMaterials()`（与网格器**同一批 cell**、实体侧 8 角众数、同票取更小槽位）；
       `rigidDebris` 判据改为"**表面出现任何刚性材质即判刚性**"，无表面 cell 时才退回多数材质；
     - `UnitPatchSampler` 改为"**采样索引与补丁索引 1:1**"（不再 `+1`），且"**非本整体的实心采样当空气**"；
     - `BuildCollapseUnitMesh` 区域尺寸改 `patchSize − 1`、偏移改 `bounds.min ± 1`（**多覆盖一格 cell**）。
  2. **`game/main.cpp`**：刚体化日志增"落地口径 …（**表面**材质 cell 数：岩 N / 土 / 草 / 沙）"。
  3. **`tests/volume_collapse_test.cpp`**：新增 helper `BuildAttachedShelfScene()`（地板 + 立柱 + **仍连着未塌岩体的悬挑板**）
     与 `CountBoundaryEdges()`（统计"只被 1 个三角形用到"的无向边）；重写 `RigidityFollowsTheShownSurface`
     （含回归用例"**体积里土占多数、皮上有岩 ⇒ 必须判刚性**"）；新增 `DetachedUnitMeshIsWatertight`、
     `AttachedUnitMeshIsWatertight`（断言 `CountBoundaryEdges == 0`）。
- 为什么：
  1. 判据必须取"**玩家真正看到的那些面**"：渲染网格的顶点材质本来就是"实体侧 8 角众数"，
     判定口径与表现口径**同源**才不会"看着像岩、被判成土"。通用经验见 `docs/learning-notes.md` Q21。
  2. 修网格闭合时**不动"原本暴露在空气中的面"**：那些面两侧采样与改前完全相同 ⇒ 顶点**逐位不变**，
     `UnitMeshIsSubsetOfTheTerrainSurfaceItWasCutFrom`（与地形同源）仍通过 —— 否则会以"修透明面"为代价
     破坏 T42 建立的"掉落中与地形同一套等值面"口径。
- 验证：
  1. **构建**：`cmake --build --preset debug` → `ninja: no work to do.`（`/W4` + `/WX`，零错误零警告）。
  2. **测试**：`ctest --preset debug` → **281/281 passed**（279 → 281：重写 1 项 + 新增 2 项；
     两项新断言均要求 `CountBoundaryEdges == 0`：**四周皆空气的悬空石板** 与 **仍连着未塌实心的悬挑板**
     —— 后者在修前实测有 **18 条边界边**，且前提断言确认"断口确实存在"）。
  3. **门禁**：`check-banned-identifiers.ps1` → `scanned 115 file(s), 0 violation(s)`。
- 下一步 / 遗留：
  1. **本轮只跑自动化测试，未做真实操作冒烟** ⇒ 两条缺陷的**目视判据待人工确认**（见阶段计划「下一步」第 1 条新增的 ④⑤）：
     ① 白色岩块掉落后**保持原形状、不熔进地面**（日志 `落地口径 = 刚性…（**表面**材质 cell 数：岩 N / 土 …）`）；
     ② 掉落的碎块**从任何角度看都不透明**。
  2. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + T33 + T42 + T43 + **T46 + T46 缺陷修复**）**尚未提交**。

## 2026-09-27  四条实测缺陷的判定与决策：**结构求解与破坏的表示守恒**（ADR 0018 / T47~T50）

- 背景（项目所有者实测四条，**要求先分析、暂不改代码**，并给出总口径："长条物体里**岩石和泥土都存在的**
  —— **泥土掉落后一直下坠到存在支点是对的，岩石应该始终不变**"）：
  ① 长条两端支点都破坏后，**中段不落**；② 破坏长条时它**又发生形变**；
  ③ 部分情况下**仍有面透明**；④ 掉落中的整体**打不中**（落地静止后才能打中）。
- 做了什么（**本轮只做判定 + 决策留档，未改任何代码**）：
  1. **逐条真伪判定（契约 + 机制，均已定位到代码）**：
     - BUG1 **确认为缺陷**：契约 = "承重被破坏 ⇒ 上部不得悬空"（ADR 0015 决策一）；机制 = 支撑检查是
       **固定窗口 + 一次判定**（水平 `reach = ceil(max_cantilever_blocks) + 1 = 5` 格）⇒ 距任何破坏点 > 5 格的中段
       **从不进入任何一次求解**；`AwakenIntersecting` 又只救济**已抽出**的保留残骸。
     - BUG2 **确认为缺陷**：契约 = "岩石落地后不允许**任何**形状变化"（ADR 0017 决策一；ADR 0017 曾把
       "被打中时量化一次"登记为可接受代价，**本次被项目所有者否定**）；机制 = `RetireRetainedAt` ⇒
       `WritebackCollapseUnit` **体素化硬量化**（体素中心 → 最近整数格 + 落位偏移找空位）⇒ 整块轮廓被改。
     - BUG3 **证据不足 ⇒ 先取证**：已**排除**"材质缺失导致透明"（片元 `o_color.a` 恒为 1 ⇒ 只能是**缺面**）；
       余三个候选：① 网格池容量截断（按四边形丢尾部，对保留残骸是**永久洞**）；② 仍有未覆盖的敞口场景；
       ③ 地形 ↔ 体积交界缝。
     - BUG4 **确认为缺陷**：契约 = 光球不得穿过"看着是实心"的物体；机制 = `GameOrbWorldQuery::IsSolid` 只认
       **`retained == true`** 的残骸，而飞行中的整体**体素已被抽出**（体积里是空的）⇒ 被判为空气 ⇒ 光球穿过。
  2. **共同结论**：四条都指向 ADR 0017 引入的"刚体网格 ⇄ 地形体素两种表示来回切换"——
     它让**结构求解、破坏、命中**三处都露了馅。业界（Teardown / UE Chaos / Red Faction）的一致做法是：
     **范围由结构拓扑决定、破坏在单一表示内完成、命中交给物理引擎**。
  3. **落盘**：新增 [ADR 0018](../adr/0018-structural-support-and-representation-preserving-destruction.md)
     （四条决策 + 一条诊断）；`docs/plans/v0.1.md` 新增 **T47~T50** 任务行与 **3A 基线三问**；
     `docs/adr/README.md` 索引加 0018 行并回填 0015 / 0017 头部互链（未回改其正文）。
- 为什么（**决策口径全部来自项目所有者的四问四答**，均为选项中的推荐项）：
  ① BUG2 ⇒ **在碎块自身补丁上雕刻 + 重网格**（同一份 Surface Nets ⇒ **未触及区域顶点逐位不变**，
     把"岩石始终不变"从"靠自觉"变成"可证伪"）；备选"预切分 Voronoi 碎块"登记为后续（代价 = 新系统）。
  ② BUG1 ⇒ **连通域洪泛到结构边界**（scope 由拓扑决定；**物理判据不变**，避免与 ADR 0015 冲突）；
     备选"只放大窗口"被否（只把失效距离推远）；"持久结构图"登记为切换条件。
  ③ BUG4 ⇒ **接入 Jolt 物理场景查询**；副产品 = 覆盖"飞行中"这一态、用**真凸包**（一并消掉 ADR 0017 后果 5）。
  ④ 材质共存 ⇒ **按材质边界拆子块**（岩子块保留几何体、土子块回写 + 接地沉降）——
     现有"整块一个 `rigidDebris` 布尔"在结构上表达不出"同一条长条里岩土行为不同"。
- 验证（本轮只有文档，无可运行产物）：
  1. **未改任何代码** ⇒ 不跑构建 / 测试（现状仍为 `ctest --preset debug` **281/281**、门禁 0 违规）。
  2. 判定依据全部可在代码中复核：`world/dig/volume_collapse.cpp`（窗口与支撑 BFS）、
     `game/rigid_collapse.cpp`（`RetireRetainedAt` / `AwakenIntersecting` / `BuildUnitMesh` 截断）、
     `game/main.cpp`（`GameOrbWorldQuery::IsSolid`）、`assets/shaders/mesh.frag`（`o_color.a` 恒为 1）。
- 下一步 / 遗留：
  1. **开工顺序 = T47（只插桩取证 BUG3 根因）→ T48（命中接入物理查询）→ T49（支撑求解连通域化）→
     T50（雕刻自身补丁 + 按材质拆子块）**；T47 开工前须确认"本轮只取证、暂不动外观网格"这一过渡口径。
  2. **T49 / T50 属"重活"**（洪泛 + 雕刻 + 重网格）⇒ 与 **T45（帧尖峰打点的覆盖缺口）** 一起做，
     否则无法自证没有制造新卡顿（SKILL 第四节 / M9 的"所有重活都必须离开渲染帧"）。
  3. **待项目所有者提供的一条证据**：BUG3 的透明面出现在**哪里**（残留岩石上 / 掉落中的长条上 /
     普通地面 / 可挖区域边界）+ 是否伴随"超出网格池容量"的 WARN —— 有这条就能一次定位。
  4. 本批改动（T36~T39 + M8 / M9 + T35 + T41 + T33 + T42 + T43 + T46 + T46 缺陷修复 + **本轮文档**）**尚未提交**。

---

## 2026-09-27  T47 落地：**BUG3 取证 —— 整体外观网格闭合自检**，结论 = 候选③（地形 ↔ 体积交界缝）

- 背景：项目所有者实测"部分情况下还是存在某个面没有颜色、直接透明显示了"（BUG3，根因未定）。开工前按
  SKILL「降级必须先问 / 需求受理」**先提问**，得到三条口径：① 本轮**只取证、不动外观网格代码**；
  ② 透明面出现在 **掉落中的长条 / 落地后的长条 / 可挖区域边界**（**不含**残留岩石、普通地面）；
  ③ 是否伴随"超出网格池容量"的 WARN：**没注意**（⇒ 由本次插桩的一次冒烟判定）。
- 做了什么：
  1. **共用判据实现**：`world/dig/volume_mesher.*` 新增纯函数 **`CountBoundaryEdges(MeshData)`** ——
     统计"不是恰被 2 个三角形共用"的无向边条数（闭合曲面恒 0）。实现改用**排序 64 位边键 + 线性扫描**，
     无节点分配、与顺序无关（红线 7）。**测试改为复用同一实现**（删掉 `tests/volume_collapse_test.cpp`
     里那份重复的 `std::map` 版本）⇒ 口径只有一处。
  2. **插桩（`game/rigid_collapse.cpp`）**：`BuildUnitMesh` 在**截断前**与**截断后**各量一次边界边，
     `Spawn` 时记进 `ActiveCollapseUnit`，`RetireRetained`（惰性回写 / 腾位）时据此回报"玩家看到的缺面来自这里"。
     WARN 的**成因判定**直接对应 ADR 0018 的三个候选：`full==0 且截断` ⇒ **①容量截断**；
     `full!=0` ⇒ **②/③ 网格生成本身**；两者都有 ⇒ 并存。
  3. **修掉容量截断 WARN 的一处错报**：原实现打印的是**截断后**的 `indices.size()/6` 与顶点数（于是
     "原始规模"永远等于"保留规模"，无法判断超了多少）；现如实打印**原始四边形 / 顶点数**、每槽上限，
     并补上**体素数 / patch 尺寸**（T47 的要求）。
  4. **回归测试**（+2）：`VolumeMesher.CountBoundaryEdgesDetectsOpenMesh`（球面 → 0；**删一个三角形 → >0**
     ⇒ 判据有区分度）、`VolumeCollapseLanding.LongBarUnitMeshIsWatertight`（**长条**场景 30 × 2 × 2、
     长径比 15:1，抽出后整体网格必须闭合）。
- 为什么（**证据链**，详见计划 T47 行）：
  - **候选① 排除**：容量截断 WARN 在本轮冒烟**从未出现**（devlog 里 T43 / T46 两次真实爆破冒烟也都记录为
    "无 ERROR、无 WARN（除帧尖峰）"），且真实整体的规模远低于上限（每槽 98304 顶点；本轮最大整体仅 360 体素）。
  - **候选② 未复现**：本轮真实爆破产生了 **360 体素岩整体、54 / 245 体素散体整体**，并走通了
    `光球命中岩石残骸 ⇒ 惰性体素化`（覆盖 `RetireRetained` 路径）——**全部没有 T47 WARN**（即截断前后都是 0 条边界边）；
    headless 侧新增的"长条"用例同样判 0。
  - **候选③ = 唯一与实测位置自洽者**："可挖区域边界"这一条只有它能解释 —— [ADR 0011](../adr/0011-layer-transition-volume-takeover.md)
    已登记"区域内由体积接管地表、边界一圈**共面重叠**（两者在平面地形上逐字重合）⇒ 可能出现 z-fighting / 缝"。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **283/283 passed**（281 → +2）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 115 file(s), 0 violation(s)`、`PASS`。
  4. **冒烟（Debug，自动化注入鼠标 / 键盘，真实爆破，见下"测试要点"）**：命中可挖山体 11 次 ⇒
     产生 1 个 **360 体素岩整体**（表面材质 cell：岩 876）+ 54 / 245 体素散体整体 + 1 次
     `光球命中岩石残骸 ⇒ 惰性体素化（T46）：回写 360 个`；**WARN 仅 2 条「帧尖峰」**（35.7 / 40.6 ms），
     **无"超出网格池容量"、无"整体外观网格缺面（T47）"、无"保留残骸退役自检（T47）"**。
- 下一步 / 遗留：
  1. **本轮未复现"塔长量级"的长条**：自动化瞄准没走到地标塔基（走到了可挖山体），故 `3×5×138` 这类
     **超长整体**未被判定。插桩已在位 ⇒ **针对地标塔再跑一次**即可把该形状一并判定（有 T47 WARN ⇒ ②，无 ⇒ ③）。
  2. **候选③ 的处置归入 [ADR 0011](../adr/0011-layer-transition-volume-takeover.md)**（层间过渡），本轮**不动外观网格**（所有者确认）。
  3. BUG3 的**视觉确认**（透明面是否真的出现在可挖区域边界那一圈）本环境无目视能力，须项目所有者复核。
  4. 本批改动（T47）**尚未提交**。

---

## 2026-09-27  T31 落地：**材质坚固度 × 伤害预算的逐格³ 挖除**（器物部分仍待层 ③）

- 做了什么（**地形体量部分**；口径见 [ADR 0013](../docs/adr/0013-destructible-elements.md) 与
  `references/destructible-elements.md`）：
  1. **`assets/config/materials.toml`（`schema_version` 4 → 5）**：每层新增**必填** `toughness`（点/格³）——
     草 2 / **土 3** / **岩 5** / 沙 2。**必填 ⇒ 破坏性变更 ⇒ 升版**（缺该字段会直接报错，而不是悄悄退化成
     "到处一样硬"）。土 / 岩取项目所有者给的原话锚点；**草与沙由本实现补齐**（真实世界：松散表层 / 砂更易挖）。
  2. **`assets/config/projectiles.toml`（`schema_version` 1 → 2）**：`damage = 10.0`（点）。
  3. **新增 `assets/config/destruction.toml` + `world/dig/destruction_table.*`**：`points_per_cubic_block = 271`
     （**单一手感旋钮**）、`prop_damage_threshold` / `prop_broken_tint`（**器物参数先落表**，待层 ③ 消费 —— 见
     ADR 0013 §三 / §四）；缺失 / 越界 / 版本不符一律抛异常。
  4. **`world/dig/dig_volume.*`**：新增 **`CarveByDamage(center, radius, budgetPoints, …)`** ——
     候选 = 半径内的**格³**（格心距），按「距离升序 → (x, y, z) 升序」的**确定序**逐格³ 扣减该格材质的
     `toughness`（四舍五入为整数点），**余额不足即停**；`toughness <= 0` / `indestructible` / **已是空气**的格
     **不消耗预算**；随后以"最后一格的半径"为**光滑球面半径**栅格化（球面本身光滑 ⇒ 无台阶感），
     不可破坏材质的采样保持原状。`CarveSphere` 降级为该栅格化的半径底层（测试 / 工具用，语义不变）。
  5. **`game/main.cpp`**：区域内破坏改走 `CarveByDamage`，预算 = `damage × points_per_cubic_block`；
     启动期加载破坏表并打日志；爆炸日志新增「伤害 N 点 ⇒ 预算 M 点」。
- 为什么：
  1. 项目所有者要求"不同元素可破坏性不同 + 材质按坚固度抵抗伤害"；现状是**半径驱动**（到处挖一样大的坑），
     与真实（泥软岩硬）及业界（Minecraft / 7 Days to Die 的方块硬度、UE Chaos 的 Damage Threshold、
     NVIDIA Blast 的 material toughness）都不符。
  2. **逐格扣减**而不是"先算半径再挖球"：后者在混合材质处**不会**出现"软的先被挖掉、硬的留在原地"。
  3. 预算用**整数点**、次序**确定**（红线 7）：浮点累加会让结果随编译选项漂移。
  4. "空 / 不可破坏的格不消耗预算"是**语义正确性**要求：在空气中爆炸不该空烧预算，不可破坏材质
     更不能因为它而少挖旁边的软材质（ADR 0013 §二.3）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **289/289 passed**（281 → +6）：
     `CarveByDamageMatchesDirtAnchor`（**泥土锚点**：伤害 10 × 271 = 2710 点 ÷ 3 ⇒ r ≈ 6，允许 ±1）、
     `CarveByDamageInRockIsSmaller`（**纯岩**同预算 ⇒ r ≈ 5，构成"岩比泥难挖"的对照）、
     `CarveByDamageSkipsIndestructibleMaterial`（把岩的 `toughness` 置 0 的临时表 ⇒ **零改动**）、
     `CarveByDamageIsDeterministic`（脏块 + 密度逐位相同）、`DestructionTable.LoadsShippedTableAndMatchesDefault`、
     `DestructionTable.RejectsInvalidConfig`（换算系数 0 / 缺字段 / 版本不符 / 颜色越界各自抛异常）。
     另同步升版代价：材质 / 弹丸表的相关测试与 `volume_collapse_test` 用的临时材质表一并更新
     （`TerrainMaterial.SchemaVersionMustBeFour` → `SchemaVersionMustMatch`）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 117 file(s), 0 violation(s)`、`PASS`。
  4. **游戏内运行证据：缺失（如实登记）** —— 两次自动化冒烟（注入鼠标 / 键盘）**输入未送达游戏焦点**
     （日志停在"世界就绪"，无任何爆炸），故"同一发球在泥 / 岩里腔体大小不同"目前只有**单测 + 启动日志**
     佐证，**待人工目视验收**（见下）。
- 下一步 / 遗留：
  1. **器物部分未做**（前置 = [ADR 0004](../docs/adr/0004-hybrid-layered-world-representation.md) 层 ③：
     EnTT + Jolt 的物件与建造层，尚未建）⇒ 破坏表的 `prop_damage_threshold` / `prop_broken_tint` 目前
     **只解析 + 校验 + 打日志**，不消费；T31 的器物判据（`Intact → Broken`、几何不可变、变黑占位）仍**未实现**。
  2. **地表爆破（区域外）仍按半径剖面**：ADR 0013 §二.5 只把"地形体量"的结算改为预算，地表坑是高度场笔刷
     的另一条路径，未纳入材质坚固度（已在计划 T31 行登记为已知限制）。
  3. **待人工目视**：对着可挖山体（岩）与平地地下（土）各打一发，比对**坑体明显一大一小**（岩 < 土）；
     日志应出现 `伤害 10 点 ⇒ 预算 2710 点`。
  4. 本批改动（T47 + T31）**尚未提交**。

---

## 2026-09-27  T48 落地：**命中判定接入物理场景查询**（修 BUG4"掉落中的整体打不中"）

- 背景（BUG4，[ADR 0018](../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）：`GameOrbWorldQuery::IsSolid`
  只认 `retained == true` 的残骸，而**掉落中的整体体素已被抽出**（体积里是空的）⇒ 被判定为空气 ⇒
  光球**穿过去**，落地静止（变 `retained`）后才打得中。
- 做了什么：
  1. **`engine/physics/physics_world.*`**：新增 **`RayCastDynamic(from, to) → {hit, point, body}`** ——
     Jolt `NarrowPhaseQuery::CastRay` + **只放行动态刚体**的 `BodyFilter`（静态地形 / 体积 / 角色不参与，
     它们由玩法层按"谁画谁挡同源"的现行口径判定）；命中点是**真实凸包表面**（不再是手工 OBB），
     并把命中刚体的**句柄**回报给玩法层。**成本与活跃刚体数无关**（宽相位 + 窄相位查询，有界）。
  2. **`game/orb.hpp`**：`IOrbWorldQuery` 新增 `SegmentHitsDynamic(...)`（**给默认实现返回 false** ⇒
     只关心地形 / 体积的测试桩无需改动）；`OrbHit` 增 `body` 字段；`MarchRay` 在原有的 0.25 格点采样之外
     **追加一次动态线段查询**，并与地形命中**按距离取更近者**。
  3. **`game/main.cpp`**：`GameOrbWorldQuery` 改持 `PhysicsWorld` 并实现 `SegmentHitsDynamic`；
     `IsSolid` 里的"保留残骸 OBB 分支"**删除**（否则 OBB 会先于凸包命中 ⇒ 命中点不是真实表面）。
     命中后由 **`hit.body`** 直接定位"是哪个整体"，调 `RigidCollapseRuntime::RetireBody`。
  4. **下线手工 OBB 判据**：`RigidCollapseRuntime::ContainsRetainedPoint` / `RetireRetainedAt` 删除，
     改为 **`RetireBody(句柄, …)`**；`world/dig/volume_collapse.*` 的 `LocalAabbContainsPoint` 一并删除
     （`UnitWorldAabb` 保留 —— 它仍用于"块碰撞体重建后唤醒相交残骸"）。
- 为什么：业界（UE `LineTrace` / Unity `SphereCast`）的命中判定一律走**物理场景查询**，而不是游戏层手工记账。
  这一改动同时消掉两个副作用：**覆盖"飞行中"这一态**（无需任何状态记账）、命中用**真凸包**而非 OBB 近似
  （ADR 0017 后果 5 随之作废）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/WQ` 级别不变）。
  2. **测试**：`ctest --preset debug` → **290/290 passed**（新增 `PhysicsBody.RayCastDynamicHitsMovingHull`）：
     立方体凸包自由下落 **0.5 秒后仍在运动**（前置断言 `v.y < -1`），此时沿竖直方向打一条 10 格长的射线 ⇒
     **命中**、报告**同一个句柄**、命中点落在**凸包表面**（盒心下方 1 格 ±0.2，而不是盒心或 OBB 外沿）；
     另两条反例（远射线 miss、退化线段 miss）确保"命中"有区分度。
     原 `VolumeCollapseLanding.RetainedPointTestFollowsPose` 精简为 `UnitWorldAabbFollowsPose`（只保留仍在用的判据）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 119 file(s), 0 violation(s)`、`PASS`。
  4. **游戏内运行证据：缺失（如实登记）** —— 与 T31 同一原因（自动化冒烟输入未送达游戏焦点）⇒
     "掉落中直接打中整体"待人工目视，验收步骤见下。
- 下一步 / 遗留：
  1. **待人工目视**：把地标塔基座轰断后，**在它翻滚下落的过程中**再打一发 —— 期望**当帧就命中并爆炸**
     （日志出现 `爆炸…` 而不是等它落定），且 `光球命中岩石残骸 ⇒ 惰性体素化（T46 / T48）` 仍会在命中保留残骸时出现。
  2. `RayCastDynamic` 只查**动态刚体**：静态地形 / 体积仍由玩法层的密度与高度场口径判定（范围界定见 ADR 0018 决策三）。
  3. 本批改动（T47 + T31 + T48）**尚未提交**。

---

## 2026-09-27  T45 落地：**帧尖峰打点的覆盖缺口**（把未计时区显式量出来）+ 三个"游戏内证据"补记

- 做了什么（`game/main.cpp`）：新增三段相位计时并全部写进尖峰 WARN ——
  **① 动态顶点上传**（主角 + 光球）、**② uniform 构建**（材质 / 光照 / 相机 / 渲染原点 / 阴影级联）、
  **③ 限帧**（`FrameLimiter::Throttle`）；日志改为
  `逻辑 + UI + 渲染提交 + 动态上传 + uniform + 限帧 = 合计，**未计时** = 帧时间 − 合计（钳到 ≥ 0）`。
  节流口径不变（≥200 ms），观测本身不制造新卡顿。
- 为什么：T43/T46 的尖峰里"三相 CPU 只占 22.2 ms、≈39 ms 归属不明"—— 那部分**被无声吞掉**，
  导致"主要受限在谁"的结论可能是误判（SKILL 第四节「观测先于结论」）。
- 验证（冒烟实测，节选）：
  ```
  帧尖峰 38.6 ms：逻辑 0.31 + UI 0.29 + 渲染提交 1.00 + 动态上传 0.07 + uniform 0.19 + 限帧 0.00 = 1.86，**未计时 36.70** ms
  帧尖峰 41.1 ms：逻辑 33.97 + UI 0.00 + 渲染提交 0.82 + 动态上传 0.13 + uniform 0.18 + 限帧 0.00 = 35.10，**未计时 6.02** ms
  ```
- **结论（推翻了 T43/T46 的假设）**：原先怀疑的"顶点上传 / uniform / 限帧"三段在尖峰帧上**合计 ≤ 0.4 ms**
  ⇒ **不是**那 ≈39 ms 的来源。残余现在被**量化**（36.7 ms）但**仍未归属**；已登记为后续第 8 条
  （继续细分"呈现 / 事件 / 其它"，并复核 `stats.frameSeconds = clock.DeltaSeconds()` 的取值口径）。
  另一条有用的实测：**爆炸当帧的 33.97 ms 落在「逻辑」段**（与 T43 记录的"爆炸当帧成本"一致）。
- 顺带补记**三条游戏内证据**（同一轮冒烟首次成功注入输入，此前 T31/T48 只差这条）：
  1. **T31**：日志出现 `爆炸：… 半径 6.0 格（伤害 10 点 ⇒ 预算 2710 点）` ⇒ 伤害预算路径在真实玩法里已生效。
  2. **T31 的代价（如实登记）**：「挖除」段由改前的 0.19~0.66 ms 变为 **4.49~4.72 ms** ——
     逐格³ 扣减要枚举球内格³、按确定序排序、并逐格查材质（列派生的代价）；仍在一帧预算内
     （爆炸当帧总计 ≈ 8~18 ms），**但确实是本次引入的成本上升**；如需可加"按列缓存材质"优化（登记待办）。
  3. **T33/T46 路径**：出现 `倒塌整体已刚体化（T33）：体素 292 个…落地口径 = 保留几何体（T46）`
     ⇒ 保留残骸路径正常；**但"光球命中保留残骸 ⇒ 惰性体素化"本轮仍未在日志中出现**（未打中），
     该项仍待人工确认。
- 下一步 / 遗留：T45 的**残余归属**（见结论）与 T31 的**挖除段耗时**（见证据 2）都已登记；
  本批改动（T47 + T31 + T48 + T45）**尚未提交**。

---

## 2026-09-27  T49 + T50 落地：**支撑求解连通域化**（修 BUG1）+ **破坏时不切换表示**（修 BUG2 + 材质分区）

- 背景（[ADR 0018](../adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一 / 二 / 四）：
  **BUG1** = 支撑检查是"固定窗口 + 一次判定"，水平窗口只有 `ceil(max_cantilever) + 1` 格 ⇒ 距任何破坏点都超过
  窗口的**中段从不进入任何一次求解** ⇒ 长条两端支点都被炸断后中段**永远悬空**；
  **BUG2** = 光球命中保留残骸走"惰性体素化回写"（体素中心 → 最近整数格 + 找空位）⇒ 整块轮廓被改
  （"岩石不允许任何形状变化"被违背）；**材质共存** = `rigidDebris` 是整块一个 bool ⇒ 一条长条里岩土行为无法各自正确。
- 做了什么：
  1. **`world/dig/volume_collapse.cpp`（T49，scope）**：抽出 `AnalyzeWindow(seed, spec, region, narrowByDomain)` ——
     一次算完 **① 纵向接地 → ②（可选）连通域洪泛 → ③ 同层悬挑传播**（**判据一字未改**）；
     **洪泛必须排在悬挑传播之前**（此刻 `reached` 恰好只表示"纵向接地"，被"拱效应"救回的体素才不会把域切碎）。
     窗口从「被改动采样 ± (悬挑 + 1) 格」起步，**只向触界的那些侧**把"离种子的距离"翻倍
     （细长结构保持细长窗口 ⇒ 每轮成本与结构形状成正比，而不是退化成一个大立方体）；
     终止条件三条：域被窗口包住（收窄成立）/ **累计**采样超 `kMaxRegionSamples`（8M）/ 窗口已覆盖整个体积
     的水平范围但域仍触界 ⇒ 后两者**告警 + 保守回退固定窗口**（所有者确认过的显式例外）。
     分组条件多了"**必须在连通域内**"（窗口里与本次破坏无关的其它悬空结构不该被连带带塌）。
     顺带把"包围盒 / 质心 / 凸包点集 / 局部 AABB"抽成 `RebuildUnitGeometry(unit, keepCentroid)`。
  2. **`world/dig/volume_collapse.*`（T50，表示守恒）**：新增
     **`CarveCollapseUnitPatch`**（在碎块**自身 `patchDensity`** 上挖球 —— 复用 `RasterizeBall` 的同一套数学：
     `clamp(round((半径 − 距离) × 每格单位))` 取 max、`kCarveSdfBandBlocks` 过渡带、不可破坏材质保持原状；
     只写 `patchIsUnit != 0` 的采样）与 **`RefreshCollapseUnitFromPatch`**（由补丁重算体素清单 / 包围盒 / 凸包 /
     质量与落地口径，**质心保持不变** ⇒ 局部坐标系不变 ⇒ 刚体与网格原地不动）；`kCarveSdfBandBlocks` 提到
     `dig_volume.hpp` 作**唯一口径**（地形挖除与碎块补丁雕刻共用）。
  3. **`world/dig/volume_collapse.cpp`（T50，材质分区）**：新增 `SplitUnitByMaterial` ——
     抽出的连通分量按**材质一致性**（同槽位且 6 邻域连通）切成子块（扫描序 = 体素的 x → z → y 升序 ⇒ 划分确定；
     整块同材质走快路径）；`ComputeUnitPhysics` 的落地口径改为**该子块自己的材质**的 `rigid_debris`
     （`surfaceMaterialCounts` 退化为**诊断**）；`ComputeUnitPhysics` 里的**爆心冲量**拆成独立的 `ComputeUnitImpulse`
     （只在"抽出那一刻"算一次；雕刻后重算不重算冲量）。
  4. **`game/rigid_collapse.*`（T50，接线）**：新增 **`CarveBody(句柄, 中心, 半径, spec, materials, …)`** ——
     雕刻 → 按剩余体素在**当前姿态**下原地重建刚体（先建后删，不留一帧真空）→ 复用同一网格槽位
     （`UpdateMeshGeometry`）、同一帧内完成；剩余 ≤ `debris_delete_max_voxels` / 凸包点数 < 4 / 重建失败 ⇒
     **删除整个整体**兜底（`RetireCarved`）。**`RetireBody`（命中即惰性体素化）下线**；
     `RetireOldestRetained`（池满腾位）保留 —— 它不是破坏事件，而是"把位置让给新倒塌"。
  5. **`engine/physics/physics_world.*`**：`ConvexHullDesc` 新增 **`rotation`**（默认单位四元数 ⇒ 与 T33 逐位一致），
     创建刚体时把质心偏移**先旋转到世界方向** —— 这是"雕刻后原地重建"的必要条件（已倒下的岩石不该因为重建而回正）。
  6. **`game/main.cpp`**：命中动态刚体改调 `CarveBody` 并打日志；爆炸日志增加
     `连通域 N 体素（已按连通域收窄 / **回退固定窗口**）`；刚体化日志的"表面材质 cell 数"标注为**诊断**。
  7. **`assets/config/collapse.toml` + `world/dig/collapse_table.hpp`**：邻域注释同步为"scope = 连通域"
     （**数值与 `schema_version` 一律未动**）。
- 为什么：
  1. **scope 由结构拓扑决定**（Teardown / UE Chaos / Space Engineers 的一致做法）：固定窗口把"影响范围"交给
     一个人为常数，于是"承重被破坏 ⇒ 上部不得悬空"这条契约在**超过窗口的跨度**上直接失效。
  2. **洪泛排在悬挑传播之前**：这是本实现里最容易写反的一处 —— 先传播的话，被拱效应救回的体素会被洪泛排除，
     连通域被切碎，长条反而又漏。
  3. **只扩触界的那几侧**：对称扩张会让一条 30 格的长条付出 120k 采样的立方体窗口；分侧扩张后窗口是细长的，
     成本与结构形状成正比（实测：长条 1 次分析就够，游戏内每次爆炸的邻域 71k~164k 采样）。
  4. **累计上界**：每次扩张都要把整个窗口重扫一遍，只限"单个窗口不超"的话总成本可达上限的两倍以上
     ⇒ 按**累计**算，这条路径的单帧成本才有硬上界（"不冻结画面"）。
  5. **破坏在单一表示内完成**：等值面是补丁采样的**纯函数** ⇒ "未被挖到的区域顶点逐位不变"是**可证伪**的
     —— 这条把"岩石始终不变"从"靠自觉"变成判据（也是本项目第一次把"表示守恒"写成不变量）。
  6. **按材质拆子块**而不是逐体素决定行为：物理上"一个刚体必须整块一致"，所以只能在**分组**这一步拆
     （子块仍是原连通分量内材质一致的部分 ⇒ 与 ADR 0015 的"整体"定义不冲突）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **294/294 passed**（290 → **+2 个 T49 用例 + 2 个 T50 用例**，
     另有 **2 个用例因 T46 口径升级而改写**：`MixedUnitTakesMajorityAndTieBreaksToLowerSlot` →
     `EachSubBlockTakesItsOwnMaterialParameters`、`RigidityFollowsTheShownSurface` → `RigidityFollowsEachSubBlockMaterial`）：
     - **T49①** `LongBarLosesBothPillarsAndFallsEntirely`：30 格长条 + 两端立柱，两次爆破各炸一侧**根部**后
       区域内 `CountFloatingSolid == 0`（**整段落下**；前提"两端都在时稳定"由 `maxCantileverBlocks = 15` 保证）；
     - **T49②** 同用例内双场景**逐位比对**（域体素数 / 各整体首末体素 / 质心）；
     - **T49③** `OversizedDomainFallsBackToFixedWindow`：连通域触界 ⇒ `domainNarrowed == false`（+ WARN），
       回退窗口仍 ≤ 8M 且仍抽出失去支撑的部分；
     - **T50①** `CarveKeepsUntouchedVerticesBitIdentical`：雕刻前后顶点集合的差集（**两个方向**）都落在球附近
       ⇒ **未触及区域顶点逐位不变**；
     - **T50②③④** `RefreshKeepsCentroidAndShrinksVoxelsToTheCarvedPatch`（质心**逐位不变**、质量随剩余体素减小、
       被移除的体素只落在球附近）+ `RigidityFollowsEachSubBlockMaterial`（岩 / 土各半 ⇒ **两个子块**：
       岩刚性保形、土散体回写；土 / 草两子块都散体）+ `EachSubBlockTakesItsOwnMaterialParameters`（双场景逐位比对）。
  3. **门禁**：`check-banned-identifiers.ps1` → `scanned 117 file(s), 0 violation(s)`、`PASS`。
  4. **性能（判据④"最贵一步在哪个线程 / 单帧最坏多少 ms"）**：最贵一步 = **连通域洪泛 + 支撑求解**，
     跑在**逻辑线程（主线程）的固定步内**（与 T30/T33 的"挖除 + 塌落"同一相位；重网格 / 上传 / 碰撞体重建
     仍走 T37 的分帧队列）。debug 实测：典型（域被初始窗口包住 ⇒ **1 次分析**，游戏内每次爆炸都是这一档）；
     30 格长条（含扩张）**14.3 / 8.6 ms**；超界路径（累计扫过上界内的多个窗口）**≈126 ms**
     —— 该路径要求连通域在**两个方向**都延伸 300+ 格，当前测试地图不存在（已按 ADR 0018 的切换条件登记）。
  5. **游戏内实测（`build/vx_t47_smoke.ps1`，1280×720 Debug，26 发连射；本次输入注入成功）**：
     - 每次爆炸都打 `邻域 N 采样 / 连通域 M 体素（已按连通域收窄，T49）`（N = 71k~164k、M = 1808~6753），
       `倒塌` 段 17.4~43.2 ms；
     - `光球命中整体 ⇒ **就地雕刻**（T50）` 共 **26 次**（打掉 0~54 个体素、剩余 35~1437 个；`打掉 0` 是
       "球只擦到凸包、没碰到体素" ⇒ 正确地按无操作返回）；
     - **子块分流实测**：岩子块 `落地口径 = **保留几何体**（不回写）`（诊断 表面材质 cell 数 岩 2922）、
       土子块 `回写并与地面融合`（诊断 土 339~972）⇒ "一条长条里岩石与泥土同时存在"两侧都对；
     - **全程无 ERROR、无 WARN**（未触发超界回退、无帧尖峰、无"整体外观网格缺面"）。
- 下一步 / 遗留：
  1. **未在游戏内触发**：① T49 的"超界 ⇒ 回退固定窗口"WARN（本图结构够小，只在单测里走到）；
     ② T50 的"雕刻后剩余过少 ⇒ 删除整体"兜底（游戏内最小剩余 35 > 阈值 24）。
  2. **T49 的超界路径成本**（≈126 ms debug）已如实登记；切换条件 = **持久结构图**（ADR 0018「本轮不做 / 切换条件」）。
  3. **凸包填平**（ADR 0015 后果 1）在 T50 之后仍然存在：被雕刻出的凹腔在物理上是凸包、在渲染上是等值面
     （ADR 0018 后果 2 已登记，"精确凹形分解"仍是未做项）。
  4. 腾位路径 `RetireOldestRetained` 仍会**体素化回写**：它不是破坏事件（是"把槽位让给新倒塌"），
     故 ADR 0018 决策二没有覆盖它（如实记录，未改）。
  5. 本批改动（T47 + T31 + T48 + T45 + T49 + T50）**尚未提交**。

---

## 2026-09-28  T51 + T52：**降不到支撑的散体一律清除**（修"悬空泥土"）+ **岩石改为完全不可破坏**

- 背景（两条都由项目所有者当天提出）：
  1. **T51 缺陷报告**："泥土落到地上以后由于会发生变形导致有些悬空的泥土元素存在"（并指定"悬空的小土块
     可以直接删除或者降落到地上"）。**真伪判定 = 确认是缺陷**：
     - **契约**：[ADR 0017](../docs/adr/0017-landing-by-material-rigid-vs-granular.md) 决策三"散体落定后**接地沉降**、
       **不允许'下方为空'的悬空体素**"，以及 `kSettleMaxDropBlocks` 注释里明写的承诺
     > "真正把它走满会**计入 `stuckVoxels` 并告警**（不静默留悬空体素）"。
     - **机制**：① 落地沉降有 **32 格上限**，走满即停下、体素**仍留在空中**（只计数 + WARN）；
     ② **`below < settleBottom`（已经降到回写区域之外）这条 `break` 分支既不计数也不告警**
     ⇒ 落点下方 32+ 格全空时**静默**留下悬空体素。这与注释承诺的"不静默"直接冲突。
  2. **T52 需求**："岩石材质修改为完全无法击毁无法挖洞，具备物理引擎能够被光球打到"。
- 做了什么：
  1. **T51（`world/dig/volume_collapse.cpp`）**：接地沉降之后**追加一次自下而上（y 升序）的"仍悬空 ⇒ 清除"扫描**：
     凡"下方为空 / 下方已在回写区域之外"的**已落位**体素**直接从回写结果里去掉**。
     为什么自下而上：删掉一个体素会让**它上方**那个变悬空 —— 自下而上保证"处理到某个体素时，
     它下方已经是最终状态"，同列连锁悬空被一并清除、**一次扫描即收敛**。
     `below < settleBottom` **也算悬空**（32 格内没有任何采样可查，不该按"接地"处理 ——
     注意这与"支撑检查把邻域底面视作地面"的取舍**不同**：那里的底面之外**已被证明**是整块实心）。
     `CollapseWriteback::stuckVoxels` **改名并改义**为 **`removedFloatingVoxels`**（"回写后仍悬空、**已清除**"的体素数），
     `writtenVoxels` 相应扣减（否则守恒记账虚高）；`game/main.cpp` 的日志与 WARN 文案同步。
  2. **T52（`assets/config/materials.toml` + `world/terrain/material_table.cpp` + `game/main.cpp`）**：
     rock 层置 **`indestructible = true`**（`Default()` 同步；**既有字段 ⇒ 不升 `schema_version`**）；
     `toughness = 5.0` 保留但**不再参与结算**（注释写明"改回 false 即恢复可挖的硬岩"）；
     `Detonate` 的**区域外**分支在 `ApplyTerrainCrater` 之前先查"坑覆盖到的每一列"的可挖材质，
     **整片都是不可破坏材质 ⇒ 直接返回、不动地形**。
     为什么不做**逐列**过滤：区域外只有高度场，坑是"高度场剖面"，逐列过滤会在坑面上留下"柱子"、
     破坏 ADR 0013 §二.5 的**边界一阶连续**；整片判定要么整坑不挖、要么整坑照旧，几何上始终自洽。
     为什么"完全不可破坏"是**配置**而不是新代码：`indestructible` 这个开关从 T31 起就在
     （`CarveByDamage` 跳过它**且不消耗预算**），T50 的碎块补丁雕刻也跳过它 ⇒ 一处开关同时覆盖
     "地形挖洞"和"打碎岩石残骸"两条破坏路径；而**密度 / 碰撞体 / 命中判定**完全不经过这个开关
     ⇒ **光球仍被岩石挡住、岩石仍会失去支撑并作为刚体倒塌**（倒塌不是"破坏"）。
- 为什么：
  1. **"不允许悬空"是契约，不是观感**：悬空的泥土既不可站、也不可挖，还破坏"地形总是可站立"的整体承诺；
     所有者给的处置（删除 / 降落到地面）里，**删除**是唯一代价可控的（降落到地面要一路扩到世界地面）。
  2. **补齐"不静默"**：旧实现在"降到区域之外"这条路径上连 WARN 都没有 ⇒ 现象无法被日志解释；
     现在**要么降到支撑面、要么被清除**，两者都有计数，且清除时打 WARN。
  3. **岩不可破坏用显式开关而不是删掉代码**：破坏路径、耐久口径、命中与碰撞全部保持原样，
     以后想恢复"可挖的硬岩"只需改**一行配置**（可回滚）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **296/296 passed**（294 → +2）。新增 / 改写：
     - **T51** `VolumeCollapseLanding.FloatingGranularVoxelsAreRemovedInsteadOfBeingLeftInTheAir`：
       深井场景（地板只到 y < 70）+ 高空石板（y = 109/110）⇒ 落点下方 32 格内没有支撑
       ⇒ `removedFloatingVoxels == kSlabVoxels`（**全部清除**）、`writtenVoxels == 0`、`droppedVoxels == 0`、
       回写后 `CountFloatingSolid == 0`；
       对照 `GranularWritebackSinksVoxelsOntoSupport` 断言 `removedFloatingVoxels == 0`（**不误删**）。
     - **T52** `DigVolume.CarveByDamageCannotTouchRockAnymore`（岩 = 零改动 / 不标脏 / 仍实心；
       **对照**：同场景同预算换成土 ⇒ 正常挖出 r ≈ 6）、
       `DigVolume.CarveByDamageSkipsIndestructibleMaterial`（把**土**置 `indestructible` ⇒ 零改动；对照原表 ⇒ 被挖开）、
       `VolumeCollapseCarve.IndestructibleRockDebrisIsNotCarvedAtAll`（球心就在岩里 ⇒ 补丁**零改动**、
       完整网格顶点**逐位相同**、体素清单不变）。
     - **连带修订（如实登记）**：T50 的两个雕刻用例原本用**岩**（岩不可破坏后必然失败）⇒ 改用**土**验证
       雕刻数学，并把"岩不可雕刻"单独立成上面那条用例 ⇒ 语义变成"**岩石被打中只被挡住，掉落中的土块仍可被就地雕刻**"。
  3. **门禁**：`check-banned-identifiers.ps1` → 0 违规、`PASS`。
  4. **游戏内实测（`build/vx_t47_smoke.ps1`，1280×720 Debug，连发；输入注入成功）**：
     - 5 次爆炸全部照旧打 `爆炸（当帧 = 挖除 + 抽出 + 刚体化）… 连通域 N 体素（已按连通域收窄，T49）`
       （N = 0 / 10 / 2862 / 3469 / 0），`挖除` 4.67~5.83 ms、`倒塌` 4.32~10.48 ms ⇒ **T49 / T31 路径无回归**；
     - **`就地雕刻` 0 次、`倒塌落定` 0 次**、**无 `悬空体素 ⇒ 已清除` 的 WARN**、**全程无 ERROR / WARN**
       ⇒ **没有出现新的坏现象**；
     - **但本轮冒烟没有取得"岩石打不动"的直接证据**（如实登记）：对岩层的爆炸若**零改动**，
       `Detonate` 会在挖除前就返回 ⇒ **根本不打这条日志**；因此"日志里没有对岩的爆炸"与
       "没打到岩"在日志上**不可区分**。同理 `就地雕刻 0 次` 也可能只是"没命中刚体"。
     - ⇒ **T51 / T52 的判据由单测钉死（296/296）**；游戏内"岩壁挖不出坑 / 岩块不被雕刻 /
       泥土落地后无悬空"三项**待人工目视验收**（见下）。
- 下一步 / 遗留：
  1. **待人工目视**：① 对着**陡坡岩壁**连发 ⇒ **挖不出坑**，且光球**被岩石挡住**（不是穿过去）；
     ② 对着**草 / 土坡**连发 ⇒ **照常挖出坑**；③ 把**岩块的支点炸掉**（若支点本身是土）⇒ 岩石仍会**失去支撑并倒塌**；
     ④ 泥土残骸落地后**看不到悬空土块**（若出现 `散体回写后仍有悬空体素 ⇒ 已清除` 的 WARN，
     说明落点与真实支撑差了 32 格以上 —— 现象已修，但值得回看落点为什么那么高）。
  2. **范围界定**：区域外**地表**只做"整片皆不可破坏 ⇒ 整坑不挖"（无法逐列区分岩 / 土，见上"为什么"）；
     当前测试地图整张地表都在可挖区域内 ⇒ 该分支实际只在世界边缘之外才可能触发。
  3. **岩不可破坏的副作用（如实登记）**：山体内部的岩层现在**挖不动**（只有草 / 土 / 沙可挖），
     "从山侧射入挖洞"的可挖层变薄；**回滚方法** = `assets/config/materials.toml` 里 rock 的
     `indestructible` 改回 `false`（`toughness = 5.0` 仍是可挖时的坚固度）。
  4. 本批改动（T47 + T31 + T48 + T45 + T49 + T50 + T51 + T52）**尚未提交**。

---

## 2026-09-28  T53 + T54 落地：**岩石遮挡爆炸波**（修"岩后的东西被炸"）+ **贴陡坡无限跳**（修 T54）；T55 **已复现并定位**

- 背景（项目所有者 2026-09-28 一次给出三条）：
  ① "**岩石要遮挡爆炸波，保护岩石后的东西不被破坏**"（指定项）；② "当前跳跃有 bug，**贴着垂直岩壁能够一直跳**，
  要求按照业内规范优化跳跃逻辑"（**判定：确认是缺陷**，契约 + 机制见下）；③ "爆炸破坏的地方**有概率出现某个面
  透明没有染色**"（T55 / BUG3）。
- 做了什么：
  1. **T53 `world/dig/dig_volume.*`**：`CarveByDamage` 在预算结算**之前**新增一次**以爆心为起点的可达性洪泛**
     （`BlastMask`：6 邻域、**不可破坏材质 = 遮挡体**、掩码球 = `radius + kCarveBandBlocks`），
     只有**可达**的格才参与预算结算；`RasterizeBall` 新增 `BlastMask` 入参 ⇒ **不可达采样保持原状**。
     同一趟把每格的**材质槽位 / 实心性**缓存下来（洪泛与预算结算共用一次采样 ⇒ 消掉了改前预算循环里的第二轮查询）。
  2. **T54 `engine/physics/physics_world.*` + `game/character_movement.hpp` + `game/main.cpp`**：
     `CharacterState` 增 `walkableGround`（= Jolt `EGroundState::OnGround`，**不含** `OnSteepGround`）与 `groundNormal`；
     起跳门槛由 `onGround` 改为 **`walkableGround`**；新增 `vx::JumpAssist` / `vx::AdvanceJumpAssist`
     （**土狼时间 0.10 s + 跳跃缓冲 0.15 s**，只在固定步推进、起跳即清零、飞行 / 瞬移时 `Reset`）。
  3. **T55 取证（本轮只取证，未改外观网格代码）**：新增 `vx::CountDegenerateTriangles`（诊断）+
     两个探针测试；**关键证据来自游戏内冒烟**（见「验证」第 4 条）。
- 为什么：
  1. **T53 只改预算不改栅格化是不够的**：④ 用"最后一个被破坏格的半径"栅格化一个**光滑球面**，该球面不看材质
     （只跳过不可破坏采样）⇒ 岩后的土照样被球面挖掉。所以掩码必须一路传到 `RasterizeBall`。
  2. **T53 用洪泛而不是"方向投影阴影"**：爆炸波会**绕过**岩体边缘（衍射），只有**真正被围住**的格才受保护；
     投影式近似会把"绕过去的那部分"也一并挡掉。6 邻域（不对角）则是为了不让波从两块岩的**角缝**漏过去。
  3. **T54 的机制**：`onGround = (OnGround || OnSteepGround)` 把"站在过陡坡上"也算作着地，玩法层据此放行起跳
     ⇒ 贴着陡壁下滑时**每一步**都满足起跳条件。业内口径一致（Unity `isGrounded` + `slopeLimit`、
     Unreal `Walking` / `WalkableFloorZ`、Jolt 官方示例 `GetGroundState() == OnGround`）⇒ 改门槛 = 修缺陷。
     土狼时间与跳跃缓冲是**业界默认项**（"按了没跳 / 落地没跳"是可感知残缺），按 SKILL「需求受理」第二步**主动补上**。
  4. **T53 的性能口径（SKILL 要求）**：最贵一步 = **掩码填充**（主线程、逐格 2 次采样、O(r³)）。
     Debug 单块场景实测：**r=6 ≈ 1.9 ms / r=8 ≈ 3.9 ms / r=10 ≈ 7.0 ms**（改前同场景"半径驱动栅格化"基线 r=10 = 0.31 ms）；
     出货配置 `explosion_radius = 6` ⇒ **单帧 ≈ 2 ms，在 3 ms 规则内**；**切换条件** = 半径提到 > 8 时，
     掩码填充必须按帧切分或下沉 worker（已登记到阶段计划 T53 行）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **306/306 passed**（296 → +10）。新增：
     - **T53**（`tests/dig_volume_test.cpp`，4 项）：`CarveByDamageIsBlockedByRockWall`（岩墙后的土**零改动**；
       **对照**：同一场景把岩墙换成土 ⇒ 墙后照旧被挖通）、`CarveByDamageOccludedCellsDoNotConsumeBudget`
       （墙前近侧腔体与"没有墙"时**一样大**）、`CarveByDamageEpicenterInsideRockLeaksNothing`（爆心在岩里 ⇒ 波不外泄）、
       `CarveByDamageInsideRockShellCarvesOnlyTheInterior`（**立方岩壳**完全围住 ⇒ 只挖壳内、壳外零改动，
       且掩码切断处**退化 = 0、绕序反转 = 0**）。
     - **T54**（`tests/character_jump_test.cpp`，4 项）：`SteepSlopeIsSupportedButNotWalkable`（71.6° 陡坡上
       `onGround == true` **且** `walkableGround == false`）、`SteepSlopeCannotBeClimbedByRepeatedJumps`
       （**缺陷判据**：4 秒内每步都请求起跳 ⇒ `不可站立时起跳次数 == 0`、爬升 < 0.5 格）、
       `AirborneJumpRequestAfterCoyoteExpiresIsIgnored`、`CoyoteTimeAndJumpBufferWindows`（5 条边界）。
       既有 `character_jump_test` 的 3 项（B3 锁存 / 派生高度 / 物理可达）全部改为**与玩法层共用同一实现**。
     - **零回归证据**：`DigVolumeProbe` 的 8 组真实挖除路径配置，**三角数改前改后完全一致**
       （3356 / 3392 / 3992 / 4876 / 4712 / 3836 / 2528 / 4624）⇒ 无遮挡时 T53 逐位等价。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 117 file(s), 0 violation(s)`、`PASS`。
  4. **游戏内冒烟（`build/vx_t47_smoke.ps1`，1280×720 Debug，连发 14 轮）**：
     - **无 ERROR**；`爆炸（当帧 = 挖除 + 抽出 + 刚体化）` 全程正常（`挖除 4.84~5.67 ms`、
       `连通域 9315~12194 体素（已按连通域收窄，T49）`），`就地雕刻` 11 次、`倒塌落定` 9 次 ⇒ T31 / T49 / T50 **无回归**；
     - **T53 的成本在游戏内得到确认**：`explosion_radius = 6` ⇒ `挖除 4.8~5.7 ms`（与改前同量级 —— 改前只有球内候选格采样，
       现在多采了过渡带那 1.5 格一圈，同时省掉了预算循环的第二轮查询）；
     - **⚠ T55 找到直接证据（本轮最重要的发现）**：stderr 共 **5 条**
       `整体外观网格缺面（T47 自检）：体素 N 个、patch a×b×c；**截断前**边界边 1 条、**实际上传**网格边界边 1 条、
       容量截断 = **否** ⇒ 成因 = **网格生成本身**（候选②敞口场景 / 候选③地形↔体积交界缝）`
       （N / patch = 99 / 7×10×7、173 / 8×15×6、173 / 8×15×6、**85 / 6×27×5（边界边 20 条）**、838 / 8×49×8）
       ⇒ **"某个面透明"确实存在且可复现**，且**容量截断（候选①）被排除**，指向 **`BuildCollapseUnitMesh` 的
       等值面生成本身（候选②）** —— 即 Naive Surface Nets 在**歧义 cell**（对角实心等）处的**非流形捏合 / 边缘计数异常**。
- 下一步 / 遗留：
  1. **T55 的下一步（须先定口径再改代码）**：把上述 5 个形状之一（优先 `6×27×5 / 边界边 20 条`）在单测里**复现成
     `CountBoundaryEdges > 0` 的固件**，再决定修法 —— 候选：① 对**歧义 cell** 拆顶点（按实体侧连通分组，每次面片一个顶点，
     即 dual contouring 的常见做法）；② 只丢弃**零面积三角形**（合并自交四边形 ⇒ 修"边缘被 4 个三角形共用"的计数异常）。
     两条都要过"未触及区域顶点逐位不变"（T42 / T50 的表示守恒）与"与地形同源"两条既有判据。
  2. **待人工目视**：① 对着**陡坡岩壁**连发 ⇒ 挖不出坑、光球被挡住；② 贴着**陡坡**反复按跳 ⇒ **爬不上去**（T54）；
     ③ 站在**洞口边缘**跳 ⇒ **跳得起来**（土狼时间生效，不出现"边缘跳不起来"）；④ 爆炸后**抬头看碎块**：
     是否仍能看到"透明的面"（T55 未修，仍在）。
  3. 本批改动（T47 + T31 + T48 + T45 + T49 + T50 + T51 + T52 + **T53 + T54**）**尚未提交**。

---

## 2026-09-29  T55 落地：**等值面网格的歧义处置**（修"爆炸处某个面透明"）—— 按实体侧连通分量拆顶点

- 背景：项目所有者 2026-09-28 实测"爆炸破坏处**偶发某个面透明 / 未染色**"（T55 / 原 BUG3）。上一轮
  已排除"退化四边形 / 地形与洞的网格"两个方向，并把嫌疑收敛到**倒塌整体的外观网格**（T47 自检报出
  `边界边 1 条 / 20 条`，而地形路径一直判 0）。本轮按 DODS 顺序先**复现成固件**再定修法。
- 做了什么：
  1. **复现固件（`tests/volume_mesher_test.cpp`）**：新增 `CheckerboardFaceSampler` —— **两个对角相邻的
     实心采样点**（网格面 `x=3` 上 `(3,3,3)` 与 `(3,4,4)`）⇒ 该面四角符号成**棋盘格**，即**歧义面**。
     位置刻意离区域边界 ≥3 格，避免"区域边界棱"造成**假阳性**（第一版固件把实心点放在 `y=0/k=0`，
     剩下的 6 条边界边其实来自区域边界，不是歧义 —— 已按此修正）。
  2. **`world/dig/volume_mesher.*`（T55 修法，[ADR 0019](../adr/0019-ambiguous-cell-vertex-splitting.md)）**：
     - 顶点阶段改为**实体侧连通分量**：实体角之间以"**共棱**"（角下标 Hamming 距离 = 1）连通，
       **分量数 = 该 cell 的顶点数**；位置 = **该分量所辖棱**的交点均值；材质 = 该分量实体角的众数。
     - 新增 `cellEdgeVertex`（cell × 12 条局部棱 → 子顶点序号）与 `kQuadCellLocalEdge[3][4]`
       （一条网格棱在 A / B / C / D 四个 cell 里各自的局部棱编号）。
     - 四边形发射时，4 个 cell 各取"**与本网格棱同一分量**"的子顶点。
     - **单分量 cell 的偏移恒为 0** ⇒ 与引入本机制前**逐位相同**（既有判据零回归）。
  3. **诊断转回归**：`VolumeMesherProbe.LatticeAlignedQuantaReport`（12 组整数密度球面）补上三条
     **断言**（退化 / 边界边 / 绕序反转均 = 0），不再只是打印；新增
     `VolumeMesher.AmbiguousFaceMeshesAsTwoManifoldSheets` 作为歧义面的回归固件。
- 为什么（**证据链**）：
  1. **机制**：**歧义 cell**（实体角分成 ≥2 个不连通分量，例：对角实心）在"每 cell 只放 1 个顶点"下会把
     两片本应分开的表面**捏合**成一点 / 一条线；**歧义面**（网格面四角棋盘格）的 4 条棱**全部**跨面 ⇒
     该面两侧 cell 顶点之间的那条网格边被 **4 个三角形**共用（非流形）⇒ 面缺失 / 自交 / 绕序翻转 ⇒
     背面剔除（`cull_mode = CULLMODE_BACK`、片元恒写 `alpha = 1`）后屏幕上就是"**那个面透明**"。
  2. **前后对照（同一条固件，仅换 mesher 实现）**：修前 `顶点 14 / 三角 24 / 退化 0 / **边界边 1** / 绕序反转 0`；
     修后 `顶点 16 / 三角 24 / 退化 0 / **边界边 0** / 绕序反转 0`。修前的 `边界边 1`
     **与游戏内 WARN「边界边 1 条」同签名** ⇒ 根因锁定。
  3. **为什么不按 ADR 0007 的原定切换（Marching Cubes）**：MC 是表驱动、**顶点位置在 case 之间跳变**，
     与"平滑手感"及"**与地形同源 / 破坏时表示守恒**"两条既有判据冲突；本次改为**留在 SN 家族**的
     歧义处置即可闭环 ⇒ 记 **ADR 0019**（补充 ADR 0007，未取代；其"可切 MC"仍作为其它情形的备选保留）。
  4. **"只丢弃零面积三角形"（上一轮的候选②）被否**：本次固件的 `退化 = 0` 而 `边界边 = 1` ⇒ 非流形来自
     "**4 个三角形共用一条边**"，与面积是否为零无关；该做法既修不了非流形，也不修自交 / 捏合。
- 验证：
  1. **构建**：`cmake --build --preset debug` → 退出码 0、**零错误零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **307/307 passed**（306 → +1：新增歧义面固件）——
     **零回归**：`VolumeMesher.RegionEntryIsBitIdenticalToBlockEntryAtBlockSize`（区域入口与整块**逐位相同**）、
     `VolumeMesher.SubRegionEqualsWholeBlockInWorldSpace`、`VolumeCollapseLanding` 系列的碎块网格闭合、
     球面面积 / 法线 / 材质全部通过。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 117 file(s), 0 violation(s)`、`PASS`。
  4. **前后对照证据**：临时 `git stash` 掉 mesher 改动后重跑同一条固件 ⇒ **失败**（`边界边 1`）；
     恢复后 ⇒ **通过**（`边界边 0`）。命令与数字见上。
  5. **游戏内冒烟（`build/vx_t47_smoke.ps1`，1280×720 Debug，14 轮连发）：无 ERROR、`stderr` 仅 8 条「帧尖峰」WARN（34~38 ms，其中"未计时"12~22 ms）** ⇒
     **`整体外观网格缺面（T47 自检）` 未再出现**。**但本轮必须如实登记：整场未产生任何倒塌整体**
     （每次爆炸都是 `失去支撑 0 体素、整体 0 个`）⇒ `BuildCollapseUnitMesh`（自检所在处）**根本没被调用**
     ⇒ **这条"没出现 WARN"不能当作 T55 的游戏内证据**；T55 的游戏内确认仍需**人工目视**（或让自动化瞄准走到地标塔基）。
  6. **游戏内视觉确认：待人工目视**（本环境无目视能力）——见下"测试要点"。
- 下一步 / 遗留：
  1. **V0.1 收口批次继续**（阶段计划 T56）：② I1（Linux CI，**本机不可验证**）→ ③ P6（占位纹理核对，
     疑已随 ADR 0009 / 0010 改为程序生成而作废）→ ④ T10（阶段验收 + §4 回填）。
  2. **待人工目视**：把地标塔基座轰断、抬头看掉落中的碎块与落地残骸 —— 期望**从任何角度都不出现"透明面"**，
     且 stderr **不再出现** `整体外观网格缺面（T47 自检）…`。
  3. **本批改动（T55 + ADR 0019 + 文档）尚未提交**。

---

## 2026-09-29  产品方向确定：**引擎优先 + 游戏目标（修仙 · PvE+PvP · 战斗 · 养成 · 部分可破坏 · AAA）**；把「防走偏标尺」写进 SKILL

- 背景：项目所有者提问"**全部物体可破坏是不是成本高、性价比低？我是否越走越偏了**"。给出现状分析后，
  所有者随即明确了产品方向（原话要点）：**做引擎**、**引擎层与游戏层分开**、**先为当前这款游戏做引擎**；
  游戏要 **修仙题材**、**能 PvE 与 PvP**、**在随机生成或固定生成的地图里战斗**、**有充足的养成系统**、
  **战斗时为了更真实、部分物体可破坏**、**品质对标 AAA、流畅可玩**；并要求"分析这些方向，**合理则写入 SKILL
  以规范后续开发方向、避免走偏**"。
- 分析结论（**判定：方向合理，予以落盘**；附两处必须提问的点）：
  1. **"引擎/游戏分层 + 先服务当前这款游戏"** 与既有架构（`platform → engine → world → game`）和
     第五节"不为未来第二款游戏提前抽象"完全一致 ⇒ **不是新增约束，而是把既有原则提升为"第一道过滤器"**。
  2. **"部分物体可破坏"** 与现状口径**一致**（地形体量可破坏 / **岩石 `indestructible`** /
     固定器物**几何不可变 + 状态切换**，[ADR 0013](../adr/0013-destructible-elements.md)）⇒ 这是**收敛**而非扩张；
     **不存在"要不要做全可破坏"的待办决策**（此前担心的成本大头本就不在设计里）。
  3. **真正"偏"的位置是另两处**：① **破坏/塌落的"长尾"**（凹形分解 / 连锁复核 / 落地摔断 / 持久结构图）——
     边际收益低、且已连开 7 个 ADR（0013~0019）并连出 4 个 BUG；② **世界规模的地基与核心玩法零投入**
     （待收敛项 7 未收敛、流式 / 存档 / 物件层 / 收集 / NPC 均未开始，`world-setting.md` 9 问无人回答）。
  4. **两处必须提问（未替所有者决定）**：① **PvP 与"可破坏地形"叠加**是联机里最贵的部分之一
     （Teardown 不做多人；Space Engineers 为此付出巨大代价）⇒ **PvP 的破坏范围须单独定**；
     ② **"充足的养成系统"属世界设定 / 玩法内容，AI 不得代拟** ⇒ 记入 `game-design.md` §6 待答问题。
- 做了什么（**落盘，未改代码**）：
  1. **SKILL 新增「产品方向与防走偏标尺」**（放在「阅读约定」内、**先于「需求受理」**）：四项定调 +
     **四条硬标尺**（需求必须挂到"世界成立 → 玩法成立 → 表现与深度"这条链上；顺序不得倒置；
     "对标 AAA"必须可判定；阶段位置必须写明）+ **两个高风险点**（PvP×可破坏 / 养成须所有者提供）+
     与其它章节的关系（**优先于"业界标准"与"3A 基线"**）。
  2. **SKILL 第五节**：记明 **PvP 使"网络联机"从"可选暂缓项"升级为"产品最终目标"**（仍排在正式版、当前不启动）；
     其余暂缓项不变。
  3. **SKILL DoD**：新增一项"需求已过产品方向标尺"。
  4. **`docs/game-design.md`**：§1 定位表补 **产物定位 / 玩法形态 / 养成 / 破坏范围 / 品质目标** 五行；
     §2.4 由"明确不在当前阶段"改写为**「目标 ↔ 阶段」对照表**（同时防"提前做"与"永远不做"两种走偏）；
     §6 追加 5 条落地前必答问题（战斗形态 / PvP 破坏范围 / 养成骨架 / AAA 具体标尺 / 世界尺度）。
  5. **`docs/tech-plan-v2.0.md` §8**：加"产品方向 + 执行顺序 + 破坏边界"说明；V1.0 补"承载 PvP"与"养成内容展开"。
  6. **`docs/plans/v0.1.md`**：「下一步」第一顺位**重排**为 **ⓐ 待收敛项 7 → ⓑ V0.2 流式加载 → ⓒ 玩法骨架**，
     破坏长尾**降为后续**（冻结与否须所有者确认）；「阻塞 / 未决」补记产品方向。
- 为什么：方向若不落盘，就会继续被"实测 → 修"的反馈循环牵着走（这次的 T47~T55 就是一次完整例证）。
  落盘位置遵循 SKILL 第八节：**方向进 方案文档 §8 + 阶段计划**，**判定标尺进 SKILL**，**游戏需求进 `game-design.md`**，
  三者互链、不留"两处并存的有效方向"。
- 验证：本轮**只改文档，无可运行产物** ⇒ 未跑构建 / 测试（代码与测试维持 **307/307**、门禁 **117 文件 0 违规**）。
  落盘完整性可逐处核对：`SKILL.md`（新增节 + 第五节 + DoD）、`docs/game-design.md`（§1 / §2.4 / §6）、
  `docs/tech-plan-v2.0.md`（§8 头部 + V1.0）、`docs/plans/v0.1.md`（下一步第 2 条 + 阻塞 / 未决）。
- 下一步 / 遗留：
  1. **待所有者回答**：`game-design.md` §6 的第 7~11 问（战斗形态 / PvP 破坏范围 / 养成骨架 / AAA 具体标尺 / 世界尺度），
     以及 `world-setting.md` §3 的 9 问 —— 这是"先定核心玩法与设定"这一步的实际内容。
  2. **待所有者确认的降级**：破坏长尾（凹形分解 / 连锁 / 摔断 / 持久结构图）**是否冻结**。
  3. 本批（T55 修复 + ADR 0019 + 产品方向落盘 + 文档同步）**尚未提交**。

---

## 2026-09-29  破坏/塌落**长尾冻结**（所有者确认；降级三点已登记）

- 做了什么（**只改文档**）：把"**凹形分解 / 连锁复核 / 落地摔断（T44）/ 持久结构图 / `structureId` 强制合并**"
  一律**冻结为「已知限制（后续可选）」**，并在四处登记：`docs/game-design.md`（「结构塌陷」行，含降级三点）、
  `docs/engine-capabilities.md`（结构塌陷行）、`docs/plans/v0.1.md`（T44 行状态 + 「下一步」第 2 条 +
  「阻塞 / 未决」）、本条目。
- 为什么（**降级三点，按 SKILL「降级必须先问」事后留档**——事前已由所有者在本轮问答中确认）：
  1. **冻结到什么程度** = 上述五项一律不作；**破坏系统保持现状**（已可玩、已自洽：岩不可破坏、器物几何不可变、
     塌落残骸可站可继续挖）。差异是**可判定的**：不会再有"多凸包分解 / 连锁二次判定 / 落地分段 / 持久结构图"的产出。
  2. **为什么达不到（不做）** = 破坏在本项目中是**战斗的表现手段之一、不是游戏核心**（所有者 2026-09-29 的
     产品方向："战斗时**为了更真实、部分物体**可以破坏"）⇒ 继续深挖的边际收益低于把力量投到"世界成立 / 玩法成立"。
     代价（如实登记）：**凹形碎块仍会被凸包填平**（"塔 + 平台板"连成一体时易被卡住、倾角偏小），
     **落地不会摔断**，**超长条中段**在极端场景下仍可能出现一次求解不够精细的表现。
  3. **备注（何时补上 / 切换条件）** = 满足任一即重估：**(a)** 战斗设计**明确依赖**破坏的真实感
     （如"轰塌建筑用于战术"成为核心机制）；**(b)** 该长尾成为**实测**的玩家可见缺陷（有复现步骤）。
     **建造层不在此冻结**（按 `game-design.md` §2.4 排 V1.0）。
- 验证：**只改文档，无代码 / 无可运行产物** ⇒ 未跑构建与测试（代码与测试维持 **307/307**、门禁 **117 文件 0 违规**）。
  登记完整性可逐处核对：`game-design.md`（结构塌陷行）、`engine-capabilities.md`（结构塌陷行）、
  `plans/v0.1.md`（T44 行 / 下一步第 2 条 / 阻塞未决）。
- 下一步 / 遗留：**待所有者回答**"先定核心玩法与设定"三组问题（世界尺度 / 战斗形态 / 养成骨架），
  已写入 `docs/game-design.md` §6 与 `docs/world-setting.md` §3。

---

## 2026-09-29  玩法方向三条：**战斗形态 = 混合**、**养成骨架 = 境界阶梯 + 功法 + 资源**（已答）；**世界尺度仍开放**

- 做了什么（**只改文档**）：按所有者本轮回答落盘两条 ——
  1. **战斗形态 = 混合**（法术 / 飞剑远程 + 近身打击），**MVP 先落地两种** = 一种**远程弹道**（**复用现有光球**）
     + 一种**近战挥击**；其余（伤害类型、最小战斗循环）待补。
  2. **养成骨架 = 境界阶梯 + 功法 + 资源**（**结构**已确认；**境界数量与名称、功法表、属性维度、成长资源清单待补**，
     **AI 不得代拟**）。
  落点：`docs/game-design.md` 的 **§2.3 主角能力清单**（新增"战斗""养成"两行 —— 按第六.7"**先登记后实现**"）
  + **§2.4** 目标↔阶段表（PvE / 养成两行的备注）+ **§6** 第 7 / 9 问标记已答与"仍待补"；
  `docs/world-setting.md` **§0.1** 新增"养成骨架"行（并指出若灵石 / 矿脉要生成 ⇒ 必须落到生成规则，否则视为未生效）；
  `docs/plans/v0.1.md` 的"待所有者回答"改为"已答两条 / 仍开放三条"。
- 为什么：这两条属**玩法方向**，按 SKILL 第六.7 必须先登记再实现；且"战斗"与"养成"此前**在能力清单里不存在**
  （是新增需求，不是既有条目的状态变化）⇒ 必须新占一行，否则后续开工就没有权威依据。
- 验证：**只改文档，无代码 / 无可运行产物** ⇒ 未跑构建与测试（代码与测试维持 **307/307**、门禁 **117 文件 0 违规**）。
- 下一步 / 遗留：**世界尺度仍开放**（所有者选了"其他"但未给出内容）—— 它与 `game-design` §6 第 11 问、
  `world-setting` §3 第 3 问是同一件事，且**决定待收敛项 7 与 V0.2 流式加载的参数**，须尽快定；
  另两条仍开放：**PvP 的破坏范围**、**"对标 AAA"的具体标尺（含目标硬件基线）**。

---

## 2026-09-29  玩法方向补答：**世界尺度 = 首个世界 ≈ 1×1 km**、**PvP 只允许在标记区域内破坏**、**品质基线沿用方案 §1.3**

- 做了什么（**只改文档**）：落盘所有者本轮三条回答 ——
  1. **世界尺度 = 首个世界 ≈ 1×1 km**（按 1 格 ≈ 1 米、1 tile = 64 格 ⇒ 约 **16×16 个 tile**），
     **"小而完整"起步**、后续按区域扩展。落点：`game-design.md` §1 定位表（替换原「大世界 / 待补」）+
     §6 第 11 问标已答；`world-setting.md` §1.1 的「尺度」行。
  2. **PvP 破坏范围 = 只允许在标记区域（现有 `dig_regions.toml`）内破坏，其余静态** ⇒ 联机只需同步"脏块"，
     带宽与回滚成本可控。落点：`game-design.md` §2.4 的 PvP 行 + §6 第 8 问标已答。
  3. **品质基线 = 沿用方案 §1.3**（GTX 1660 级 / 6 GB 显存 / 1920×1080 @ 60 FPS）。落点：`game-design.md`
     §1「品质目标」行 + §6 第 10 问标已答。
- 为什么（**一处必须提问的连带后果，未替所有者决定**）：**1×1 km 与"超大世界引擎"之间有一处张力** ——
  1 km 尺度内 `float32` 精度绰绰有余 ⇒ **待收敛项 7（物理世界坐标精度）可能不再是硬阻塞**，流式加载与
  地表 LOD 也可能不再是"必须马上做"的前置。这会**直接改变"下一步做什么"**（原计划是 ⓐ #7 ADR → ⓑ V0.2 流式）。
  按「需求受理」第一步（与要求矛盾 ⇒ 不动代码、先提问）⇒ 已记入 `game-design.md` §6 **第 12 问**并单独请示。
- 验证：**只改文档，无代码 / 无可运行产物** ⇒ 未跑构建与测试（代码与测试维持 **307/307**、门禁 **117 文件 0 违规**）。
- 下一步 / 遗留：等第 12 问的答复；答复后按 SKILL 第八节把"下一个阶段（V0.2 或按 1 km 收口的玩法阶段）"
  落成新的 `docs/plans/<阶段>.md`，并重排 `tech-plan-v2.0.md` §8 的前置关系。

---

## 2026-09-29  阶段顺序修订：**先按 1×1 km 做玩法骨架；大世界能力（流式 / LOD / 待收敛项 7）延后**

- 背景：`game-design` §6 第 12 问的答复 —— 所有者选择"**先按 1 km 做玩法**"。
- 做了什么（**只改文档；产品方向不变，仅阶段先后调整**）：
  1. **`docs/game-design.md`**：§6 第 12 问标已答（写明**切换条件** = 世界需要变大 / 跨区域时再启动大世界能力）；
     §2.4 新增「**引擎范围（2026-09-29 定）**」——**大世界能力延后**，且**禁止**以"引擎将来要支持大世界"为由
     提前引入长视距 LOD / 超远坐标那套重装备。
  2. **`docs/tech-plan-v2.0.md` §8**：加「**阶段顺序修订**（所有者确认；产品方向不变）」——
     **先做"世界成立（1 km 内的 tile 管理）→ 玩法骨架（战斗 MVP / 收集 / 物件层 / 养成骨架）"**，
     把 **V0.2 流式加载**与 **V0.5 的 LOD / 遮挡剔除延后**；各阶段**内容不变、仅先后调整**。
  3. **`docs/plans/v0.1.md`**：「下一步」第 2 条的"第一顺位"由 ⓐ #7 → ⓑ 流式 → ⓒ 玩法
     改为 **ⓐ 世界成立（扩到 1 km 的 tile 管理）→ ⓑ 玩法骨架 → ⓒ 表现与深度**。
  4. **SKILL「待收敛项」#7**：状态补记"随流式延后而**不阻塞当前工作**；**收敛时限口径不变**"。
- 为什么：
  1. **1×1 km 内 `float32` 精度足够** ⇒ 待收敛项 7（Jolt 单精度 vs `double` 世界坐标）**不再是当前工作的硬阻塞**；
     而它原本是 V0.2 的前置 ⇒ 顺序自然前移的是"玩法"而不是"流式"。
  2. 与 SKILL「产品方向与防走偏标尺」的执行顺序一致：**世界成立 → 玩法成立 → 表现与深度**；
     1 km 的"世界成立"只需要 **tile 管理**，不需要长视距 LOD 与超远坐标那套重装备。
  3. **登记为"阶段顺序修订"而非新版本方案**：产品方向、世界表示、技术选型**均未变**，只有阶段先后与
     范围优先级变化；若所有者认为这属"重大方向变更"，可另行升级为 `tech-plan-v2.1`（本轮按局部修订处理）。
- 验证：**只改文档，无代码 / 无可运行产物** ⇒ 未跑构建与测试（代码与测试维持 **307/307**、门禁 **117 文件 0 违规**）。
- 下一步 / 遗留：
  1. **V0.1 尚未正式闭环**：① **I1（Linux CI）阻塞**（需 CI 日志或 Linux 环境）；② **人工目视验收**（本环境无目视能力）。
  2. 闭环后按 SKILL 新建下一阶段计划 `docs/plans/<新阶段>.md`（内容 = ⓐ 世界成立 → ⓑ 玩法骨架），
     并把 `tech-plan-v2.0.md` §8 的 V0.2 / V0.3 定位与新阶段对齐。
  3. 本批（T55 修复 + ADR 0019 + 产品方向与阶段顺序落盘 + 文档同步）**尚未提交**。

---

## 2026-09-29  T57 落地：**引擎能力「纹理资源加载」**（美术方向 = 写实已定；贴图接入留 ⓒ）

- 背景：项目所有者提问"**能否去公共资源库下载公共美术资源来美化项目，我只做玩法的创新，合理吗 / 可行吗**"，
  并在随后的三问中给出：**美术方向 = 写实**；本次范围勾选 **地形 PBR 贴图替换 + HDRI 光照**；
  **阶段安排 = 「只做"资源加载能力"，贴图等 ⓒ」**。
- 需求受理（**判定：合理；可行但只有一小块现在能做**）：
  1. **合理**，且**早就在计划里**：ADR 0004 第 5 条已写"人物先胶囊占位，后续再接自由建模 / **公共资源库模型**与骨骼动画"；
     ADR 0009 §4 / ADR 0010 P2 已写"占位贴图程序生成，**后续可直接替换为美术 PBR 资源而不改管线**"；
     `file-index.md` 的 `assets/textures/` 一行本就写着"**真实美术资源落地时在此新增**" ⇒ 不是方向变更，是兑现预留。
  2. **可行性分四块，只有一块现在能做**：**地形 PBR 贴图**（管线就绪，缺"从文件加载纹理"这一引擎能力）⇒ **本次做**；
     **角色模型 + 骨骼动画**（缺 Assimp，NOTICE 排 V0.5 + 骨骼动画系统）、**道具 / 树木 / 建筑**（依赖**物件层 = ADR 0004 层 ③**，
     `game-design` §2.4 排 V0.4、未建）、**音效**（无音源）⇒ **三块均不可行，只登记**。
  3. **许可（硬约束）**：**Quixel Megascans 不可用** —— 借 UE 授权获得的免费额度是 **UE / Twinmotion 专用**（UE-Only Content，
     见 Epic 官方 Quixel Bridge 文档），用不上自研引擎；**安全区 = CC0**（Poly Haven / ambientCG 贴图与 HDRI、Kenney / Quaternius / KayKit 模型）。
     **风格**：CC0 写实线只有**贴图与 HDRI、无配套角色** ⇒ 角色 / 物件日后须另找来源（如实登记）。
- 做了什么（**先登记后实现，全部落盘**）：
  1. **`docs/plans/v0.1.md`**：新增 **T57** 计划条目（范围 / 顺序 / 落点 / 验收）—— 开工前先写，符合「任务下发必须先有计划」。
  2. **`docs/engine-capabilities.md`**：`资源管理（纹理 / 模型加载）` 由 **未开始 → 部分实现**（纹理加载已实现、**尚无消费者**，
     故不记"已实现"）；单元测试数 **307 → 314**、门禁文件数 **117 → 120**。
  3. **`docs/game-design.md`**：§2.4 新增两行 ⓒ 需求（**地形材质换真美术资源** / **环境贴图 HDRI 光照**，均 `未开始`，
     并写明前置 = T57 与"HDRI 光照**须另开 ADR**"）；§3「外观模型」补记**美术方向 = 写实**与"角色模型当前不可行"。
  4. **`docs/world-setting.md`** §0.1：新增「**美术方向 = 写实**」（所有者提供；注：与"世界基调"的对应关系仍待确认）。
  5. **`docs/file-index.md`**：新增 `engine/render/texture_loader.hpp` 模块入口；`assets/textures/` 一行补"读取入口已就绪、
     真实资产入库仍待 ⓒ、只入 CC0 且许可须先登记到 `NOTICE.md`"。
  6. **`NOTICE.md`**：`stb_image` 行补"**T57 起已接入**"。
  7. **代码**：新增 `engine/render/texture_loader.{hpp,cpp}` —— `LoadImageRgba8`（LDR → RGBA8）与 `LoadImageHdr`
     （Radiance `.hdr` → 线性 `RGB32F`）；**先读文件头在解码前**做尺寸守卫（`kMaxImageDimension = 4096`，防解压炸弹）；
     失败**一律抛 `std::runtime_error`**、不静默回退（与 ADR 0005 同口径）；**不触碰 GPU**（可从工作线程调用，
     上传仍留渲染线程 —— 见 SKILL「所有重活都必须离开渲染帧」）。`stb_image` **只在 `.cpp` 内出现**（公共头零泄漏）；
     MSVC 侧 `#pragma warning(push, 0)` + CMake `SYSTEM` 包含目录双保险，避免第三方单头库在 `/W4 + /WX` 下炸警告。
     接线：`engine/CMakeLists.txt` 加 `find_package(Stb REQUIRED)`、`SYSTEM PRIVATE ${Stb_INCLUDE_DIR}`、新增源文件；
     `tests/CMakeLists.txt` 新增测试文件。
  8. **测试**：`tests/texture_loader_test.cpp` **7 项** —— TGA 往返像素与通道序（BGR → RGB、A 补 255）/
     同一文件两次载入**逐字节一致**（红线 7）/ 文件缺失抛 / 空文件抛 / 非图像内容抛 /
     **文件头声明 8192² 在解码前被拒**（关键：证明守卫发生在解码之前）/ LDR 文件喂给 HDR 载入器抛。
     夹具**不引入任何二进制资产**（测试自行拼 18 字节头 + BGR 的未压缩 TGA），符合 ADR 0009"仓库不放占位二进制图"。
- 为什么（**3A 基线三问 + 分期登记**）：
  1. **业界 / 3A 参考**：① 引擎侧资源加载的标准形态是"**元数据 + 异步 IO + 后台解码 + 渲染线程上传**"
     （Unreal 的 async loading / Unity Addressables、Godot `ResourceLoader` 的线程化加载）；② 资产来源上，
     "**买 / 取公共扫描库 + 自研玩法**"是成熟做法（Quixel Megascans 被大厂环境美术使用；独立侧有 Kenney / Poly Haven 等 CC0 生态）。
  2. **本项目的可判定判据**：同一文件 ⇒ **逐字节相同**的解码结果；缺失 / 空 / 非图像 / **头声明超上限**四类失败**必须显式抛错**；
     解码**不触碰 GPU**、可在工作线程完成；守卫发生在**解码前**（有专门的单测钉死）。
  3. **分期（属"先做最小版"，已由所有者当轮确认；登记三点）**：① **做到什么程度** = 只交付**加载能力**，
     **不接任何美术资产、不改渲染管线**（`assets/textures/` 仍为空）；② **为什么只做到这里** = 所有者当轮明确选择
     "只做资源加载能力，贴图等 ⓒ"，且贴图替换属 **ⓒ 表现与深度**，插入到 ⓐ 世界成立 / ⓑ 玩法骨架之前会打乱既定顺序；
     ③ **备注（何时补上、切换条件）** = ⓒ 阶段接入地表材质（改为"文件优先 / 程序生成兜底"）时即成为消费者；
     HDRI 若要参与光照，**须先另开 ADR**（超出 ADR 0010 质量线）。
- 验证：
  1. **构建**：`cmake --build --preset debug` → `BUILD_EXIT=0`，**警告行数 0**（`/W4` + `/WX`；编译命令行可见
     `-external:I<stb include> -external:W0`，证明 SYSTEM 目录生效）。
  2. **测试**：`ctest --preset debug` → **314/314 passed**（**307 → 314**，+7；`Total Test time 20.13 sec`）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 120 file(s), 0 violation(s)`，`PASS`，退出码 0。
  4. **冒烟运行**：`voxel_game.exe` 启动到主循环，日志显示世界 / 体积 / 碰撞接管 / 角色 / 光球全部就绪，
     **无 `ERROR`、无 `WARN`**（含帧尖峰）—— 本批未新增运行时路径，用于确认无回归。
- 下一步 / 遗留：
  1. **本批尚未提交**（与 T55 批同属未提交状态）。
  2. **ⓒ 阶段的两项内容需求已登记、未开工**：地形 PBR 贴图替换、HDRI 光照（后者须另开 ADR）。
  3. **资产入库的前置手续**（开工时必须先做）：选 **CC0** 来源 → 逐项核对许可 → 登记 `NOTICE.md`（含许可原文归档）
     → 再放进 `assets/textures/`。
  4. **仍待所有者给**：**具体对标的作品**（美术方向已定 = 写实，但"写实到哪一档"仍无参照对象）。

---

## 2026-09-29  V0.2 开工：**「世界成立（1×1 km）」阶段与 ADR 0020 落盘**（先登记后开工）

- 背景：所有者验证 T57（`100% tests passed out of 314`）后指示"继续开发"。按 `docs/plans/v0.1.md`「下一步」第 2 条，
  第一顺位 = **ⓐ 世界成立（1 km 内的 tile 管理）**。动手前先把**口径外推**算了一遍（现状 3×3 tile = 192 m、
  可挖体积 441 块 ≈ 15.1 MB、启动 ≈ 8.2 s）：

  | 项 | 外推到 16×16 tile（1×1 km） | 判读 |
  | --- | --- | --- |
  | 地形网格缓冲 | ≈ **49 MB**（0.19 MB/tile；1080p 现状 VRAM ≈ 174 / 300 MB） | **可承受** |
  | 可挖体积：整个垂直范围（y 16 块 × 32 × 32） | **16 384 块 ≈ 549 MB** | 超 CPU 预算（≤150 MB）**3.6 倍** |
  | 可挖体积：只留地表附近薄带（48~80 格） | **2 048 ~ 3 072 块 ≈ 69 ~ 103 MB** | **仍超** |

  ⇒ **静态常驻不成立**；且"视距"在 1 km 尺度下近似全量（要盖住世界边缘需数百米视距）
  ⇒ 本阶段**不靠流式 LOD 解题**，靠"**体积只在地表附近、且只在玩家周围常驻**"。
- 所有者三答（**本轮决策，已落盘**）：① 世界规模 = **直接 16×16 tile（1 km 全量）**；
  ② 可挖体积 = **随玩家动态生成与卸载**；③ 补充"**只有地表部分深度可挖**"。
  这三条恰好触发 [ADR 0006](../adr/0006-diggable-region-marking.md)「何时重新审视」第 1 条
  （"需要运行时动态生成可挖区域 ⇒ 须重审本 ADR"）⇒ 必须新开 ADR。
- 做了什么（**全部为文档落盘，本轮未写引擎代码**）：
  1. **新增 [ADR 0020](../adr/0020-dig-volume-vertical-band-and-dynamic-residency.md)**：可挖体积的
     **竖向带宽**（只在地表附近，`[地表 − D_down, 地表 + D_up]`，建议 32 / 16）+ **动态常驻**
     （玩家所在 tile ± K，建议 K = 2 ⇒ 5×5 tile ≈ 320 m，内存 ≈ 7~10 MB）；
     并明确四条连带：**接管与碰撞的判据来源改为"当前常驻集合"**（ADR 0011 / 0012）、
     **窗口移动的重网格必须走既有分帧队列**、**已改动（脏）块不得卸载**（否则洞会消失 —— 「世界内一致性」硬要求）、
     **窗口外沿用既有"区域外地表爆破"路径**（不新增第二套破坏语义）。备选四案（静态全图 / 标记片 / 提高精度 /
     接管按改动驱动）均留档并写明**切换条件**。
  2. **新增 [docs/plans/v0.2.md](../plans/v0.2.md)**：新阶段计划（前置条件 / 任务分解 T58~T63 / 当前进度 / 验收对照），
     **下一步 = 开工 T58**（世界尺寸参数化到 1 km + 启动分帧）。明确不做：LOD / 遮挡剔除 / 存档 / 物件层 / 战斗。
  3. **`docs/adr/README.md`**：加 ADR 0020 行；「当前阶段计划」改为 **v0.2（当前）+ v0.1（仅收口遗留）**。
  4. **`docs/tech-plan-v2.0.md` §8**：加「**阶段对齐**」——V0.2 的两条原条目改写为"地形 tile **全量常驻** + 分帧"与
     "可挖体积**竖向带宽 + 动态常驻**"；enkits / Tracy 顺延；V0.3 / V0.4 内容不变、顺延。
  5. **`docs/game-design.md` §2.4**：新增「世界成立」一行（阶段位置 = V0.2）+ 「引擎范围」补记三答。
  6. **`docs/engine-capabilities.md`**：「流式加载 / 卸载」行由"未开始（当前固定 3×3 tile）"改为
     **"未开始（口径已定 → ADR 0020）"** 并写清目标形态；「可挖标记区域」行补记"标记同时决定**哪里可挖**与**是否常驻**"。
  7. **`docs/file-index.md`**：目录树 + 目录表新增 **`world/streaming/`**（可挖体积的常驻调度；约束 = 不做主线程同步重活、
     脏块不得卸载）。
  8. **`SKILL.md`**：待收敛项 **7** 的触发条件由"流式 / 大世界任务开工前"改为
     **"坐标量级超出 `float32` 精度时（世界需要变大 / 跨区域）"**（**V0.2 不触发**：坐标量级仍 ≤ 1024 格）；
     唯一口径表的「**可挖范围**」行补上 ADR 0020 的两条约束（竖向带宽 + 平面常驻窗口）。
- 为什么：
  1. SKILL 第八节：**任何"选定技术方案"或"确定开发方向"都必须先落盘再动手** —— 这三条只存在于对话里就等于没定。
  2. SKILL 六.4：**阶段切换必须新建计划**（上一阶段计划冻结不动），故新建 `plans/v0.2.md` 而非往 v0.1 里加任务。
  3. 这轮**不写引擎代码**是因为：① 阶段计划与 ADR 必须先于施工（SKILL「任务下发」）；
     ② ADR 0020 里的 `D_down` / `D_up` / `K` 是**建议值**、且 `D_down` 直接决定"能挖多深"这一**可见玩法参数**，
     须先经所有者确认再据此施工（SKILL「降级 / 参数先问」）。
- 验证：**只改文档，无代码 / 无可运行产物** ⇒ 未跑构建与测试（代码与测试维持 **314/314**、门禁 **120 文件 0 违规**）。
  落盘完整性可逐处核对：`docs/adr/0020-*.md`（新）、`docs/plans/v0.2.md`（新）、`docs/adr/README.md`（§一 / §三）、
  `docs/tech-plan-v2.0.md`（§8 头部）、`docs/game-design.md`（§2.4）、`docs/engine-capabilities.md`（§1.3）、
  `docs/file-index.md`（目录树 + 目录表）、`.trae/skills/.../SKILL.md`（唯一口径表 + 待收敛项 7）。
- 下一步 / 遗留：
  1. **待所有者确认**：ADR 0020 的 `D_down = 32` / `D_up = 16` / `K = 2` 三个**建议数值**（详见本轮回复）。
  2. **下一轮开工 T58**：地图预设的 tile 半径 → 16（世界扩到 1 km），tile 生成与网格化**逐帧推进**并接进既有加载画面。
  3. **本批（T57 + V0.2 落盘）尚未提交**。

---

## 2026-09-29  T58 落地：**世界扩到 1×1 km**（289 tile）+ 消除"每帧 O(世界总量)"热点

- 背景：`docs/plans/v0.2.md` T58（V0.2「世界成立」的第一个实现任务）。所有者已确认三答与数值
  （世界 1 km 全量 / 可挖体积随玩家动态常驻 / 只有地表部分深度可挖；`D_down = 32`、`D_up = 16`、`K = 2`）。
- 做了什么：
  1. **世界尺寸**：`assets/maps/test_range.toml` 的 `tile_radius` 由 `[1, 1]` → **`[8, 8]`**。
     **口径修正（本轮发现）**：V0.2 计划建立时写的"16×16 = 256 个 tile"是**近似**——仓库的 tile 半径是
     **`-r..r` 的奇数个数制**，而 `world/generation/map_preset.cpp` 的上限 **`kMaxTileRadius = 8`
     恰好 = 每边 512 列 = 1024 格 = 1 km** ⇒ 精确表达是 `tile_radius = 8`（**17×17 = 289 个 tile**，
     世界列 `[-512, 576)` = **1088 格 ≈ 1.09 km**）。已据此回填 `plans/v0.2.md`、`game-design.md`（§1 / §2.4）、
     `ADR 0020`（背景表）与 `engine-capabilities.md`。
  2. **消除"每帧成本与世界总量成正比"的既有热点**（SKILL 第四节自查项）：
     `TerrainWorld::MaxSurfaceHeightBlocks()` 每帧被调用（CSM 投射体扩展要用），原实现要遍历
     **全部 tile × 全部顶点** —— 9 tile 时约 3.8 万次无感，**289 tile 时约 122 万次**（debug 下可达数十毫秒）。
     改为在 `TerrainTile` 上缓存 `maxSurfaceBlocks`（由 `GenerateTile` / `MeshTile` 刷新，
     `MeshTile` 是"生成后"与"笔刷改动后（经 `RemeshDirtyTiles`）"的唯一汇合点）⇒ 每帧降为 **O(tile 数)**。
     语义上缓存**可能偏大**（某 tile 最高的列被削低后、到重网格前仍报旧值），方向是**安全**的
     （只用来放大阴影投射体盒，偏大不漏阴影）；测试同时覆盖**双向**刷新。
  3. **新增回归测试** 1 项（`TerrainTile.CachesMaxSurfaceHeightAndRefreshesAfterBrushEdit`）：
     ① 每个 tile 的缓存 == 遍历其顶点重算的真值；② 抬到 400 格后总最大值跟上；③ 再削回 0 后**回落**并与重算一致。
  4. 同步文档：`plans/v0.2.md`（T58 行 + 进度 + 下一步 + §4 两行 + 启动耗时登记）、
     `game-design.md`（§1 世界尺度 / §2.4 两处）、`ADR 0020`（背景表）、`engine-capabilities.md`（流式加载行）、
     `map_preset.cpp`（`kMaxTileRadius` 的注释：8 即 1 km，世界要更大须先收敛待收敛项 7 与 LOD）、
     `assets/maps/test_range.toml`（头部坐标口径 + `dig_hill` 注释：可挖区域当前只覆盖中心 3×3 tile）。
- 为什么：
  1. **先扩世界再改体积口径**：T58 只动"世界边界"，`dig_regions.toml` 保持原样（只覆盖中心 3×3 tile）
     ⇒ 世界立刻可走可看，而"体积只在窗口内"由 T59 / T60 接管（ADR 0020）。这样每一步都可独立验收。
  2. **先修热点再扩规模**：把 289 个 tile 铺开后，任何"每帧 O(总量)"的循环都会变成可见的帧时间；
     按 SKILL「观测先于结论 → 不成就重新设计」，先在扩规模的同一次改动里把它降为 O(tile)。
  3. 不新增 `world/streaming/`：T58 不需要调度器（体积集合仍是静态的），该目录留给 T60（避免建空壳）。
- 验证（命令 + 真实结果）：
  1. **构建**：`cmake --build --preset debug` → `BUILD_EXIT=0`，**零警告**（`/W4` + `/WX`）。
  2. **测试**：`ctest --preset debug` → **315/315 passed**（**314 → 315**；`Total Test time 21.83 sec`）。
  3. **门禁**：`check-banned-identifiers.ps1 -RepoRoot .` → `scanned 120 file(s), 0 violation(s)`，`PASS`。
  4. **游戏内实测**（`1280×720` / `MSAA 4×` / debug）：
     - `预设地图已加载：… tile 半径 [8, 8]（289 个 tile）`（3.949 s）；
     - `碰撞接管（ADR 0012）：地表高度场碰撞体 280 个；9/289 个 tile 的可见面已全由体积绘制；可挖体积三角网碰撞体 112 个`；
     - `世界边界（由 tile 半径 [8, 8] 自动推导）：范围 (-512.0, -8.0, -512.0) ~ (576.0, 520.0, 576.0)`（空气墙 4/4 自动跟随）；
     - `首帧视锥剔除：地表 tile 116/289、可挖体积块 108/441 通过；本帧提交网格 225 个`；
     - `角色落地自检：脚底 (0.00, 120.00, -0.00)，着地=是`；
     - `GPU 纹理显存记账：… 合计 250.54 MB`（**2560×1440** 交换链 + MSAA 4×；仍 < 300 MB）；
     - **启动 ≈ 15.3 s**（tile 阶段 ≈ 1.7 s；体积 init + 碰撞体 ≈ 10.4 s），全程有加载进度；
     - 连续运行约 2 分钟：**无 `ERROR`、无 `WARN`、无「帧尖峰」**（阈值 = 2 × 帧预算 = 33 ms）。
- 下一步 / 遗留：
  1. **启动 15.3 s（debug）登记为体验债**（非阻塞）：不冻结画面成立，但离"流畅可玩"有距离；
     优化候选见 `plans/v0.2.md`「阻塞 / 未决」。
  2. **下一轮开工 T59**（竖向带宽）：块集合按"该块覆盖列的地表高度"裁剪到 `[地表 − 32, 地表 + 16]`（纯函数）。
  3. **待人工目视**：1 km 世界的观感（地形边界无裂缝、远景正常、空气墙位置）。
  4. **本批（T57 + V0.2 落盘 + T58）尚未提交**。

---

## 2026-09-29  T59 落地：**可挖竖向带宽（能挖多深）**；顺序调整落盘；★ 修正 T58 的"无 WARN"假结论

- 背景：所有者指示"**完成 ⓐ 的剩下 5 步（T59~T63），然后把 ⓒ 的主角建模与环境贴图美术资源先落地，ⓑ 先搁置**"，
  并给出两条具体反馈：① "**当前土层还是十分厚，完全没有必要，降低可挖的厚度**"；
  ② "将 ABC 都完成以后，**生成一个新的测试地图**（旧的不删，以后在新地图里测试）"。
- 做了什么：
  1. **顺序调整落盘**（ⓐ → ⓒ → ⓑ，**所有者已明确确认**）：`plans/v0.2.md`（头部 + 明确不做 + §3）、
     `tech-plan-v2.0.md` §8、`game-design.md` §2.4 三处登记**代价**：ⓑ 的玩法内容后移；
     ⓒ 的**角色建模**由 §8 的 **V1.0 前移**（Assimp + 骨骼动画随之提前引入）。
  2. **T59 竖向带宽**（[ADR 0020](../adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策一）：
     - `DigRegionTable` 新增**可选**字段 `band_down` / `band_up`（**缺失 = 0 = 不裁剪** ⇒ 旧文件照旧可用、
       `schema_version` **不变**，与 `material_table` 新增可选字段的先例一致）；
     - `assets/config/dig_regions.toml` 置 **`band_down = 16` / `band_up = 8`**；
     - `DigVolumeWorld` 新增 `ColumnBandFloor` / `BelowDiggableBand`：在 `RasterizeBall`（**逐列**带宽地板，
       在 k 层里只算一次，避免 33³ 内层反复查地表高度）与 `CarveByDamage` 的**预算结算**里跳过带宽之下的格
       —— 与"不可破坏材质"同口径：**保持原状、不消耗预算**；
     - 启动日志打印带宽（可观测）。
  3. **口径更正（重要）**：ADR 0020 原来把带宽写成"**只在相交处建立块**"，但 32³ 块对齐会把**可见厚度量化到
     32 格倍数**（`D_down = 32` 实际可挖到 ≈56 格深，**反而更厚**）。改为**体素级（1 格精度）的挖除守卫**，
     "哪些块存在"留给决策二（玩家窗口，T60）。**数值同时按所有者反馈下调 32 → 16**（`D_up` 16 → 8）。
  4. **新增 2 项测试**：带宽之下保持实心（球体跨越地板时下半段不被挖）/ **整体落在带宽之下的爆炸一格都挖不动
     且采样逐值不变**（这一条同时证明"不消耗预算"：`destroyed == 0` ⇒ 不发生栅格化）；出厂表断言 16 / 8。
  5. **★ 修正上一批的假结论**：T58 的 devlog 与阶段计划写着"连续运行约 2 分钟**无 WARN / 无帧尖峰**"——
     **该结论作废**：当时命令用了 `Select-Object -First 80`，**只捕获了前 80 行**，之后的日志从未进入缓冲区。
     已在本文件与 `plans/v0.2.md` 就地标注作废，并记下**取证纪律**：长时间冒烟必须**完整落盘再检索**。
- 为什么：
  1. 带宽做成"体素级守卫"而不是"裁块"，是因为**用户感知的厚度**由格精度决定；按块裁剪会因 32 格对齐而失真。
  2. 数值进**可选字段**（而非升 `schema_version`）：旧文件与旧测试**逐位不变**，改动面最小（SKILL：不做不必要的破坏性变更）。
  3. 带宽走**独立于"块是否存在"**的路径：与 T60 的窗口调度正交，两者互不干扰。
- 验证（命令 + 真实结果）：
  1. **构建**：`cmake --build --preset debug` → `BUILD_EXIT=0`，**零警告**。
  2. **测试**：`ctest --preset debug` → **317/317 passed**（**315 → 317**）。
  3. **冒烟（完整捕获）**：启动日志出现
     `可挖区域表已加载（schema_version=1）：1 个区域，共 441 个体积块（32³，密度数据约 15.50 MB）；**竖向带宽**：地表以下 16 格 / 地表以上 8 格（0 = 不裁剪，T59 / ADR 0020）`。
- 下一步 / 遗留（**T59 的代码已闭环，但冒烟暴露了一个阻塞项**）：
  1. **★ 新发现（阻塞"世界可走"，登记为 T64）**：1 km 世界下**静置无输入**运行会出现
     `[ 48.951] [WARN ] 角色出界（超出边界 16 格余量）→ 已送回出生点`（t = 48.9 s）
     —— 即**角色坠出了世界**；同时伴随 `[30.4] 82.2 ms`、`[34.7] 102.8 ms`、`[35.0] 69.6 ms` **帧尖峰**
     （逻辑 15.8 / 83.0 / 76.2 ms、draw call 620~724），以及 `[30.3] 散体回写后仍有悬空体素 ⇒ 已清除 15 个（T51）`。
     **须先诊断再继续 T60**（T60 建立在"世界可走"之上）。**待验证假设**：T59 只改挖除路径、静置无挖除
     ⇒ 与 T59 大概率无关（但必须用实验排除）。
  2. 诊断完成后按顺序回到 **T60**（动态常驻调度）。
  3. **本批（T57 + V0.2 落盘 + T58 + T59）尚未提交**。

---

## 2026-09-29  更正：**T64 作废** —— 那三条 WARN 是"真人在操作游戏"，不是缺陷

- 背景：上一条记录里我把 T59 冒烟捕获到的 `角色出界 → 送回出生点`、`散体回写后仍有悬空体素 ⇒ 已清除 15 个（T51）`、
  `帧尖峰 82.2 / 102.8 / 69.6 ms` 判为"**静置无输入**下出现的缺陷"并登记为 T64（标"阻塞世界可走"）。
- **更正依据（完整日志）**：改用 `Start-Process -RedirectStandardOutput` 完整落盘后，日志里出现**明确的输入事件**：
  `[ 20.849] 调试面板：隐藏`（F1）、`[ 21.498] 飞行模式：开（无重力…）`（F）、`[ 22.266] 渲染原点重定基到 (-2, 133, -21)`。
  ⇒ 本次冒烟**不是静置**，是有真人（项目所有者）在**同一台机器上操作游戏窗口**：开了飞行模式到处飞、开炮破坏地形。
- 结论（**三条全部是既有行为，不新增缺陷**）：
  1. `角色出界 → 已被送回出生点` = **T18 的设计**（"墙挡不住飞行越界"，故配出界救援），飞行模式一开就会走到；
  2. `散体回写后仍有悬空体素 ⇒ 已清除` = **T51 的设计**（把降不到支撑的散体清除，不允许悬空泥土）；
  3. 82~103 ms 帧尖峰发生在**破坏 / 倒塌当帧**，且 `逻辑 15.8 / 83.0 / 76.2 ms` 全在既有流程里
     = **早已登记**的成本（T45「未计时 36.7 ms 仍未归属」、T46 遗留「判据"无 > 50 ms 单帧"本轮未满足，
     1 条 53.5 ms 全在爆炸当帧的既有逻辑成本上」），debug 构建下本就如此。
- 做了什么（**只改文档**）：`plans/v0.2.md` 的 T64 行标记**作废**并写明依据；「下一步」由"T64 诊断"改回 **T60**；
  「阻塞 / 未决」把 T64 一项改为作废说明，并沉淀**两条取证纪律**：
  ① `Select-Object -First N` 会**截断捕获**（T58 的"无 WARN"因此是假结论 —— 该结论已在上一批就地标注作废）；
  ② **"我没发输入"≠"没有输入"** —— 本机是真人桌面，游戏窗口会被操作；判断"静置"必须在日志里找输入事件。
  长时间冒烟一律 `Start-Process -RedirectStandardOutput` 完整落盘（PS 管道会 4 KB 分块缓冲，看不到完整日志）。
- 为什么记这条：**假缺陷会占用排期、误导诊断方向**（本次差一点就去"修"一个不存在的问题）。
  这与 SKILL「缺陷报告：先判真伪」是同一条纪律：判定必须能同时点出**契约**与**机制**，否则不动代码。
- 验证：仅文档改动 ⇒ 代码与测试维持 **317/317**、构建零警告。
- 下一步 / 遗留：**开工 T60（动态常驻调度）**，见 `docs/plans/v0.2.md`「下一步」。

---

## 2026-09-29  T60 落地：**可挖体积的常驻调度器本体**（窗口 / 脏块保护 / 分帧）

- 做了什么（**只做调度器本体与 `DigVolumeWorld` 的建卸能力，尚未接进游戏**）：
  1. **新增 `world/streaming/dig_volume_residency.{hpp,cpp}`**（[ADR 0020](../adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二 / 五）：
     - `TileOfBlockIndex`：块 → tile 的**精确**映射（块 32 格、tile 64 格 ⇒ 一个块完全落在一个 tile 内；负坐标向下取整）；
     - `DigVolumeWindow` + `WindowForPlayerBlocks`：玩家窗口（tile ± K），含 `ContainsBlock` 与 `TileDistanceFromCenter`；
     - `PlanDigVolumeResidency`（**纯函数**）：算出四张清单 —— **要建** / **要卸** / **脏块留驻** / **超限淘汰**；
       `toCreate` / `toUnload` 升序，且结果**只由集合决定**（常驻集合先排序 ⇒ 不依赖容器迭代顺序，红线 7）；
     - `DigVolumeScheduler`：`Update`（**幂等**：窗口中心没动就不打乱分帧推进中的待办）+ `Step`（**先建后卸**、
       各自按坐标升序、每次至多 `maxActions` 个动作；实际发生的块写入 `changedOut` 供调用方同步碰撞体与网格）。
  2. **`DigVolumeWorld` 的建卸能力**：`CreateBlock`（走**与批量初始化逐字相同**的 `FillBlockDensity` + `MeshBlock`；
     非区域块拒绝）、`UnloadBlock`（**脏块拒绝卸载**）、`IsBlockDirty`（`carved` 或写过体素材质）、
     `ResidentBlocks`（升序），以及 **`BeginInitFromHeightField(子集)`** ⇒ 启动时可**只初始化玩家窗口**
     （`InitTotalSteps()` 随之；无参重载保持"全部区域块"的旧行为，既有测试逐位不变）。
  3. **脏块超上限**（`kMaxKeptDirtyBlocks = 256`，≈ 8.5 MB）⇒ 按 **Chebyshev 距离最远优先**淘汰并 **WARN**
     （"这些块里玩家挖过的洞会消失"），**不静默**。
  4. 接线与构建登记：`world/CMakeLists.txt`、`tests/CMakeLists.txt`；`docs/file-index.md`（新增该模块入口 +
     更新 `world/streaming/` 行）；`docs/engine-capabilities.md`（"流式加载 / 卸载"由**未开始 → 部分实现**，
     明确写"尚未接进游戏"）。
- 为什么这样切：
  1. **T60 与 T61 必须成对但可分开验证**：接管判据若仍按**静态区域**、而体积只留窗口内，窗口外会出现
     "地表四边形被跳过、体积又没画"的**空洞** ⇒ 接线必须同时改接管判据（T61）。
     故本轮先把**可单测证明**的部分做实（窗口 / 集合差 / 脏块保护 / 分帧语义），接线单独一步做。
  2. **脏块保护是本阶段的世界内一致性硬要求**：玩家挖的洞不得因为走远而消失（ADR 0020 决策五），
     故把它做进**纯函数**（可穷举验证），而不是散在游戏循环里。
  3. **先建后卸的顺序**不是随意：先让窗口内该有的块存在，玩家才不会看到"脚下没体积"的瞬间。
- 验证（命令 + 真实结果）：
  1. **构建**：`cmake --build --preset debug` → `BUILD_EXIT=0`，**零警告**。
  2. **测试**：`ctest --preset debug` → **323/323 passed**（**317 → 323**，新增 6 项：块→tile 映射（含负坐标）/
     窗口计算（含负坐标）/ 窗口包含判定 / 纯集合差 / **脏块不卸载** / 超限淘汰**最远**者 /
     调度器端到端（初始只常驻窗口 + 窗口前移后建新卸旧 + **挖过的块仍在常驻**））。
- 下一步 / 遗留：
  1. **T61（接线）**：`game/main.cpp` 的 `volumeCoords` / `volumeHandles` / `volumeBounds` 三个**平行静态数组**
     要改成**按块索引的动态容器**；启动改 `BeginInitFromHeightField(窗口块)`；每帧 `Update` + `Step` 并用
     `changedOut` 同步 `VolumeCollision` 与 GPU 网格；**接管判据改为按常驻集合**；窗口移动引起的 tile 重网格
     走既有分帧队列；F1 面板暴露常驻块数 / 待办 / 脏块留驻。
  2. **本批（T57 + V0.2 落盘 + T58 + T59 + T60）尚未提交**。

---

## 2026-09-29  T61 落地：**把常驻调度器接进游戏**（动态容器 / 每帧分帧 / 接管判据随常驻集合）

- 做了什么（**代码完成，人工走动验收待所有者**）：
  1. **游戏层的数据结构换成"按块坐标索引"**：`VolumeSlot`（GPU 网格 + 世界 AABB）+ `VolumeSlotTable`
     （`std::map<BlockCoord, …>`，**键存在 == 该块常驻**）+ `BlockOfWorldPoint` / `UploadVolumeMeshAt`；
     `WorldEditContext` 由三个**平行静态数组**改成持 `VolumeSlotTable&`，延后队列的 `VolumeRemesh` 分支随之改写。
  2. **启动只常驻"出生点窗口"**：`initialVolumeCoords = 区域表 ∩ WindowForPlayerBlocks(出生点, K=2)` ⇒
     `BeginInitFromHeightField(子集)`、`ResidentQuadFilter`（**T61 的接管判据**，见 ADR 0020 决策三）。
  3. **每帧调度**（`game/main.cpp` 主循环，`destructionProcessor.Process` 之前）：`Update` + `Step(1 个动作)`
     ⇒ 新建块：**先入表**（否则接管判据与体积不同步）→ 立刻上传网格 → 只把"**碰撞体**"排进延后队列
     （网格已在 `CreateBlock` 内生成；新增 `PendingDestruction::MergeVolumeBlockCollisions`，**不重复重网格**）；
     卸载块：释放 GPU 网格 → 出表 → `VolumeCollision::RemoveBlock`；**接管状态翻转的 tile 一律走延后队列重网格**。
  4. **接管判据翻转的双向处理**：`PendingDestruction` 的 `TileCollision` 分支原来只会"建 / 更新"高度场碰撞体，
     现在"该 tile 地表网格已无面"时改为 **`RemoveTile`**（与启动期判据同源）—— 否则走远卸载后残留的隐形高度场
     会把新挖的洞口挡住。
  5. **补 T60 的一个真缺陷**：超上限的脏块原先被塞进"待卸清单"，而 `UnloadBlock` **拒绝卸载脏块** ⇒ 淘汰**从未发生**、
     且每次窗口移动都会重新计划同一批 ⇒ WARN 刷屏且内存上界失效。现新增 `DigVolumeWorld::EvictBlock`
     （**不查脏**，注释写明"会丢改动"）与调度器的 `m_pendingEvict`（**排在最后动手**），并 WARN（不静默降级）。
  6. F1 面板：`可挖体积块` 一行改为 `常驻（已挖；待办、留驻）`（`DebugStats` 加 `volumePendingActions` /
     `volumeKeptDirtyCount`，标签走 `UiText` 缝，中英两表同步）。
- 验证（命令 + 真实结果）：
  1. **构建**：`cmake --build --preset debug` → 零警告；**测试**：`ctest --preset debug` → **324/324 passed**
     （323 → 324：新增 `EvictBlockIsTheOnlyWayToDropADirtyBlock`）；**门禁**：`123 文件 0 违规`。
  2. **运行时实验（临时改 `K=1`，跑完已还原）**：启动日志
     `可挖体积常驻窗口：玩家 tile (0, 0) ± 1 ⇒ 目标 324 块`、`碰撞接管：4/289 个 tile 全由体积绘制`
     （对照 `K=2` 时是 **9/289**）⇒ **接管判据确实随常驻集合走**；启动后由角色真实位置（tile (0,-1)）触发一次
     **108 块卸载**，`常驻 217/216`（当时日志打点早了一帧，已修）→ 收尾日志
     `常驻 324 块（世界内 324、窗口目标 324、留驻脏块 0）`**三数一致**（含**建**的路径，见下）。
  3. **再实验（临时把出生窗口往 -Z 偏一个 tile，跑完已还原）**：启动窗口只装 108 块 ⇒ 运行期补建 108 块，
     收尾 `常驻 324 块（世界内 324、窗口目标 324、留驻脏块 0）`⇒ **建 / 卸两条路径都跑通、计数自洽**。
  4. **性能实测（debug，均为临时实验）**：**卸载几乎免费** —— 108 次卸载期间**零帧尖峰**、逻辑相位 ≈ 0.3 ms/帧；
     **建块是瓶颈** —— 108 次建块耗时 ≈ 6.6 s（1 个动作/帧），期间 **12+ 条帧尖峰（33~190 ms）**，逻辑相位 20~190 ms。
- 遗留 / 待所有者裁定：
  1. **人工走动验收**（T61 判据）：需走到 **tile ±2 以外**才会触发卸载（测试地图的可挖区域只有 7×7 块，
     出生点附近窗口把区域整个包住 ⇒ 不动就不触发）；看 F1 的 `常驻 / 待办 / 留驻` 与日志
     `可挖体积常驻集合已随窗口调整完毕`；同时目视窗口边缘有无裂缝 / 穿模。
  2. **建块爆发期的帧尖峰**（上条 4 的实测）：debug 下跨越一次 tile 边界 ≈ 6.6 s 的偏长帧。成因是**单个动作本身就重**
     （`CreateBlock` = 填密度 + 等值面网格化 ≈ 22 ms，其中含一次**阻塞到 GPU 完成**的上传，另加一个延后队列单位）；
     "分帧"能摊平总量，但摊不掉**单动作**的高度。可选方向（**须所有者决定，不擅自改**）：
     ① 登记为**体验债**（与 T58 的 debug 启动 15.3 s 同口径），先看 release 实测；
     ② 把 `CreateBlock` 拆成"填密度 / 网格化"两步，各占一帧（峰值约减半）；
     ③ 只实体化**与可挖带宽相交**的块（区域表声明 y∈[0,8] 共 9 层，实测只有 ~29% 的块存在等值面）⇒ 建卸量约降 3~4×，
     但属 ADR 0020 的方案调整，需另开 ADR 修订。
  3. **启动窗口用的是预设出生点（tile (0,0)）、运行期用的是物理回报位置（tile (0,-1)，与 -1e-9 有关）**：
     二者不一致会在启动后立刻触发一次窗口调整（本图恰为无操作，故只多算一次计划）。已登记，暂不处理。
  4. **本批（T57 + V0.2 落盘 + T58 + T59 + T60 + T61）尚未提交**。

---

## 2026-09-29  T62 + T63：**边界／出界在新尺寸下自洽** + **阶段验收（自动侧）**

- 做了什么：
  1. **T62 只补证据、不改代码**：`ComputeWorldBounds` / `ComputeBoundaryWalls` / `IsCharacterOutOfBounds` 三个
     **纯函数**本来就由 tile 半径驱动（T58 已参数化），故本轮把**出厂尺寸（`tile_radius = [8, 8]`）**的期望值
     用测试钉住 —— 新增 3 项（`tests/world_bounds_test.cpp`）：
     - `DerivesOneKilometerBoundsAtMaxRadius`：边界盒 `±512 / 576`、每边 **1088 列**（17×17 tile）、y 取 `-8 ~ 520`；
     - `OneKilometerWallsAreFlushAndCoverTheVerticalRange`：4 堵墙**内表面齐平**（走不进墙里、也漏不出去）、
       中心 `±513`、Z 向半长 `546`、y 中心 `256` / 半长 `264`、四角靠"平行墙多铺一个墙厚"封口；
     - `RescuesAtOneKilometerOnlyBeyondTheMargin`：**贴墙内侧不救援**（墙挡人、不替人判定出界）、恰好落在余量上不算、
       越界 / 坠落才送回出生点，且救援一次后下一帧不再触发（T18 口径不变）。
     三项期望值**逐值取自运行期日志**（`世界边界（由 tile 半径 [8, 8] 自动推导）：范围 (-512.0, -8.0, -512.0) ~
     (576.0, 520.0, 576.0)；不可见围墙 4/4 个，厚 2.0 格；出界救援余量 16.0 格`）⇒ "纯函数算的 = 运行时打的"。
  2. **T63 阶段验收（自动侧）**：按 §4 六个维度逐行回填**命令 + 实测值 + 日志原文**，未达成项如实登记（不粉饰）：
     - **世界存在且可走**：289 tile / `(-512,-8,-512) ~ (576,520,576)` / 围墙 4/4 / 角色落地自检通过 ⇒ 自动侧达成；
     - **不冻结画面**：加载全程有进度，启动 **16.5 s（debug）**（贴图 0.3→2.3 / tile 2.3→3.9 / 体积 3.9→13.9 /
       碰撞 + 收尾 →16.5 s）；**静置 43 s（无输入）零 `WARN`、零帧尖峰**（`.err` 为空）；
     - **可挖体积自洽**：接管判据随常驻集合（`K=1` ⇒ `4/289`、`K=2` ⇒ `9/289`）、建 / 卸计数自洽
       （`常驻 378 块（世界内 378、窗口目标 378、留驻脏块 0）`）、脏块保护与淘汰出口均有单测；
     - **卡顿消除：未达成（debug）** —— 建块爆发期 12+ 条 33~190 ms 尖峰（已裁定登记为体验债）；
     - **内存与显存**：VRAM 记账 **250.54 MB ≤ 300**、体积常驻 **441 块 / 15.11 MB**（上界 = 区域表）；
       **CPU 侧缺分项记账**（进程工作集 ≈ 266 MB、私有提交 ≈ 634 MB，debug 且含 D3D12 驱动侧提交）
       ⇒ 无法按 ADR 0008 的 ≤150 MB 逐项核对，**登记为待补测量能力**；
     - **工程门禁：达成**（构建零警告 + `ctest` **327/327** + 门禁 **123 文件 0 违规**）。
- 为什么这样切：**验收必须由证据构成**（SKILL 第四节「观测先于结论」）—— 世界尺寸、边界、救援这些量都能用
  **纯函数 + 运行期日志双向对照**，所以先做"机器能证的"，把"必须人看的"（观感 / 手感 / 洞口可通行 / 裂缝穿模）
  原样列进 §4 的"待人工"，交所有者现场判定，**不替所有者下结论**。
- 验证（命令 + 真实结果）：
  1. `cmake --build --preset debug` → 零警告；`ctest --preset debug` → **327/327 passed**（324 → 327）；
     门禁脚本 → **123 文件 0 违规**。
  2. 运行时冒烟（`Start-Process -RedirectStandardOutput`，完整落盘、正常关窗退出）：启动 16.5 s、`常驻集合已随窗口调整完毕`
     一条（`K=2` 下启动窗口 441 块 → 运行期窗口 378 块，卸载 63 块**零尖峰**）、43 s 内 `.err` 为空（无 WARN / 无 ERROR）。
- 遗留 / 待所有者：
  1. **§4 的"待人工"项**：走到 1 km 四边被挡、飞行越界后被送回、挖洞进出 + 走远再回来确认**洞不消失**、
     窗口边缘**无裂缝 / 穿模**。
  2. **卡顿消除（debug 未达成）**：建块爆发期；已按所有者裁定**登记为体验债**，待 release 实测再判。
  3. **CPU 侧分项内存记账**（ADR 0008 的 ≤150 MB 无逐项证据）—— 待补的**测量能力**，建议排入 ⓒ 或其后。
  4. **本批（T57 + V0.2 落盘 + T58 … + T62 + T63）尚未提交**。

---

## 2026-09-29  V0.3（ⓒ 表现与深度）开工：阶段计划落盘 + **T65 美术资源取回**

- 做了什么：
  1. **新建 `docs/plans/v0.3.md`**（阶段 ⓒ ＝ **表现与深度（美术资源先行）**）：按 SKILL「任务下发」写全
     **范围 / 顺序 / 落点 / 验收**，并补 **3A 基线三问**（每个交付物给 2~3 个具体参照 + **可判定判据** + 是否降级）；
     任务分解 **T65–T70**（资源与台账 → 地表 PBR 替换 → HDRI/IBL → Assimp 模型能力 → 主角落地 → 阶段验收）；
     明确不做（LOD / 后处理 / NPC 模型 / 音频 / 世界层语义改动）。
     **阶段编号口径**：本文件的 "V0.3" ＝ **第三份阶段计划**，**不改写** §8 原文的 V0.3（生成系统与持久化）；
     已在 `tech-plan-v2.0.md` §8 头部、`docs/adr/README.md`「当前阶段计划」、`plans/v0.2.md` 头部**三处互链**。
  2. **开工前先问清三件会改变"做什么"的事**（SKILL「降级必须先问」）⇒ 所有者裁定：**① 主角先用 CC0 占位人形**
     （CC0 里没有写实人形，与"美术方向 = 写实"冲突 ⇒ 先用占位把 Assimp + 骨骼动画管道跑通，**外形设定仍待所有者**）；
     **② 资源不入库：仓库只放下载脚本 + 校验和 + 许可台账**；**③ 顺序 = PBR → HDRI → 主角**。
  3. **T65 落地**：新增 **`tools/fetch_assets.ps1`**（幂等 + **逐项 SHA-256 校验**；`-Record` 写校验和；上游文件变化**报错**不静默）
     ⇒ 取回 **4 套地表 PBR**（草 `Grass 001` / 土 `Ground 037` / 岩 `Rock 030` / 沙 `Ground 093 C`，ambientCG，
     **均 2048² 四件套齐备**）+ **1 张户外 HDRI**（`Kloofendal 48d Partly Cloudy`，Poly Haven，2K `.hdr`）；
     `tools/assets.sha256`（**进仓库**，17 项）；`NOTICE.md` 新增「美术资源台账」（表 1 来源与许可 / 表 2 逐文件 SHA-256）；
     `.gitignore` 排除 `assets/textures/`、`assets/models/`；`docs/file-index.md` 登记 `tools/` 与两个资源目录。
  4. **顺带登记（先登记后实现）**：`game-design.md` §2.4 的「世界成立」行改为**部分实现**（T58–T63 已落地）；
     两行美术需求的阶段位置改为「**ⓒ（当前阶段）**」并指向 `plans/v0.3.md`；§3「外观模型」行补上**两个前置**与**风格冲突**；
     `engine-capabilities.md` 的「流式加载 / 卸载」行更新为**已实现（T61 起在游戏内生效）**；`NOTICE.md` 的 Assimp 行改为**前移**。
- 为什么：
  1. **先落计划再施工**（SKILL 硬规则）：ⓒ 的每一项都要有落点与可判定判据，否则"让项目好看"这种目标无法验收。
  2. **资源不入库**是所有者裁定 ⇒ 连带两件事必须一起做，否则会把项目弄坏：**① 校验和锁文件**（保证别人拿到同一份素材）
     与 **② 缺资源时游戏仍能启动**（T66 落地：`WARN` + 回落程序生成贴图）。这两条已写进 `plans/v0.3.md` §1.1。
  3. **踩过的坑（已归档）**：Windows PowerShell 5.1 按**系统代码页**读 `.ps1`，带中文的 UTF-8 **无 BOM** 脚本会炸成
     语法错误（`Unexpected token`）⇒ 脚本必须**UTF-8 with BOM**；已写入 `docs/learning-notes.md` Q31。
- 验证（命令 + 真实结果）：
  1. `powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1 -Record` → 5 项资源全部下载成功，
     输出台账 **17 个文件 + 各自 SHA-256**；
  2. **幂等性复跑**：第二次运行输出 `压缩包已在缓存中，跳过下载` × 5 + **`校验通过：全部资源与 tools/assets.sha256 一致`**；
  3. **规格自检**：16 张贴图实测 **2048×2048**（合计 ≈ 79 MB）、HDRI **6.3 MB**；目录 = `assets/textures/terrain/{grass,dirt,rock,sand}/`
     与 `assets/textures/env/`；
  4. 本阶段**未改任何 C++** ⇒ 构建 / `ctest` / 门禁状态不变（最近一次：零警告、**327/327**、门禁 0 违规）。
- 下一步 / 遗留：
  1. **T66（下一步）**：`materials.toml` 增贴图路径字段 + `world/terrain/material_textures.*` 接入 `texture_loader`
     （**首次接消费者**）+ 缺文件回落（WARN）；启动日志逐个打印"已打开 + 尺寸 + 通道"。
  2. **T67 的第一步是 ADR**（HDRI/IBL 超出 ADR 0010 质量线，须另开决策）。
  3. **P5 的后续**：占位人形**不是**主角外形设定 ⇒ 待所有者提供外形后重做外观（管道不变）。
  4. **V0.2 的人工目视 / 手感项仍待所有者**（T61 走动 / T62 四边 / T63 目视），与本阶段并行。
  5. **本批（T57 … T63 + V0.3 落盘 + T65）尚未提交**。

---

## 2026-09-29  T66 落地：**地表换成真实 CC0 PBR 贴图**（+ MSAA 默认 4×→2× 的显存取舍）

- 做了什么：
  1. **材质表新增可选段 `[textures]`**（`enabled` / `root` / `size`，`world/terrain/material_table.*`）：缺省 = 不启用
     ⇒ **旧文件照旧可用、`schema_version` 不变**；写了就按同口径校验（类型错 / `size ∉ [16, 4096]` / 启用但 `root` 空 ⇒ 抛）。
     目录约定 `<root>/<层名>/{albedo,normal,roughness,ao}.<jpg|png|…>`（层名 = `[[layer]].name`）。
  2. **加载器**（`world/terrain/material_textures.*`）：`MaterialTextureAssetLoader`（**分步**：每步一张贴图，
     与 `MaterialTextureBuilder` 同构 ⇒ 不冻结画面）+ `TryLoadMaterialTextureAssets`（一次跑完的便捷入口，供单测）；
     纯函数 `ResolveMapFile`（**固定扩展名顺序** jpg→jpeg→png→tga→bmp）与 `DownscaleBoxRgba8`（**整数倍** box 降采样、
     四舍五入、逐字节确定）；校验：必须正方形、**层间与件套间源尺寸一致**、源尺寸能被目标整除。
  3. **着色器 `textureMode` 分支**（`mesh.frag` + `MaterialUniform` 追加一个 `vec4`，288 → **304 字节**）：
     **真实模式 = 贴图取"绝对值"**（粗糙度 / AO 直接取贴图；层色 tint 由 CPU 侧置 1）、**宏观变化改用该层 albedo 放大采样**
     （真实资源没有"宏观噪声图"，也不额外占显存与采样器）；程序生成模式（回落路径）逐字不变。
  4. **上传与回落**（`game/main.cpp`）：真实模式上传 4 张 1024² 数组（macro 槽位绑 albedo）；**缺资源 / 解码失败 / 尺寸不合法
     ⇒ `VX_LOG_WARN` 并回落**程序生成占位贴图（资源不入库 ⇒ 干净克隆必然走这条路径，游戏必须照常启动）。
  5. **MSAA 默认 4 → 2**（`engine/platform/settings.hpp` + `ui-inventory.md`）：见"为什么"第 2 条。
- 为什么：
  1. **"贴图 ≥ 2K"是我在计划里写错的口径**：源资源 2048² × 16 层 RGBA 含 mip 要 **588 MB**，
     而 ADR 0008 的 VRAM 上限是 **300 MB**（当前非材质占用 244.87 MB）⇒ 2048² **物理上放不进预算**。
     按 SKILL「降级必须先问」当场提问，所有者裁定：**1024² + MSAA 默认 4×→2×**（1024² 需 85.33 MB；
     2560×1440 下 4× 的 MSAA 要 154.69 MB、2× 只要 70.31 MB ⇒ 腾出 84 MB）⇒ **实测总显存 245.83 MB ≤ 300**。
  2. **语义必须跟着贴图来源变**：程序生成的占位贴图是"单色细节 + ±20% 变化"，真实贴图是"绝对值" ——
     若沿用旧公式，真实粗糙度会被再乘一次层基准值（对比被抹平）、真实 albedo 会被再乘一次单色 tint（二次上色）。
     故用 `textureMode` 显式分支，而不是"调一堆数值凑"。
  3. **回落不静默**：资源不入库是所有者裁定，代价就是"别人的机器上必然没有资源" ⇒
     这条路径必须**每次都 WARN 并照常启动**，否则干净克隆会直接崩。
- 验证（命令 + 真实结果）：
  1. `cmake --build --preset debug` → **零警告**；`ctest --preset debug` → **331/331 passed**（327 → 331：新增 4 项：
     扩展名顺序 / box 降采样（含非整数倍拒绝与确定性）/ **缺资源软失败 + 可读原因** / `[textures]` 段解析与非法值抛错）；
     门禁 → **123 文件 0 违规**。
  2. **启动冒烟**（完整落盘 + 正常关窗退出）：`MSAA：2×`、`**真实美术贴图已加载**：1024² × 4 层 × [albedo/normal/roughness/AO]；
     源图 2048² ⇒ 整数倍 box 降采样`、`真实材质贴图已上传 … 含 mip 约 85.33 MB`、
     `GPU 纹理显存记账 … 合计 **245.83 MB**`（≤ 300）。
  3. **未覆盖（如实登记）**：贴图阶段的 16 步实测 ≈ 3.7 s（**每步 ≈ 230 ms**，加载画面照常出帧、进度前进，
     但单帧 > 50 ms）；**目视项**（"像真材质 / 法线方向 / 粗糙度对比"）留所有者。
- 下一步 / 遗留：
  1. **T67**：先落 `docs/adr/0021-environment-ibl.md`，再实现天空盒 + IBL（显存余量 ≈ 50 MB）。
  2. **切空间基的已知隐患**：真实模式沿用既有 TBN（由几何法线叉乘构造），与平面 UV 轴存在 90° 错位 ——
     对程序生成的各向同性噪声不可见，对真实各向异性法线可能表现为"凹凸朝向偏转"；若目视发现再单开任务修。
  3. **加载期单步 ≈ 230 ms**：属加载阶段（不冻结画面已满足）；若要在加载期也守住"无 > 50 ms 单帧"，
     需把解码下沉到工作线程（前置 = 线程设施，见 `references/concurrency.md`）⇒ 登记为候选优化，不擅自启动。
  4. **本批（T57 … T63 + V0.3 落盘 + T65 + T66）尚未提交**。

## 2026-09-29  T67：环境贴图与 IBL（天空 + 漫反射 irradiance + 预过滤高光 + BRDF LUT）

- 做了什么：
  1. **决策先行**：新增 `docs/adr/0021-environment-ibl.md`（**先落 ADR 再实现**）。理由是它**超出**
     [ADR 0010](adr/0010-render-quality-pipeline.md) 的"半球天空光"质量线 —— 属该 ADR 自己列出的"切换条件"之一。
     四个备选全部留档并写明切换条件：A 继续半球天空光 / B CPU 烘焙到 `R8G8B8A8`（**装不下 HDR**）/ C 只用 HDRI mip 近似高光 / D compute shader。
  2. **四个新 shader**（顶点阶段**复用 `tonemap.vert`**，不新增 `.vert`）：`sky.frag`（逆视图投影 → 等距柱状采 HDRI）、
     `ibl_irradiance.frag`（32×16 余弦加权卷积）、`ibl_prefilter.frag`（GGX 重要性采样，6 级 mip 按粗糙度分级）、
     `ibl_brdf_lut.frag`（Karis 解析拟合）。`game/CMakeLists.txt` 各加一条 `add_shader`。
  3. **`engine/render/environment.*`（新增模块）**：尺寸 / 级数 / `PrefilterRoughnessForMip` / `EstimateTextureMipChainBytes` / `HalfFromFloat`
     —— 都是**纯函数**（可单测、无 SDL 类型）。**原计划写作 `sky.*` / `ibl.*` 两个文件，实际合并为一个**（口径只有一份）。
  4. **`mesh_renderer.*`**：`BakeEnvironment`（三条全屏烘焙 pass 一次提交 + 等栅栏）、天空管线、环境纹理绑定、
     全屏通道辅助 `DrawFullscreenPass`（色调映射与三条烘焙**共用同一形状**）、显存记账新增"环境贴图"项。
     采样器从 6 个增到 9 个（set 2 binding 6..8）。
  5. **`lighting_table.*`**：`[environment]` **可选段**（`enabled` / `hdri`）+ `BuildLightingUniform` 的 IBL 启用位与预过滤 mip 级号。
  6. **`game/`**：新增加载阶段 `Environment`（**解码 → 烘焙两步**，之间照常出加载画面）；缺 HDRI **WARN + 回落**。
- 为什么：
  1. **半球天空光有三个可观察缺陷**（天空不可见 / 环境光与"看到的天空"不同源 / 低粗糙面在阴影里几乎全黑），
     而这三条**只有环境贴图能表达方向性** —— "再调几个常量"做不到。所有者 2026-09-29 裁定做**完整三件套**。
  2. **天空与主通道共用同一个渲染通道**（深度测试与写入关闭 ⇒ 网格自然覆盖天空）：省掉第二条颜色目标，
     也避开了 MSAA 的 resolve 冲突；代价是**天空管线必须随 MSAA 档位重建** ⇒ 与主通道同生共死（`CreateMainPipeline` 一并重建）。
  3. **"配置要求 ≠ 真的烘焙成功"**：`BuildLightingUniform` 收的是**实际烘焙成的 mip 级数**（0 = 回落），
     而不是"配置里 enabled" —— 否则资源缺失时着色器会去采样并不存在的环境贴图。
  4. **天空不带 mip 链**：预过滤只有 6 级输出、且不做 pdf 选级（已登记取舍）⇒ 源图给 mip 只增显存不增质量；
     实测 HDRI 因此 16.78 MB 而非预估的 22.4 MB。BRDF LUT 用 `R16G16B16A16_FLOAT` 而非 ADR 里的 `RG16F`：
     它是主通道 HDR 目标**已经在用**的格式，渲染目标支持面最广，代价 0.25 MB（ADR 0021 后果 1 已回填说明）。
  5. **未烘焙时绑 1×1 占位纹理**：`mesh.frag` 恒声明 binding 6..8，SDL_gpu 要求声明的采样器都有绑定；
     此时 `fogParams.z = 0`、着色器整段跳过采样 ⇒ 占位内容无意义，只求"有合法绑定"（**不能**借 HDR 目标占位：它正被写）。
- 验证（命令 + 真实结果）：
  1. `cmake --build --preset debug` → **零警告**（警告即错误）；`ctest --preset debug` → **350/350 passed**
     （331 → 350：`environment_test` 12 项 + `lighting_table_test` 7 项）；门禁 → `scanned 126 file(s), 0 violation(s)`、`PASS`。
  2. **启动冒烟（有资源，40 s 后强制结束）**：
     `**环境贴图已烘焙**（IBL 三件套）：HDRI 2048x1024（半精度上传、线性光）；irradiance 32x16 + 预过滤 6 级 mip（128x64 起）+ BRDF LUT 256²；环境贴图显存 16.59 MB；烘焙耗时 165 ms`
     + `GPU 纹理显存记账：… + 环境贴图 16.59 MB = 合计 **262.42 MB**`（≤ 300）；全程 **无 WARN / 无 ERROR**，`角色落地自检` 正常。
  3. **回落冒烟（临时把 HDRI 改名后运行，跑完已还原）**：
     `[WARN] 环境贴图不可用 ⇒ **回落半球天空光**（不静默）：… 无法打开图像文件（不存在或没有读取权限）…`，
     随后照常 `地表世界就绪`、合计回到 **245.83 MB** 基线 ⇒ **缺资源不崩、不静默**（ADR 0021 第二条）。
- 下一步 / 遗留：
  1. **人工目视项（待所有者）**：天空可见且与地形环境光**同源**（同一张 HDRI）；低粗糙面在阴影里**不发黑**；
     地平线处雾色与天空的融合（见下条）；帧时间不劣化 > 5%（**需 release 复测**，debug 下无对比价值）。
  2. **雾色仍是常量**（`[sky].horizon_color`）⇒ 地平线附近可能与 HDRI 的地平带不吻合。**已登记为已知限制**
     （ADR 0021 后果 7 + 切换条件），未一并实现：从 HDRI 推导雾色要定义它与显式 `fog.color` 的优先级，属独立小决策。
  3. **太阳方向不由 HDRI 推导**（ADR 0021 后果 5 的已知限制，未变）：若目视发现"天空里的太阳位置 ≠ 阴影方向"再补。
  4. **本批（T57 … T63 + V0.3 落盘 + T65 + T66 + T67）尚未提交**。

---

## 2026-09-29  T71 性能诊断：**"挖洞当帧成本"的三档对照**（基线 / 关塌落 / 减小破坏体积）

- 做了什么：
  1. **新增 `build/vx_perf_ab.ps1`**（**只改 `assets/config/*.toml`、不改任何 C++** 的对照脚本）：三档跑**完全相同**的输入序列
     （加载 → 点一下重捕获鼠标 → 按住 `W` 前进 4 s → 按住左键连发 12 s → 静置 12 s 等延后队列排空），
     每档结束把日志另存，**最后把两个配置文件按字节还原**（`finally` 块 + 备份副本；跑完实测
     `explosion_radius = 6.0`、`collapse.enabled = true` 已复原）。
  2. **三档**：**基线** = 原配置（`collapse.enabled = true` / `explosion_radius = 6.0`）；
     **A = 关塌落**（`enabled = false`）；**B = 减小每次破坏的体积**（`explosion_radius = 6.0 → 4.0`）。
  3. **度量取自两处既有内建打点**（没有新写任何观测代码）：① 每发爆炸的
     `爆炸（当帧 = 挖除 + 抽出 + 刚体化）：… 挖除 %.2f ms / 倒塌 %.2f ms（…）⇒ 入队 %zu 个块的延后工作`；
     ② 超 33 ms 的 `帧尖峰 …（逻辑 + UI + 渲染提交 + 动态上传 + uniform + 限帧 = …，**未计时 …** ms）`。
     日志完整落盘：`build/perf/{baseline,A_collapseoff,B_radius4}.{out,err}.log`。
- 为什么：所有者反馈"测试地图十分卡顿、尤其挖洞时"，并提出"能否把大部分地图改成只有贴图 / 不可挖材质"。
  按 SKILL「缺陷报告先判真伪」+「观测先于结论」，**先量再改**：把"挖洞当帧"的钱花在哪三项
  （挖除 / 倒塌 / 延后块级工作）上分开，才能判断"减小破坏体积"是否对症。
- 验证（**debug** 构建，1280×720，同一段输入序列；三档汇总）：

  | 档 | 爆炸数 | 挖除 avg / max ms | 倒塌 avg / max ms | 入队块 avg | 刚体化整体 | 帧尖峰 数 / 峰值 ms | 逻辑 Σ / 峰值 ms | 未计时 Σ / 峰值 ms |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | 基线（r=6，塌落开） | 28 | **33.10 / 43.47** | 16.94 / 39.63 | 1.71 | 3 | 31 / **182.0** | 1958.7 / 168.66 | **2369.6 / 154.39** |
  | A 关塌落（r=6） | 20 | 31.65 / 44.63 | **0 / 0.01** | 1.95 | **0** | 25 / 130.6 | 1811.2 / 142.10 | 903.0 / 112.04 |
  | B 减小体积（r=4） | 20 | **13.45 / 17.47** | 6.37 / 40.92 | 2.15 | 2 | 24 / 133.1 | 1449.1 / 153.15 | 1103.4 / 98.91 |

  结论（每条都能指到上表）：
  1. **单发当帧成本 = 挖除 + 倒塌**。基线 ≈ **50 ms/发**（一帧预算 16.7 ms 的 3 倍）⇒ "挖洞必卡"**属实**，
     不是错觉；且**挖除本身（33.10 ms）比倒塌（16.94 ms）更贵**。
  2. **挖除成本按半径三次方增长**：6.0 → 4.0 使挖除 **33.10 → 13.45 ms（−59%）**、峰值 43.47 → **17.47 ms（≈1 帧预算）**。
     ⇒ **所有者提的"减小每次破坏的体积"对症**，且是本轮唯一直击"每发成本"的旋钮。
  3. **关塌落能拿掉 16.94 ms/发（当帧总成本 −34%）与未计时峰值的一半，但尖峰并不消失**
     （仍 25 条、峰值 130.6 ms）⇒ **塌落不是主因**；剩下的钱花在（a）**逻辑相位**
     （峰值 142~169 ms，日志显示 `固定步 5` ⇒ 一帧跑 5 个固定步，含倒塌刚体的 Jolt 步进）与
     （b）**未计时桶**（峰值 98~154 ms，疑为延后队列的重网格 / `MeshShape` 重建 / 阻塞式上传；T45 的
     "未计时未归属"仍未关闭）。
  4. **入队块数只有 ~2 块/发**（不是 T37 注释担心的 6~7 块）⇒ 块级工作**不是**本次打击点的主要来源。
  5. **A 档的 `刚体化整体 = 0`** 反证了开关确实生效（基线 3、B 2）；三档都产生了爆炸与挖除 ⇒ 目标命中。
- 下一步 / 遗留：
  1. **已按所有者裁定"改默认值"落地**：`projectiles.toml` 的 `explosion_radius` **6.0 → 4.0**（含"为什么 / 代价 / 何时可改回"注释）；
     `damage = 10` **未改**（仍作用于器物与战斗语义）；`collapse.enabled` 保持 `true`（A 只是诊断，不是默认）。
     降级三点登记见 `game-design.md`（G12 与 §2.3「破坏地形」行）。
  2. **仍未关闭**：① 逻辑相位 142~169 ms 的归属（`固定步 5` 与倒塌刚体数量）；② 未计时桶归属（T45 遗留）；
     ③ **release 复测**（本轮全为 debug，绝对值不可外推 —— 口径同 T66）。
  3. **未验证**：飞行 / 在岩壁上开炮 / 靠近 1 km 边界时的表现（本轮打击点固定在出生台地前方）。
  4. 实验脚本与三档日志留在 `build/perf/`（`build/` 不入库），可复跑。

---

## 2026-09-29  T72 定位"不挖坑也卡"：**根因是常驻窗口的 tile 翻转**（每次重建 63 块）

- 做了什么：
  1. 所有者报告**单纯走动 / 转动视角 / 飞行**也卡。先复查 T71 的基线日志，发现**走动相位**（29.2~31.2 s，尚未开炮）
     就有 **7 条 69~140 ms 尖峰**，且紧接着 `[31.279] 可挖体积常驻集合已随窗口调整完毕：玩家 tile (0, 0) ⇒ 常驻 441 块`
     —— 即走动把窗口从 tile `(0,-1)`（378 块）挪回 `(0,0)`（441 块）⇒ **一次性要新建 63 块**。
  2. 新增 `build/vx_perf_input.ps1`（四档输入脚本：`stand` 静止 / `rot` 只转视角 / `fly` 飞行上升 / `walk` 走动；
     **不改配置、不改代码**），并加**输入已送达的日志标记**（按 F 会打 `飞行模式：开` 日志）。
  3. 逐档采集 `帧尖峰` 与 `ADR 0020` 常驻事件，把"尖峰"与"窗口翻转"逐次对齐。
- 为什么：SKILL「缺陷报告先判真伪」+「观测先于结论」—— 先证明"卡"对应哪条机制，再谈改法；
  不做数据支撑的判断（禁止"应该就是这个原因"）。
- 验证（debug，2560×1369 + MSAA 2×；**尖峰时段与窗口翻转逐次对齐**）：

  | # | 档 | 尖峰时段 | 条数 | 逻辑峰值 ms | 紧随其后的翻转日志 | 方向 |
  | --- | --- | --- | --- | --- | --- | --- |
  | 1 | stand（**零输入**） | 30.5~32.5 | 7 | 157.3 | `[32.530] tile (0,0) ⇒ 441 块` | **建 63 块（贵）** |
  | 2 | stand（**零输入**） | 41.2~42.4 | 2 | 147.1 | `[42.425] tile (0,0) ⇒ 441 块` | **建 63 块（贵）** |
  | 3 | stand（**零输入**） | 49.5 | 1 | 163.4 | `[49.616] tile (0,-1) ⇒ 378 块` | 卸 63 块（便宜） |
  | 4 | 站立（20 s） | 40.9~44.9 | 8 | 157.7 | `[45.135] tile (0,0) ⇒ 441 块` | **建 63 块（贵）** |
  | 5 | T71 基线（**走动**） | 29.2~31.2 | 7 | 168.7 | `[31.279] tile (0,0) ⇒ 441 块` | **建 63 块（贵）** |
  | 对照 | 无翻转的 20 s（多档） | — | **0** | 0 | 无 `常驻…调整完毕` | — |

  结论（每条都能指到上表 / 日志）：
  1. **唯一触发因素是"常驻窗口的 tile 翻转"**：**有翻转 ⇒ 7~10 条 33~175 ms 尖峰；无翻转 ⇒ 20 s 内零尖峰**。
     尖峰几乎全部落在**逻辑相位**（62~168 ms）⇒ **CPU 侧**，不是 GPU。
  2. **只有"建块"方向贵**（#1/#2/#4/#5）；**"卸载"方向便宜**（#3 反向翻转那一次没有尖峰）——
     与 T61 实测"108 次卸载零尖峰、建块是瓶颈"一致。
  3. **不移动也会翻**（#1/#2/#3 是**零输入**的 stand 档；#4 只站着）⇒ 证明翻转**不需要跨 tile 的位移**：
     出生点 `(0.00, 120.00, -0.00)` **正好压在 tile 边界上**（启动日志已记 `预设出生点 tile (0,0)` 与
     `物理位置 tile (0,-1)` 不一致，根因是 `z ≈ -1e-9`），任何亚格级抖动都会让 **5×5 tile 窗口**的中心在
     `(0,-1) ⇄ (0,0)` 之间跳；而 `DigVolumeScheduler::Update` **只在"中心 tile 未变"时幂等**（无滞回）⇒
     一跳到就整体重规划 ⇒ **一次抖动 = 63 个块的建/卸**（9 层 × 7 块）。
  4. **单块成本决定尖峰高度**：`CreateBlock` ≈ **22 ms**（填密度 + Surface Nets）+ 每块**一次阻塞到 GPU 完成的上传**
     ⇒ 63 块 ≈ 1.4 s 的 CPU 工作被摊到 ~40 帧，每帧都远超预算（这也解释了 `固定步 4~5`、`逻辑 62~168 ms`）。
  5. **另一类、不同源的冻结**：`[20.907] 帧尖峰 1339.0 ms … **未计时 1322.65** ms`（99% 未计时）出现在
     **加载期**（首帧渲染后）⇒ 属"**首次命中才创建**"的管线 / 资源创建（SKILL 自查清单明确列为只允许在
     加载期或**分帧创建队列**里出现；此处发生在加载期，但与"转动视角"**无关**）。
- 下一步 / 遗留：
  1. **未验证（须如实说明）**：本次自动化**无法可靠地把输入送到游戏**（`SetForegroundWindow` 常被系统拒绝，
     游戏窗口的前台状态取决于本机的人工操作）⇒ 后两批 `walk` / `flyfwd` 档的"0 尖峰"**很可能是"输入没送达"**，
     **不得**据此下"走动/飞行不卡"的结论；走动触发翻转的证据取自 **T71 基线档（#5，真人操作）**。
     水平飞行（F + W）**尚未测到有效样本**。
  2. **待所有者裁定（降级 / 改动必须先问）**，候选（可组合）：
     **(A) 加窗口滞回**：中心 tile 只在越过边界一定带宽后才切换（并保证一次只挪一格）⇒ 直接消灭"站在边界上反复翻"；
     **(B) 窗口半径 `K: 2 → 1`**（5×5 → 3×3 tile）⇒ 单次翻转的块数下降，但**可挖三维洞的范围随之缩小**（属降级，需确认）；
     **(C) 压单块成本**（治本，需改代码）：`CreateBlock` 拆"填密度 / 网格化"两步、上传改非阻塞且分帧、
     之后**下沉到 enkits worker**（网格化是可并行的纯计算，只有 GPU 上传必须留主线程）；
     **(D) 出生点移出 tile 边界**（治标：只消灭"零输入也翻"那一半）。
  3. **加载期 1339 ms 冻结**（第 5 条）建议单列一个小任务（分帧创建管线 / 预热），不并入本项。

---

## 2026-09-29  T73 修掉 T72 的根因：**窗口中心 tile 加滞回**（翻转 4 → 0、逻辑峰值 163 → 62 ms）

- 做了什么：
  1. **纯函数 `HysteresisCenterTile`**（`world/streaming/dig_volume_residency.*`）：相差 0 不变；相差 **≥ 2 格**
     （传送 / 越界救援）**一次跳到目标**；相差 **恰好 1 格**时，只有玩家**越过边界 ≥ `kWindowHysteresisBlocks`** 才挪一格。
     **`band = 0` ⇒ 等价于本项之前的行为**（保留为对照口径，供单测使用）。
  2. **常量 `kWindowHysteresisBlocks = 16.0`（格 = 1/4 tile）**：窗口半径 2 tile = 128 格 ⇒ 越过边界 16 格以内时
     **旧窗口仍完整覆盖玩家**（不影响"脚下能不能挖"）；16 格按步行 9 格/秒约 1.8 s、飞行 28 格/秒约 0.57 s 的容差。
  3. `DigVolumeScheduler::Update` 改为用上面两条按轴推进中心 tile（其余逻辑不变：仍是"先建后卸、分帧推进、脏块留驻"）。
  4. **单测 +3**（`tests/dig_volume_residency_test.cpp`）：带宽内不动（含**出生点 `±1e-9` 抖动不得切换**）/
     远跳一次到目标 + `band=0` 等价旧行为 / **边界抖动 64 次不得产生任何待办**（集成测，含常驻集合不变）。
  5. **配套**：`ProjectileSpec::explosionRadiusBlocks` 的内置默认值同步 `6.0F → 4.0F`（T71 改了配置文件，
     而 `orb_test.cpp` 有"内置默认 == 仓库文件"的不变量单测 ⇒ 必须先修默认值，否则 ctest 红）。
- 为什么：T72 已用实测把"不挖坑也卡"锁定为**窗口 tile 翻转 ⇒ 63 块重建**；而翻转的触发门槛低到"站在 tile 边界上、
  亚格级抖动即可"，且 `Update` 只在"中心 tile 未变"时幂等（**无滞回**）。方案 A 是所有者选定的最小改动修复。
- 验证（命令 + 真实结果）：
  1. **构建**：`cmake --build --preset debug` → 零警告（`/W4 /WX`）；**测试** `ctest --preset debug` → **353/353 passed**（350 → 353）；
     门禁 `check-banned-identifiers.ps1` → `scanned 126 file(s), 0 violation(s)`、`PASS`。
  2. **运行时对照（同一 `stand` 档：加载 → 原地不动 20 s，**零输入**）**：

     | 指标 | 改前（T72） | 改后（T73） |
     | --- | --- | --- |
     | `ADR 0020` 常驻事件行数 | **7**（= 4 次翻转） | **2**（= **0 次翻转**，连启动那次 441→378 也没了） |
     | 帧尖峰条数 | 10 | 6 |
     | 尖峰峰值 | 170.3 ms | 85.8 ms |
     | 逻辑相位 Σ / 峰值 | 1130.4 / **163.4 ms** | 272.8 / **62.3 ms** |
     | 同档开炮次数（本机有人操作） | 0 | 6（**改后剩的 6 条尖峰与这 6 次开炮一一对应**，与窗口无关） |
  3. **顺带解决**：启动期"预设出生点 tile (0,0) vs 物理位置 tile (0,-1)"的不一致**不再引起启动 63 块卸载**
     （滞回把 `-1e-9` 的越界判为"未越过带宽"）。
- 下一步 / 遗留：
  1. **决策已回写** [ADR 0020](adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二（第二次修订：滞回），
     并在 `docs/engine-capabilities.md` 的「流式加载 / 卸载」行注明。
  2. **仍未关闭**：① **加载期 1339 ms 首帧冻结** ⇒ 已单列为 **T74**（分帧创建管线 / 资源）；
     ② **"走动 / 水平飞行是否仍卡"未有有效样本**（自动化送输入不可靠）⇒ **需真人在场复测**；
     ③ 每发开炮的当帧成本（T71：挖除 ≈13 ms + 延后网格 / 上传）仍是可感知的尖峰来源，属候选 **C**（未启动）。
  3. 本轮**未动** `K`（仍为 2）、**未改** 出生点、**未改** 塌落与伤害数值。

---

## 2026-09-29  T75/T76/T77：**SKILL 立"业界标准优先"硬规则** + **网格上传改"提交即走"** + **业界标准优化清单**

- 做了什么（三件事，按所有者 2026-09-29 的三条指示）：
  1. **T75 —— SKILL 新增顶层硬规则「业界标准优先」**（`.trae/skills/.../SKILL.md`，4 处）：
     ① 新规则正文（适用范围 = **任务下发 / 缺陷修复 / 方案指定**；三步走 = 先查清现象（只是输入）→ **再先查业界标准做法并作为方案第一候选**（必须点名 2~3 个参照）→ 用不了才按「降级必须先问」改替代）；
     ② 「需求受理」第二步加"**优先级**：业界标准 > 对当前现象的片面分析"；
     ③ 「缺陷报告」加"**修复方案必须首选业界标准做法**，只做现象层修补 = 未闭环"；
     ④ DoD 新增一条核对项。**范围控制与暂缓项未被放宽**。
  2. **T76 —— C2：网格上传改"提交即走"（业界标准做法见下）**：
     `MeshRenderer::UploadMesh` 不再"建 transfer buffer + 同步等 fence"，改为 **按容量建缓冲 → 复用常驻 staging → `UpdateMeshGeometry` 提交即走**；
     新增 `create_buffer`（只创建、不上传）；删除已无用的 `create_and_upload_buffer`；
     `UploadVolumeMesh` / `UploadTileMesh` 的**重网格**改走 `UpdateMeshGeometry` **快路径**（容量不足才重建，**WARN 不静默**），体积块按 **2× 预留**；空网格改为"就地置空"（零上传、零释放）。
  3. **T77 —— 业界标准优化机会扫描（只读）**：五类共 14 项，见下表。
- 为什么（业界标准优先，点名参照）：
  1. **C2 的标准做法**：**Sodium** 把区块构建结果交渲染线程做**批量、限额**上传、并在导入时不做同步栅栏等待；**Unity** 的 `MeshDataArray` + `ApplyAndDisposeWritableMeshData` 明确是"一次 apply、批量"；**SDL_gpu** 自身的标准用法是"提交即走 + 用 fence **非阻塞**回收 transfer 资源"。
     我们的旧路径每块 = **建 2 个 transfer buffer + 2 个 GPU 缓冲 + 2 次 `SDL_WaitForGPUFences`**（`mesh_renderer.cpp` 旧 `create_and_upload_buffer`），此后每次重网格**全部重来** ⇒ 与上述三者都不符。
     引擎里**本来就有**符合标准的 `UpdateMeshGeometry`（单命令缓冲、只传用到的前缀、**不建资源、不等栅栏**）—— 属于"能力已具备、调用方未使用"的欠账。
- 验证（命令 + 真实结果）：
  1. 构建 `cmake --build --preset debug` → **零警告**（`/W4 /WX`）；`ctest --preset debug` → **353/353 passed**；门禁 → `scanned 126 file(s), 0 violation(s)`、`PASS`。
  2. **冒烟（`build/vx_perf_input.ps1 -Mode shot`）**：加载 **441 个体积块 + 289 个地表 tile 全部走新路径** ⇒
     `可挖体积就绪：441 个块…其中 128 块存在等值面`、`地表世界就绪`、`首帧视锥剔除…提交网格 225 个`、
     显存记账 **256.87 MB** 正常，**stderr 0 行（零 WARN / 零 ERROR）**。
  3. **未覆盖（如实登记）**：本次**没有产生爆炸**（`explosions=0` —— 自动化未把点击送达游戏窗口，见 T72 的"输入不可靠"）
     ⇒ "**脏块重网格走快路径**"这一条**只有代码级证据、没有运行时证据**，待人工开炮后从日志确认（不应再出现"超出容量"的 WARN，除非确实变大）。
- **T77 优化机会清单**（只读扫描；**未获批前一律不得开工**；完整证据见本表"证据"列）：

  | 类别 | 问题（证据） | 业界标准做法（点名） | 预期收益判据 | 代价/风险 | 性质 |
  | --- | --- | --- | --- | --- | --- |
  | 提交 | **阴影通道按 3 级级联各重画全部网格**（`mesh_renderer.cpp:1699-1731`；实测 draw call 750~930） | UE/Unity 的 **per-view / per-cascade 剔除** | draw call 降到 ≈1.3~2×N | 需把级联盒交给 CPU 侧；有丢阴影回归风险 | 欠账（T40 已登记，**须先选路**） |
  | 提交 | **MSAA 档位切换在 `RenderFrame` 内重建管线**（`mesh_renderer.cpp:1671-1677`） | UE **PSO cache / 预建**；创建移出热路径（SKILL 硬规则 4） | 切档当帧不再出现在帧尖峰日志 | 极小 | 欠账 |
  | 提交 | 逐网格 3 次 API + 逐网格 64 B uniform 推送（**当前实现合规**，`mesh_renderer.cpp:1564-1582`） | Unity **SRP Batcher / Indirect**；UE **GPUScene** | 提交相位 < 0.5 ms（**收益存疑**，须先测） | SDL3_gpu 是否有间接绘制**待核实** | 纯增强（默认不做） |
  | 流式 | **建块在主线程同步做**（`main.cpp:2110-2113`、`dig_volume.cpp:281-313`；单块 ≈22 ms） | **Sodium chunk builder**（worker 池）/ UE5 **Task Graph** / Horizon **streaming budget** | 窗口挪动期间逻辑峰值（现 62.3 ms）≤ 帧预算 | 线程模型变更；需快照 + 版本号（红线 9） | 欠账（= T72 候选 C，**须先获批**） |
  | 流式 | **上传同步阻塞**（旧路径，**T76 本轮已修**） | SDL_gpu 标准用法 + Sodium 批量上传 | 已达成 | — | **已修（T76）** |
  | 流式 | **预取缺失**：窗口按当前位算、看到才开始建（`dig_volume_residency.cpp:117-182`） | Minecraft 按行进方向**预取**；id Tech page residency | 窗口挪动期间偏长帧消失 | 常驻块数上升（内存）；与 T73 滞回协同 | **新增**（ADR 0020 未含，**须先获批**） |
  | 流式 | 碰撞体粒度 + **全仓未调用 `OptimizeBroadPhase()`**（`physics_world.cpp:335-352` 等） | **Jolt 官方**：批量增删静态体后优化 broadphase | physics 相位耗时 | 一行调用；**待核实** Jolt 是否已自动优化 | 欠账（低风险小改动） |
  | CPU | **`CarveByDamage` 当帧 13.45 ms**（O(r³) 枚举 + sort + 3 个 r³ 掩码 + 洪泛；`dig_volume.cpp:563-726`） | UE **time-sliced work** / Sodium worker | T71 同一脚本的"挖除 ms" ≤ 8 | 低（局部重构 + 单测钉住） | 欠账 |
  | CPU | **`RasterizeBall` 遍历全部常驻块**（`dig_volume.cpp:383-396`） | 由球心推**受影响的块范围**再索引（Sodium 哈希定位） | 单块挖除耗时 | 低 | 欠账 |
  | CPU | **每帧 O(tile) 遍历**（`main.cpp:2389`、`terrain_world.cpp:117-126`、`main.cpp:2227-2239`） | 脏标记 + 增量维护 | uniform / 剔除相位耗时 | 低 | 欠账 |
  | CPU | **加载期分帧"单步无上界"**（`main.cpp:1029-1040`、`1494-1499`） | UE 加载期毫秒配额切分；解码下沉 worker | 加载期单帧 ≤ 50 ms | 中（JPEG 解码不易细分） | 欠账 |
  | CPU | **enkits 已引入未使用**（`engine-capabilities.md:108-109`） | UE Task Graph / Unity Job System（本项目 = **ADR 0003 既定选型**） | 是上面"下沉 worker"各项的前置 | 中 | **新增**（启用需登记） |
  | 内存 | **`ReadColumnHeight` / `WriteColumnHeight` 线性遍历全部 tile**（`terrain_world.cpp:75-99`），而 `QueryHeight` 是每帧热路径 | 列 → tile **直接索引** / 空间哈希 | 相机避障 + 弹道查询、`BuildSurfaceHeightCache` 成本 | 低；须用单测钉住"边界共享列"语义 | 欠账（**小改动高收益**） |
  | 内存 | **`VolumeSlotTable` 用 `std::map`**，每帧全量迭代；`SkipQuad` 每四边形 4 次查找（`main.cpp:546-588`、`2233-2239`） | 坐标哈希 / 窗口内稠密数组（UE `TMap` + 空间哈希） | tile 重网格与剔除耗时 | 低；换 `unordered_map` 时**迭代顺序不得影响结果**（红线 7） | 欠账 |
  | 内存 | **热路径每次分配大临时缓冲**（`dig_volume.cpp:572`、`614-628`、`661`；红线 10） | 持久 scratch / 对象池 / 帧分配器 | 挖除耗时 | 低 | 欠账 |
  | 加载 | **首帧 1339 ms（T74）**：队列单步无上界 + `BuildSurfaceHeightCache` 在两次 `Pump` 之间裸跑（`main.cpp:1493`、`dig_volume.cpp:241`、`terrain_world.cpp:75-85`） | UE 加载期**分帧创建队列 + 预热清单** | 该尖峰消失、加载期单帧 ≤ 50 ms | 需先打点拆分（**归因待实测**） | 欠账（已登记 T74） |
  | 加载 | **首个加载画面之前无出帧窗口**（`main.cpp:1195/1206/1235`，`mesh_renderer.cpp:261-545`） | UE splash → load map 顺序；SKILL「不冻结画面」第 1 条 | 到首帧可视化的时间 | 中 | 欠账 |
- 下一步 / 遗留（**须逐项获批，不得自行开工**）：
  1. **建议顺序（高收益 / 低风险 / 小改动优先）**：① `QueryHeight` 类改 O(1) 定位；② T74 打点拆分加载期首帧；③ `OptimizeBroadPhase()`（先核实）；④ 挖除路径串行提速 + 复用 scratch；⑤ `VolumeSlotTable` 换稠密/哈希；⑥ MSAA 管线重建移出 `RenderFrame`。
  2. **需所有者先裁定（属降级 / 线程模型 / 范围）**：**窗口预取（业界第一项）**、**启用 enkits + 建块下沉 worker（T72 候选 C）**、**按级联剔除（T40，须先选路）**、**显存旋钮（阴影级联分辨率 / MSAA，属画质降级）**。
  3. **纯增强 / 阶段外**（默认不做，仅登记）：批合并 / 间接绘制 / GPU-driven、逐网格 uniform 改 storage buffer 实例数据；遮挡剔除与 LOD（待收敛项 4 / 6）仍延后。

---

## 2026-09-29  T78 修复：**地表 tile 加光栅化深度偏移**（消除区域边界那一圈的共面 z-fighting）+ T79 回退与构建教训

- 做了什么：
  1. **缺陷判定（契约 + 机制，所有者已确认现象位置）**：契约 = [ADR 0011](adr/0011-layer-transition-volume-takeover.md)"**四角全部**在可挖体积内才跳过地表四边形"（T61 起输入换成**当前常驻集合**）；机制 = 常驻集合的**边界环**上四角不全在块内 ⇒ **地表 tile 网格与体积网格共面同画** ⇒ z-fighting。所有者确认"闪烁集中在**中间大方形平台边界那一圈**" ⇒ **确认是缺陷**。
     并查明**原缓解为何失效**：`assets/config/dig_regions.toml` 当年把可挖区"铺满整张图"是为了**把这一圈推到世界边缘**，但 **T58 把世界扩到 17×17 tile（1 km）而可挖区仍是手写中心 3×3 tile** ⇒ 边界环落到了地图正中、可见。
  2. **修复（业界标准：共面两层的标准手段 = 光栅化 depth bias / polygon offset）**：`MeshRenderer` 增加**按网格的深度偏移标记**与**"带偏移的主管线段位变体"**（与既有"每 MSAA 档重建管线"同一处创建/重建 ⇒ **不在 `RenderFrame` 热路径创建**，符合 SKILL 硬规则 4）；`DrawMeshes` 按标记**成区间**切管线绑定（最多 2 次，不退化）；**阴影通道不加偏移**；`UploadTileMesh` 传该标记，体积块 / 角色 / 光球 / 倒塌一律不传。
     **不采用**"tile 粒度接管 + 接缝缝合"那条更彻底的路线（改动大、有裂缝风险）⇒ 已在 `dig_regions.toml` 与本条登记为**后续可选切换项**。
  3. **一处必要的参数偏离（非静默降级，已写明）**：偏移量取 **`100.0F` / slope `1.0F`**（并非常见的 `1e-3~1e-2` 量级）—— 因为 **D3D12 后端会把 `depth_bias_constant_factor` 取整**（`SDL_lroundf`），`1e-3` 在本机会被取整成 0、**完全失效**；`100` 等效归一化偏移 ≈ `1.2e-5`，在 D3D12 / Vulkan 上一致生效。注释标注为"**待实测微调的经验值**"。
  4. **顺带纠正一处我此前的误判**：`lighting.toml` 的 `depth_bias = 0.0015` 是**片元内的阴影比较偏移**（`mesh.frag` 的 `shadowParams.z`），**不是**光栅化 depth bias；本仓光栅化 `enable_depth_bias` 此前恒为 0，本次是**第一处**。
- 为什么：这是**可见缺陷**（违背 ADR 0011 的层间接管契约），且所有者报告的现象与机制链完全吻合；按"缺陷先判真伪 → 修复方案首选业界标准做法"执行。
- 验证（命令 + 真实结果）：
  1. `cmake --build --preset debug --clean-first` → **零警告**（`/W4 /WX`）；`ctest` → **355/355 passed**；门禁 → `scanned 126 file(s), 0 violation(s)`、`PASS`。
  2. 冒烟：启动正常（`地表世界就绪…289 tile`、`可挖体积就绪：441 块…128 块存在等值面`）、显存记账 **262.42 MB ≤ 300**、stderr **无 ERROR**。
  3. **人工目视项（待所有者）**：**看哪里** = 飞离中心平台一段距离再飞回、静止注视那块大方形平台的**边界那一圈**地面；**期望** = 闪烁消失、且地表相对体积**无可见下沉 / 台阶**。若仍闪 ⇒ 上调常量（100 → 300 / 1000）；若见台阶 ⇒ 下调。
- **T79 回退（如实记录，不做"已完成"处理）**：本轮曾用一个并行子任务实现 T79 的三项（`TerrainWorld` 列查询改 O(1)、`OptimizeBroadPhase`、挖除路径 scratch/块范围提速），
  但在**首次构建后**出现大批既有测试失败（`VolumeCollapse*` / `VolumeCollision*`，含访问违例与 `xmemory` 断言）。**回退 T79 全部改动**、随后**干净重建** ⇒ **355/355 全绿**。
  **因此：T79 三项一律回到"未开始"**（既有实现已复原），需要重做。
- **教训（新增，价值最高）**：**不得让多个子任务（或人）并发对同一个 CMake build 目录做增量构建**。
  本轮两个子任务并行改不同文件、又各自/交替跑 `cmake --build`（其中一个还用了 `--clean-first`），导致**目标文件与头文件依赖不一致** ⇒ 出现"与改动无关的测试大批失败"的假象，浪费了一轮排查。
  **规则**：并发改代码时必须**串行构建**，出现"诡异失败"时**先 `--clean-first` 重建再判**；另注：本项目增量构建**未跟踪头文件依赖**（只改 header 时曾出现 `LNK2019`），改头文件后一律视为需要干净重建。
- 下一步 / 遗留：**T79（重做）**、**T80（窗口预取；分叉已定 = 只扩常驻半径、`K` 不变）**、**T81（enkits + 建块下沉 worker；须先落 ADR）** 均**未开始**；
  T78 的"闪烁是否消失"属**人工目视验收**（自动化无法可靠送输入）。

---

## 2026-09-29  T78 缺陷修复：**飞远再回来地面边界一圈闪烁** —— 地表加光栅化深度偏移（depth bias）

- 做了什么（**只改渲染侧**，不动世界层）：
  1. **`engine/render/mesh_renderer.{hpp,cpp}`**：
     - `UploadMesh` 增**按网格标记** `bool depthBiased = false`（放在既有形参之后 ⇒ **保留默认值**，既有调用方不受影响）；
       标记记入私有 `MeshResources::depthBiased`；
     - `CreateMainPipeline` 在**同一处**新增一条**带深度偏移的主通道管线变体** `m_pipelineDepthBiased`
       （与 `m_pipeline` 只差 `rasterizer_state` 的 `enable_depth_bias` / `depth_bias_constant_factor` /
       `depth_bias_slope_factor` / `depth_bias_clamp`），**随 MSAA 档位与 `m_pipeline` 同生共死**（仍在
       `CreateMainPipeline` / `EnsureMainPipeline` 里创建，**绝不出现在 `RenderFrame` 热路径**）；析构一并释放；
     - `DrawMeshes` 增形参 `SDL_GPUGraphicsPipeline* depthBiasedPipeline`：主通道传变体、**阴影通道传 `nullptr`**；
       逐网格按其 `depthBiased` 标记**只在标记变化时** `SDL_BindGPUGraphicsPipeline` ⇒ **绑定次数 = 标记切换次数
       ≤ 1**（调用方把地表 tile 全部排在绘制列表最前）⇒ **最多 2 条管线，绝不逐网格绑定**，且**不重排网格**。
  2. **`game/main.cpp`**：`UploadTileMesh` 上传地表 tile 时传 `depthBiased = true`；`UploadVolumeMesh`、角色、
     光球、倒塌整体（`game/rigid_collapse.cpp`）等**一律不传**（默认 `false`）。
  3. **偏移量常量**（`mesh_renderer.cpp` 文件内匿名命名空间）：`kSurfaceDepthBiasConstant = 100.0F`、
     `kSurfaceDepthBiasSlope = 1.0F`、`kSurfaceDepthBiasClamp = 0.0F`。**带注释写明"待实测微调的经验值"**。
- 为什么（**先判真伪 → 再按业界标准做法选方案**）：
  1. **真伪判定 = 确认是缺陷**（契约 + 机制）：
     - **契约**：`docs/adr/0011-layer-transition-volume-takeover.md` 决策第 1 条 ——"只有**四边形的四个角
       全部**落在可挖区域内才跳过该地表四边形"；而 T61 / ADR 0020 把该判据的输入换成**当前常驻集合**
       （`game/main.cpp` 的 `ResidentQuadFilter::SkipQuad`，约 597–615 行）。⇒ 常驻集合的**边界环**上四角不全在
       块内 ⇒ 地表四边形仍画，而体积同时绘制同一层地表；
     - **机制**：`assets/config/dig_regions.toml` 早已登记"区域边界那一圈共面重叠 / 轻微 z-fighting"，原缓解 =
       "把区域铺满整张地图 ⇒ 边界推到世界边缘、玩家看不到"。但**世界自 T58 扩到 1×1 km（17×17 tile）**，
       而该文件仍是**手写的中心 3×3 tile** ⇒ 边界环落到**地图正中**，缓解失效 ⇒ 浮点插值差异让两个共面面
       逐像素随机胜负 ⇒ **静止时也一直闪**。
  2. **业界标准做法（2~3 个点名参照）**：共面两层的标准手段之一 = **光栅化 depth bias / polygon offset** ——
     **D3D12** `D3D12_RASTERIZER_DESC::DepthBias` / `SlopeScaledDepthBias`、**Vulkan**
     `VkPipelineRasterizationStateCreateInfo::depthBiasConstantFactor`、**UE** 渲染 pass 的 `DepthBias`
     光栅化状态；本项目 `engine/render/mesh_renderer.cpp` 亦已为**阴影**声明了该组字段（其 `enable_depth_bias`
     此前为 **0**）。⇒ **第一候选 = 给地表一层正偏移、让体积面稳定胜出**，与 SKILL「业界标准优先」一致。
     **未采用**更彻底的"tile 粒度接管 + 接缝缝合"（改动大、有裂缝风险）——已在 `dig_regions.toml` 与本节
     登记为**后续可选切换项**（另一选项：让可挖范围随常驻窗口推导）。
  3. **偏移量为什么不是 `1e-3~1e-2`**：SDL3_gpu 把 `depth_bias_constant_factor` 原样映射到后端，而后端**再乘
     以该深度格式的最小可分辨差 `r`**（float32 深度 `r ≈ 2^-23`），并且 **D3D12 后端会取整**
     （`SDL_gpu_d3d12.c` 的 `SDL_lroundf`）⇒ 形如 `1e-3` 的浮点量级在**本机 D3D12 后端会被取整成 0、完全失效**。
     故取**整数值 `100`**（等效归一化深度偏移 ≈ `100 × 2^-23 ≈ 1.2e-5`），配合 slope factor 覆盖掠射角。
  4. **阴影通道不加偏移**：阴影通道用不到该变体（`DrawMeshes` 阴影调用传 `nullptr`）⇒ 保持阴影现状。
     另注：既有 `lighting.toml` 的 `depth_bias = 0.0015` 是**片元内的阴影比较偏移**（`mesh.frag` 的
     `shadowParams.z`），**不是**光栅化深度偏移；本次引入的是本仓**第一处**光栅化 depth bias。
- 验证（命令 + 真实结果）：
  1. **构建**（VS DevShell + `VCPKG_ROOT` 后 `cmake --build --preset debug --clean-first`，全量 102 步）：
     **零警告 / 零 error**（`/W4 /WX`），`voxel_game.exe` 与 `voxel_tests.exe` 均正常链接。
  2. **测试**：`ctest --preset debug` → **355/355 passed**；门禁 `check-banned-identifiers.ps1` →
     `scanned 126 file(s), 0 violation(s)`、`PASS`。
  3. **冒烟**（`Start-Process` 重定向 stdout / stderr 到 `build/debug/t78_smoke.*.log`，运行 ≈40 s 后停止）：
     **stderr 无 `ERROR`**（仅既有帧尖峰 WARN 与 T75 容量兜底 WARN、T51 悬空清除 WARN）；启动正常
     （`地表世界就绪…tile 289`、`可挖体积就绪：441 个块…128 块存在等值面`）、
     `GPU 纹理显存记账 … = 合计 262.42 MB`（≤ 300）——**带偏移的主通道管线创建成功**（否则会以 ERROR 抛出）。
  4. **未覆盖（人工目视验收，必须真人在场）**：
     - **看哪里**：飞离中心平台一段距离（越过常驻窗口可达范围）再飞回来，静止注视**中心大方形平台边界那一圈**地面；
     - **期望什么**：该圈地面的来回闪烁**消失**（体积面稳定压过地表面，无 z-fighting 条纹）；
     - **同时确认**：地表相对体积**没有可见下沉 / 台阶**（偏移过大的反例）；若仍闪 ⇒ 上调 `kSurfaceDepthBiasConstant`
       （100 → 300 / 1000），若见台阶 ⇒ 下调，属**待实测微调**的经验值。
- 下一步 / 遗留：
  1. **自动化无法可靠送输入进游戏窗口**（沿用 T72 结论）⇒ "边界不再闪烁"只能**人工目视**，本条目已写明看哪里 / 期望什么。
  2. **回归测试无法自动化**（z-fighting 是 GPU 光栅化行为，无纯函数可钉）⇒ 已按 SKILL「缺陷报告」写明人工验收步骤；
     可自动化的部分是"编译 / 现有测试 / 门禁 / 冒烟无 ERROR"，均已完成。
  3. **后续可选切换项（登记，不启动）**：**tile 粒度接管 + 接缝缝合**、或**让可挖范围随常驻窗口推导**（ADR 0020 方向）
     ——见 `assets/config/dig_regions.toml` 头部注释。
  4. **待实测微调**：`kSurfaceDepthBiasConstant` / `kSurfaceDepthBiasSlope` 的量级（本机 D3D12 生效值受取整影响）。
  5. **本批（T57 … T78）尚未提交**。

---

## 2026-09-30  T79 + T80 + T81：六项小优化 / **窗口预取** / **enkits 建块下沉 worker**（T79 重做并闭环）

- 做了什么（三件事，均为 `docs/plans/v0.3.md` 里"已获批、未开工"的条目）：

  1. **T79 六项"高收益 / 低风险 / 小改动"**（**本轮串行重做** —— 上一轮曾用并行子任务实现其中三项、因并发构建出现大批假失败而整体回退，见本文件 T78 条目）：
     - ① **列查询改 O(1) tile 定位**：`world/terrain/terrain_world.{hpp,cpp}` 新增匿名命名空间辅助 `TileIndexOfColumn`（**向下取整**，负数正确）与
       `ColumnTileCandidates`（**至多 4 个候选 tile**，顺序 = `TileCoord` 字典序 ⇒ 与旧 `std::map` 遍历序**逐位一致**）；
       `ReadColumnHeight` / `WriteColumnHeight` 由"遍历全部已加载 tile"改为只遍历候选集，**共享边界列仍写入全部持有者**（ADR 0008 的边界契约不变）。
     - ② **帧尖峰打点改"帧首采样 + 帧末汇总"**：原先 `clock.Tick()` 在**逻辑相位内**推进 ⇒ `frameMs` 覆盖了"上帧尾 + 本帧头"却**跳过本帧逻辑**，
       于是出现"逻辑 116 ms / 未计时 108 ms 交替"的**假残差**；改为**帧首一次采样**（`frameTimer`）+ 汇总日志移到**帧末**，
       并把分段扩为「输入 / 逻辑 / UI / 剔除 / 重定基 / 动态上传 / uniform / 渲染提交 / 限帧 + 未计时 + 等交换链」。
     - ③ **`OptimizeBroadPhase()`**：先核实**Jolt 不会自动优化**（建树工作原本摊到随后若干帧的 `Update` 里、形成"走动前几帧变慢"），
       故在静态体全部就位后**显式调用一次**（`engine/physics/physics_world.*` 暴露 `OptimizeBroadPhase`）。
     - ④ **挖除路径提速 + scratch 复用**：`world/dig/dig_volume.cpp` 的 `RasterizeBall` 由"**遍历全部常驻块**"改为
       "**由球心 ± 半径推块索引范围**"（`BlockIndexOf` 三重大循环 + `m_blocks.find`）；`CarveByDamage` 的 5 个缓冲
       （cells / `BlastMask` / slotOf / solidOf / blockerOf / floodStack）改走**成员 `CarveScratch`**（热路径零堆分配，红线 10）。
     - ⑤ **`VolumeSlotTable` 由 `std::map` 改为"升序 `std::vector` + `unordered_map` 索引"**（`game/main.cpp`）：保留"**迭代序确定**"（升序），
       `Find` / `Contains` / `Insert` / `Erase` 由树查找变成哈希 + 尾部追加/交换删除；`ResidentQuadFilter::SkipQuad` 改走 `Contains`。
     - ⑥ **MSAA 档位切换的管线重建移出 `RenderFrame`**：`engine/render/mesh_renderer.cpp` 的 `SetMsaaSampleCount` 内**预建**主管线
       （含 T78 的深度偏移变体与天空管线），`RenderFrame` 里的 `EnsureMainPipeline` 退化为**幂等保险**。

  2. **T80 窗口预取**（所有者裁定分叉 **(a)（只扩常驻半径、可挖窗口 `K` 不变）**，理由：分叉 (b) 会把"能挖范围"扩大 1 tile = 64 m，
     属**玩法口径变更**，超出本阶段"不动世界层语义"的边界）：
     `world/streaming/dig_volume_residency.{hpp,cpp}` 新增 `inline constexpr int kResidencyPrefetchTiles = 1` 与
     `ResidencyWindowForPlayerBlocks(worldX, worldZ, radiusTiles, prefetchTiles)`；`DigVolumeScheduler::Update` 里
     **常驻窗口 = 活动窗口 + 预取环**（成员 `m_residencyWindow`，用它算 `DesiredCount` 与 `PlanDigVolumeResidency`），
     而 `Window()`（"**能挖**"的口径）与 `K` **不变**；构造函数第三参默认 `0` ⇒ **旧行为逐位不变**，游戏层显式传预取值。

  3. **T81 启用 enkits + 建块下沉 worker**（**先落 ADR 再动手**）：
     - **ADR 0022**（`docs/adr/0022-volume-build-worker-pipeline.md`）：决策 = **快照式纯函数 + enkits worker 池**；
       硬约束 = **逐位一致**（split-and-merge 不允许改变结果）；给出**并发契约表**（谁能碰什么）、失败降级（池不可用 ⇒ 回落同步 + `WARN`）、
       四个备选方案与切换条件。
     - **enkits 首次真正启用**：新增 `engine/core/task_scheduler.{hpp,cpp}`（`ParallelTask` + `TaskScheduler`；**enkiTS 头不出现在公共接口**，
       用 pimpl 隔离）；`vcpkg.json` 的 `enkits` 依赖、`engine/CMakeLists.txt` 的 `enkiTS::enkiTS` 正式开始使用，`NOTICE.md` 同步标注。
     - **`world/streaming/volume_build_pipeline.{hpp,cpp}`**：`Submit(BlockBuildInput)` 提交、完成队列 + 统计、`TakeCompleted`；
       worker lambda 只做 `BuildBlockFromInput`（纯函数）；**主线程独占在飞作业表**。
     - **`world/dig/dig_volume.{hpp,cpp}`**：新增 `BlockBuildInput`（**35² 高度补丁 + 34³ 壳层采样**；内部 33³ 留给 worker）、
       `BuildBlockFromInput`（**与同步路径共用** `TerrainDerivedDensityFromHeight` / `BuildVolumeMesh` / `ClassifyFill`）、
       `SetBuildPipeline` / `CaptureBlockBuildInput` / `InstallBuiltBlock` / `PollBlockBuildsAndInstall` / `PendingBuildCount`；
       `CreateBlock` 在**装了池时只提交**（返回 `false` = 尚未就位）。
     - **`game/main.cpp`**：`vx::VolumeBuildPipeline`（**声明在 `digVolumes` 之前** ⇒ 后建先毁，避免 worker 读已释放的快照）、
       `SetBuildPipeline`、每帧 `PollBlockBuildsAndInstall(…, kVolumeBuildsInstalledPerFrame = 4)`，
       **GPU 上传仍在渲染线程**（SDL_gpu 命令缓冲单线程，符合 `references/concurrency.md`）。

- 为什么：
  1. **T79 的六项都是 T77 扫描清单里"高收益 / 低风险 / 小改动"项**，且**先判真伪再动手**：②是**打点自身错位**（观测工具坏了 ⇒ 一切"谁贵"的判断都不可信）、
     ③先核实"Jolt 是否已自动优化"（答：不会）、⑤是 `std::map` 在热路径上的树查找与缓存不友好。
  2. **T80 是"预取必须提前"这条硬规则的直接落地**（SKILL 第四节第 2 条）：*load radius > active radius* 是**业界标准做法**
     （参照：**Minecraft 的 chunk load distance**、**UE5 World Partition 的 runtime grid 预取环**、**Unity 的 streaming
     `mipmapBias`/预取环**），本项目此前是"load = active"。**关键取舍**：只扩**常驻**半径 ⇒ 内存上界变大、但**玩法语义（能挖范围）不变**，
     故本阶段（ⓒ 不改世界层语义）只此一条可行。
  3. **T81 是 T72 定位出的治本项**（每次窗口翻转要建 63 块、单块 ≈22 ms + 一次阻塞上传，全在**逻辑相位**）。
     业界同类做法：**Sodium chunk builder**（worker 独占一块、主线程只收包 + 上传）、**UE5 Task Graph**（`FNonAbandonableTask` + 完成回调）、
     **Unity Job System + `MeshDataArray`**（`Allocator.TempJob` 快照 + 主线程 `Mesh.ApplyAndDisposeWritableMeshData`）。
     三者共同点 = **"重活下沉、主线程只做提交与上传、结果晚 1~N 帧可见"**；这也是本项目的取向（SKILL「所有重活都必须离开渲染帧」的
     **②下沉 worker**，而非③按帧切分）。
  4. **为什么坚持"逐位一致"**：红线 7（确定性）与红线 9（单写者 + 不可变快照）要求"谁构建"不得改变结果；否则同一块在同步 / 异步两条路径上
     会产生**不同的存档与不同的碰撞**。做法 = **壳层（34³−33³ = 3367 个采样）在采快照时就取好**（邻块状态在这一刻定格 = **不可变快照**），
     内部 33³ 留给 worker，两条路径共用同一批纯函数。

- 验证（命令 + 真实结果；**注：本轮 `walkback` 冒烟被"真人操作"污染，见下方「发现」第 1 条，其数字只作非受控观测**）：
  1. **构建**：`cmake --build --preset debug --clean-first` → **104/104，零警告**（`/W4 /WX`）；随后 `cmake --build --preset debug` → `ninja: no work to do.`
     （证明工作树与产物**完全同步**，上文"零警告"覆盖当前代码）。
  2. **测试**：`ctest --preset debug -j 6` → **360/360 passed**（355 → 360，新增 5 项：
     `TerrainQuery.ColumnLookupIsDirectAndCoversSharedBoundaryColumns`、`TerrainQuery.ColumnLookupFloorsNegativeColumns`、
     `DigVolumeResidency.ResidencyWindowIsASupersetOfTheActivityWindow`、`DigVolumeResidency.PrefetchRemovesTheCreateBurstWhenCrossingATileBoundary`、
     `DigVolumeWorker.PipelineBuildMatchesMainThreadBuildBitForBit`）；**门禁** → `scanned 130 file(s), 0 violation(s)` / `PASS`（126 → 130）。
  3. **启动期证据**（`build/perf/input_walkback.out.log`，逐行原文）：
     - `[0.323] MSAA 管线已按 2× 预建（T79⑥：创建移出 RenderFrame，主通道 + 深度偏移变体 + 天空）`
     - `[4.342] 可挖体积常驻窗口（ADR 0020 决策二 + T80 预取）：玩家 tile (0, 0)：**活动窗口** ± 2（能挖三维洞）、**常驻窗口** ± 3（含预取环宽 1 格块）；启动常驻 441 块（区域表共 441 块）`
     - `[5.838] 可挖体积块构建（T81 / ADR 0022）：**下沉 worker**（FillBlockDensity + MeshBlock 不再占用渲染帧）（worker 线程 11 个；主线程只做「采快照 + 收包 + 上传」）`
     - `[14.216] 物理宽相位已优化（T79③ / Jolt OptimizeBroadPhase）：静态体共 396 个（地表高度场 280 + 可挖体积三角网 112 + 围墙 4），耗时 1.22 ms；此后 Update 的建树工作不再摊到随后若干帧`
  4. **打点是否可信（T79② 的判据）**：同一档 16 条帧尖峰（16.2~79.8 s）中「**未计时**」= **0.05~0.12 ms**（例：`帧尖峰 115.7 ms：输入 0.15 + 逻辑 113.89 + … + 限帧 0.00 = 115.59，**未计时 0.10** ms`）；
     即**每一帧的成本都被分段解释了**。对照：`build/perf/input_walk.err.log` 同一"未计时"口径下曾出现 **42.38 ms**（`帧尖峰 48.3 ms … = 5.88，未计时 42.38 ms`）
     —— **该 42.38 ms 至今未能归因**（SKILL「观测先于结论」⇒ 登记为遗留，不作结论）。
  5. **T79④ 的"改前 / 改后"对照（同一邻域规模 ⇒ 可比）**：改前 `build/perf/B_radius4.out.log`（T71 的 B 档，`explosion_radius = 4.0`）
      `中心 (2.4, 120.0, 2.8) … **邻域 17457 采样** ⇒ 挖除 14.20 ms`（另一发 13.92 ms）；改后本轮 `邻域 17457 采样` 的 8 发为
      **9.07 / 9.47 / 9.57 / 9.82 / 9.93 / 10.01 / 10.05 / 10.84 ms** ⇒ **≈ −29%**。**但任务书里的"≤ 8 ms"未达成**（仍高约 2 ms），如实登记为遗留。
      （数据取自 `input_walkback` 这一**非受控**运行：单发计时与"谁按的鼠标"无关，且已按**同邻域规模 17457 采样**配对 ⇒ 两者可比。）
  6. **T81 的确定性证据（自动）**：`DigVolumeWorker.PipelineBuildMatchesMainThreadBuildBitForBit` 用**两个独立世界**（同种子同编辑）比较
     "主线程构建"与"worker 构建"的同一块：**密度逐字节相同、`fill` 相同、顶点与索引逐值相同**，覆盖"邻块存在 / 不存在"两条壳层路径，
     并断言 `HasWorkers()`、`CreateBlock` 异步返回 `false`、`PendingBuildCount()` 1 → 0、安装后清账。
  7. **未受控观测（务必不要当结论用）**：本轮真人操作的那段里出现
     `[WARN] 可挖体积块 (0, 3, 0) 网格超出上传时的容量（2358 顶点 / 13506 索引）⇒ 重建 GPU 缓冲并按 2× 预留（T75 兜底路径…）`
     —— 这是**T76 兜底路径的首次运行时观测**（容量守卫按设计触发、`WARN` 不静默）；但**期望是"不触发"**（`T76` 的待验证项），
     故只作记录、不判优劣（同一场景下真人**连续速射 8 发**，多个大邻域塌落（144900 / 239400 采样）叠加，属极端输入）。

- 发现（本轮新增的两条认知）：
  1. **`walkback` 冒烟被"真人操作"污染**：脚本在**游戏内 t = 0~26 s 尚未送任何输入**，但日志里 t = 16.17 s 已有**爆炸**、t = 16.52 s 已有 **`F1` 面板切换**
     ⇒ 键鼠来自**在场的真人**（沿用 T64 的判定口径：这类日志不能当自动化结果）。**后果**：任何"走动 / 开炮"档的性能数字本轮都**不可比**。
  2. **常驻窗口在本图几乎不可能被"走"出来**：可挖区只覆盖世界中心的 **3×3 tile**（块 x/z ∈ [−2, 4] ⇒ 区域 **tile x/z ∈ [−1, 2]**，`assets/config/dig_regions.toml`），
     而 T80 后常驻窗口 = **±3 tile** ⇒ **玩家 tile ∈ [−1, 2] 时区域表被完全覆盖（常驻恒为 441 块、零卸载零建块）**。
     只有越过到 **tile ≥ 3（世界列 ≥ 192）或 tile ≤ −2（世界列 < −64）**，窗口才开始把区域边缘的 63 块排掉；再走回来才会重建。
     这解释了为什么 80 s 的走动 / 飞行日志里**一条"常驻集合已随窗口调整完毕"都没有**（也就看不到 worker 建块）——
     本轮那条 `walkback` 的最远点是 **x ≈ −59（tile −1，差 6 格没出界）**，这是 T81 运行时取证至今拿不到的直接原因。
- 下一步 / 遗留：
  1. **T81 运行时取证（待人工）**：起飞后**沿 −X（或 +X）持续前进到世界列 x < −64 或 x ≥ 192 再返程**（距出生点仅 ≈ 64~192 格），
     看日志 `可挖体积常驻集合已随窗口调整完毕（ADR 0020 窗口 + T81 worker 构建）… **worker 已构建 N 块、单块计算峰值 x.xx ms**` 的 **N 是否随往返增长**
     （`N = 0` ⇒ 走的是同步回落路径，须回报）。T80 的"成串尖峰是否消失"、T78 的"边界是否还闪"可在**同一次往返**里一起看。
  2. **技术债**：单发"挖除" 9.5~10.8 ms（目标 ≤ 8 ms）；`input_walk.err.log` 里那条 **未计时 42.38 ms** 未归因；
     T77 清单里 **T74（加载期首帧冻结的改造）** 仍未开工。
  3. **并发改动未跑 TSan**：`references/concurrency.md` §6 要求"新增或修改并发逻辑后必须在 TSan 配置下跑一遍"；
     本机为 Windows / MSVC（**MSVC 不支持 TSan**）⇒ 本轮**未跑**（已登记在 [ADR 0022](adr/0022-volume-build-worker-pipeline.md) 的「后果」里）。
     切换条件 = Linux / Clang 的 CI 作业或本机再跑一次 `--preset tsan`；**在此之前，T81 的并发正确性只由"逐位一致单测 + 只读快照 + 单一互斥量只护完成队列"的结构性保证支撑**。
  4. **本批（T57 … T81）尚未提交**。

---

## 2026-09-30  T82 落地：**地表 tile 上传按"满地表上界"预留** —— 跨界往返的兜底重建 82 → 0 次

- 做了什么（所有者 2026-09-30 选定；**只改上传容量口径，不动网格化 / 接管判据**）：
  1. `world/terrain/terrain_types.hpp` 新增 **`kTerrainTileIndexCount`**（`64×64×6 = 24576`，"满地表"索引上界），
     并写明两条事实：**顶点数是常数**（`65×65`，与四边形过滤无关）、**只有索引数随层间接管升降**（ADR 0011）。
  2. `game/main.cpp` 的 `UploadTileMesh`：**只要本次网格是"部分地表"（`indices < 上界`）就按上界建缓冲**
     （`reserveIndexCount = 上界`）；满地表时 `reserve = 0`（等价，容量恰为上界）。原**兜底重建分支保留为安全网**
     （T82 之后应当不可达）；WARN 文案回到与实际一致（不再声称"按 2× 预留"）。
  3. `engine/render/mesh_renderer.hpp`：把 `reserveVertexCount` / `reserveIndexCount` 的**口径写进接口注释** ——
     它是**总容量**（实现 `max(本次数量, reserve)`），**不是"额外预留"**。
- 为什么（**含我自己踩的两次坑，价值最高**）：
  1. 现象（T81 运行时取证发现）：跨界往返时同一 tile 在 **11 ms 内连打 2~4 次** `地表 tile … 超出上传时的容量`，
     单档合计 **82 次**；每次都走"`ReleaseMesh` + `UploadMesh`"⇒ 白建 2 个 GPU 缓冲。
  2. **第一版修错了**：照抄体积块路径的字面意思，以为"传 `size` 就是 2× 预留" ⇒ 实测 **82 → 81**（几乎没变）。
     读实现才发现 `capacity = max(size, reserve)` ⇒ **传 `size` 等于不预留**。（顺带证明体积块那句"按 2× 预留"**是对的**，
     因为它传的是 `size * 2`。）**教训**：改容量语义前必须先读实现，别照抄调用点的注释。
  3. **第二版仍不够**：改成 `size * 2` 后 **82 → 16** —— 重复消失（每 tile 恰好 1 次），但**每个 tile 的首轮仍在**：
     "部分地表"可能只有满地表的 1/8，翻倍补不上 8 倍的差。
  4. **第三版（正解）**：tile 网格有**硬上界**（= 满地表）⇒ 直接按上界预留 ⇒ **0 次**。代价有**上界**：
     能成为"部分地表"的 tile 只可能在可挖区（区域 = 中心 4×4 tile）内 ⇒ **至多 16 个 × ~98 KB ≈ 1.6 MB**
     （未记账的几何显存）。**不给全部 289 tile 预留**：那会加约 28 MB（ADR 0008 的 300 MB 预算当前已用 262.42 MB）。
- 验证（命令 + 真实结果）：
  1. **构建**（改过头文件 ⇒ 按仓库教训跑 `--clean-first`）：**零警告**（`/W4 /WX`）；后续增量 `ninja: no work to do`。
  2. **测试**：`ctest --preset debug -j 6` → **360/360 passed**；门禁 → `scanned 130 file(s), 0 violation(s)` / `PASS`。
  3. **同一 `flybound` 档（无人操作，三次对照）**：
     - 改造前：`errLines=86`（**82** 条容量 WARN + 4 条帧尖峰）——`build/perf/input_flybound_t81before.err.log`
     - 第一版（无效）：`errLines=89`（81 条 WARN）——证明"看起来对"的改动**必须实测**
     - 第二版：`errLines=35`（**16** 条 WARN，每 tile 恰 1 次、返程 0）
     - **第三版（最终）：`errLines=8`（8 条帧尖峰 + **0** 条容量 WARN + 0 ERROR）**；日志里**没有任何** `兜底路径` 行。
  4. **帧时间未劣化**：最终档 `hitches=8 hitchMaxMs=37.7 logicMaxMs=35.95 unaccMaxMs=0.09`；
     同档三次实测为 4/37.2、7/49.9、8/37.7 ms ⇒ 该档尾部**本身有运行间波动**，且这些尖峰帧**无容量 WARN 伴随**
     ⇒ 与 T82 **无关**（已并入「技术债 · 尖峰待定位」）。
- 下一步 / 遗留：
  1. **T82 闭环**：`plans/v0.3.md` 的「候选改进（未获批）」条目已改写为「已实施（T82）」。
  2. **待定位（沿用）**：`flybound` 尾部 4~8 条 33~50 ms 尖峰（逻辑 31~43 ms，候选 = 主线程"创建 + 4 块安装 + 接管重网格 + 碰撞体"收口，未验证）。
  3. **技术债不变**：单发"挖除"9.5~10.8 ms（目标 ≤ 8）；T79①③⑤ 无改前基线；T81 未跑 TSan。
  4. **本批（T57 … T82）在写入本条时尚未提交**（随后由本次提交收录）。

---

## 2026-09-30  ★ T81 **运行时取证成功**；找到"自动化送输入不可靠"的真因（F1 面板抑制 + stdout 块缓冲丢尾）

- 做了什么（**只改冒烟脚本 `build/vx_perf_input.ps1`，未动任何游戏/引擎代码**）：
  1. 新增 `flybound` 档（**确定性跨界**）：按 F1（收起面板）→ F（飞行）→ Space 4 s（升空）→ **S 20 s**（飞出可挖区）→ 静置 8 s →
     **W 10 s / 静置 6 s / W 10 s / 静置 18 s**（分两段返航，保证必然重新进入被覆盖的 tile）。
  2. 新增**抢前台守卫** `Ensure-GameForeground`（`ShowWindow(SW_RESTORE)` + `BringWindowToTop` + `SetForegroundWindow`，
     并用 `GetForegroundWindow()` **校验**，最多重试 20 次）+ 三次打印 `focus(...)=True/False`。
  3. 收尾由 `Stop-Process` 改为 **`PostMessage(WM_CLOSE)` 优雅退出**（失败才强杀）——stdout 被重定向到**管道**时是**块缓冲**，
     强杀会**丢掉尾部日志**（这正是此前冒烟"日志停在 t≈16 s、看不到 worker 行"的原因）。
- 为什么（**两条真因，价值高于本次任务本身**）：
  1. **默认显示的 `F1` 调试面板会抑制玩法键盘输入** ⇒ 这不是"自动化送输入不可靠"，而是**输入被游戏按设计吞掉了**：
     `game/main.cpp` 的 `suppression.keyboardGameplay`（T15）为真时 F / W / S / Space 全被抑制，**只有 F1 本身不受抑制**
     ⇒ 脚本必须先按一次 F1 收起面板。（此前 T72 / T75 把这一现象登记为"自动化送输入不可靠 ⇒ 只能人工验收"，**该结论是错的**，
     本条更正；同时解释了一个此前无法解释的观察：被人碰过的那些日志里"输入是正常的"——因为真人第一件事就是按 F1 关面板。）
  2. **stdout 管道 = 块缓冲** ⇒ 强杀丢尾。前几轮"走不到 tile 边界"的判断有一半是被这条掩盖的（实际轨迹确实越了界，
     但收尾行被丢了；不过**只有当窗口真正跨界时才会打印收尾行**，这一条仍然成立）。
  3. 另注：`build/*.ps1` **必须保持纯 ASCII**（本仓库既有规则，见学习笔记 Q8）——本轮在脚本里加了中文注释，PowerShell 5.1
     按 ANSI 解码后产生了**语法错误**与"正则匹配不到中文"的怪现象。
- 验证（`build/perf/input_flybound.out.log` / `.err.log`，逐行原文；**本次全程无人操作**，`focus(...)=True` 三次）：
  1. **去程（卸载）**：`常驻集合已随窗口调整完毕（ADR 0020 窗口 + T81 worker 构建）` 依次打印
     `玩家 tile (-1,-2) ⇒ 378 块` → `(-2,-2) ⇒ 324` → `(-2,-3) ⇒ 216` → `(-3,-3) ⇒ 144` → `(-3,-4) ⇒ 72` → `(-4,-4) ⇒ 36` → `(-4,-5) ⇒ **0 块**`（**全部卸载、worker 已构建 0 块** —— 卸载确实不建块）。
  2. **回程（★ T81 的关键证据）**：`(-4,-4) ⇒ 36 块，**worker 已构建 36 块、单块计算峰值 11.06 ms**` →
     `(-3,-4) ⇒ 72 块，**worker 已构建 72 块、峰值 12.13 ms**` → `(-3,-3) ⇒ 144 块，**144 块 / 12.13 ms**` →
     `(-2,-2) ⇒ 324 块，**324 块 / 13.14 ms**` → `(-1,-1) ⇒ **441 块，worker 已构建 441 块、单块计算峰值 13.14 ms**、worker 11 个`。
     ⇒ **全部 441 块由 worker 池建成**（`N = 0` 的同步回落路径**未**被走到），单块计算峰值 **≈13 ms**，
     与 T72 记录的"单块 ≈22 ms"相比还降了（快照式纯函数 + 壳层采样预取）。
  3. **帧时间（同一档，12 次真实跨界、最大一次重建 441 块）**：`hitches=4 hitchMaxMs=37.2 logicMaxMs=35.5 unaccMaxMs=0.09`；
     4 条尖峰逐条为 `33.7 / 33.9 / 36.9 / 37.2 ms`，全部 `主要受限在 CPU 侧`（逻辑 31.79 / 32.05 / 35.01 / 35.50），
     **未计时 0.06~0.09 ms**、**无 > 50 ms 单帧**。**对照 T72（T80/T81 之前）**：一次 **63 块**的窗口翻转就有 **7~10 条 33~175 ms**（逻辑峰值 62~168 ms）
     ⇒ 本次重建量是它的 **7 倍**（441 vs 63），尖峰却收敛到 **≤ 37 ms**（T80 预取 + T81 下沉 worker 的合计效果）。
  4. **stderr 无 ERROR**（86 行 = 4 条帧尖峰 + 82 条下述兜底 WARN）。
- 发现（**新登记，未判真伪、本轮不改**）：
  1. **地表 tile 的"超出容量"兜底被反复触发 82 次**：`[WARN] 地表 tile (x,z) 网格超出上传时的容量 ⇒ 重建 GPU 缓冲（T75 兜底路径；此后该 tile 回到快路径）`，
     同一 tile 在 **11 ms 内连打 2~4 次**（如 `(-1,2)` 在 36.550 / 36.561 / 36.608 / 36.617 各一次），集中在去程窗口收缩的那几秒。
     **机制（已读到代码）**：`game/main.cpp` 的 `UploadTileMesh` 在兜底重建时 `UploadMesh(..., reserveVertexCount = 0, reserveIndexCount = 0, …)`
     —— **预留为 0**，而**可挖体积块那条路径预留了 2×**（`UploadVolumeMesh`）⇒ tile 每次"接管翻转后网格变大"都会再次超出、再次重建，
     而日志承诺的"此后该 tile 回到快路径"只对"不再增长"成立。**判定**：这**不是**本轮 T79~T81 的回归（T78 之前已在 `walk` 档登记过同类成串 WARN），
     属**已登记的兜底路径按设计触发**；但按同样口径给 tile 加预留（一行改动）能消掉重复重建，**登记为候选，须先获批**。
- 下一步 / 遗留：
  1. **T81 由"待人工"改为"已闭环"**（自动化的确定性取证已完成）；**T80** 亦首次拿到真实跨界的实测（见上第 3 条）。
     仍属**人工目视**的只有 **T78**（区域边界那一圈是否还闪）。
  2. **候选改进（待批）**：地表 tile 兜底重建加预留（消掉 82 次重复 WARN / 重复建缓冲）。
  3. 技术债与 TSan 缺口的登记不变（见上一条 T81 条目的「下一步 / 遗留」第 2、3 条）。
  4. **本批（T57 … T81）尚未提交**。


