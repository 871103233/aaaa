# 构建、测试与 CI

改动构建配置 / 新增依赖 / 编写测试时，**本文件全部适用**。

## 1. 构建工具链

- **CMake ≥ 3.21 + CMakePresets.json + vcpkg（manifest 模式）+ Ninja**。
- 一键构建：`cmake --preset debug` → `cmake --build --preset debug`。
- Presets 覆盖 `Debug` / `Release` / `RelWithDebInfo` / ASAN / TSan。
- 编译参数一律进 `CMakePresets.json`；不在聊天记录或手工 IDE 配置中留存。
- 依赖策略：需要预编译的库（SDL3 / Jolt / Assimp / EnTT / enkiTS）走 vcpkg manifest；
  header-only 库（stb、FastNoiseLite、zstd amalgamation）可直接 vendoring 到 `third_party/`。

## 2. 编译器与警告

- Windows 主力 **MSVC 2022**；Clang 作 CI 第二编译器；GCC 用于 Linux 验证。
- **MSVC `/W4` 且警告视为错误**。
- 警告豁免须在评审中说明理由并限定到最小范围，不使用全局 `disable`。

## 3. Sanitizer 配置（互斥，必须分开）

| 配置 | 平台 | 内容 |
| --- | --- | --- |
| ASAN / UBSAN | Windows（MSVC）、Linux（Clang） | 越界、UAF、未初始化读、整数溢出 |
| TSan | **仅 Linux / Clang** | 数据竞争、死锁、锁误用 |

- **MSVC 不支持 TSan**。
- **ASAN 与 TSan 互斥**，不可在同一构建中同时开启；CI 需两套独立配置。
- 优先级：ASAN 优于 Valgrind（快 5~10 倍）；Valgrind 仅作辅助。

## 4. 代码风格与静态分析

- `clang-format`：基于 LLVM 风格微调，**4 空格缩进、列宽 120**；提交前自动格式化。
- `clang-tidy` 在 CI 开启，至少启用 `bugprone-*`、`performance-*`、`modernize-*`。

## 5. 单元测试（GoogleTest + CTest）

框架选 **GoogleTest**（与 CTest 集成成熟、社区最大）；在意编译速度可换 Catch2 / doctest。

**以下四类必须有测试（缺一即 DoD 不通过）：**

1. **生成确定性**：同种子 → 逐方块同输出。
2. **Section / Palette 编解码往返**：打包 → 解包 → 数据一致；含全空气 Section 的 `nullptr` 情形。
3. **存档序列化往返 + 版本迁移**：写 → 读 → 逐字段一致；旧版本档能被新版本正确读出。
4. **光照 BFS 跨界 与 贪婪合并边界**：邻块未加载 / 已加载 / 中途卸载；跨 Section 不合并、4 角光照/AO 判据生效。

## 6. 结构性禁止门禁（CI 硬失败）

**文档约定不等于执行。** 凡属**结构性**、可被机械识别的约定，都由 CI 强制，使其不依赖阅读者是否正确处理否定表述。

- **脚本位置**：`scripts/check-banned-identifiers.ps1`（相对本技能根目录，即
  `.trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1`）。
- **规则表是唯一事实来源**：禁用标识符只写在脚本的 `$rules` 里；规范正文只写正向做法，不重复禁令，避免把被否方案反复注入上下文。
- **用法**：

  ```powershell
  # 默认扫描 engine / voxel / game / editor / tests 并自动向上查找仓库根
  pwsh -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1

  # 显式指定仓库根
  pwsh -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1 -RepoRoot .

  # 只跑内置自检，验证规则正则是否符合预期（14 例，不扫描仓库）
  pwsh -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1 -SelfTest
  ```

- **退出码**：`0` = 通过；`1` = 命中违规（CI 据此失败）；`2` = 用法或路径错误。
- **豁免**：确有必要提及规则的场合（单元测试、生成器、迁移工具），在同一行加注释：

  ```cpp
  // vx-allow: <规则名>     豁免该行的一条规则
  // vx-allow: *            豁免该行的全部规则
  ```

- **接入 CI**：在构建步骤**之前**执行，非 0 即终止。
- **扩展方式**：新增结构性规则时，只修改脚本 `$rules` 并补一条 `-SelfTest` 用例；不要在正文里堆叠禁令。

**建议同时保留的机械门禁**（与脚本互补，均由构建保证）：

| 门禁 | 拦截的问题 |
| --- | --- |
| MSVC `/W4` + 警告视为错误 | 编译器级缺陷与可疑写法 |
| CTest 全绿 | 行为回归 |
| clang-format / clang-tidy 检查 | 风格漂移与静态缺陷 |

## 7. CI

- GitHub Actions：每次 PR 在 **Windows + Linux** 跑 Debug(ASAN) + Release 构建，**并执行全部单元测试（CTest）**。
- 执行顺序：**结构性禁止门禁 → 构建 → 单元测试**；任一步失败即终止。
- **TSan 单独一套 Linux 配置**。
- **红灯不得合并**。

## 8. 性能观测

- **Tracy 从 V0.2 起接入**（`ZoneScoped` 埋点），覆盖 CPU 帧时间、线程时间线、锁竞争、内存分配、GPU zone。
- 性能相关改动**必须附 Tracy 对比数据**，不接受"感觉更快了"。
- 排查渲染问题用 RenderDoc 截帧。

## 9. 版本控制

- Git + `.gitignore`（忽略 `build/`、`out/`、二进制产物、用户 IDE 配置）。
- 大资源（PNG / FBX / 音效）走 **Git LFS**。
- 提交规范：模块前缀 `[voxel]`、`[render]`、`[engine]`、`[build]`、`[test]` 等，方便回溯；一个提交只做一件事。
- 涉及参数 / 数据结构变更时，提交信息须说明**迁移方式**。

## 10. 提交前自检

- [ ] 结构性禁止门禁通过（脚本退出码 0）
- [ ] `cmake --preset <cfg>` 一键可构建，未手改 IDE 工程
- [ ] 零警告（`/W4`，警告即错误）
- [ ] 新增 / 修改逻辑已补对应测试，CTest 全绿
- [ ] 并发相关改动已在 TSan 配置下验证
- [ ] 新增依赖已进 `vcpkg.json` 或 `third_party/`，并同步 `CMakeLists.txt`
- [ ] 性能敏感改动附 Tracy 对比数据
