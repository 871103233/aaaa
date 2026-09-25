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
