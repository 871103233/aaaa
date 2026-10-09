#pragma once

#include <array>
#include <cstddef>

namespace vx {

/// UI 标签的**唯一**枚举（T16）。
///
/// 两个面板的每一段文字——包括窗口标题、分区标题、控件标签与数值格式串——都必须经
/// [`UiText()`](#UiText) 取值。这样做的目的：**标签语言由"是否加载到 CJK 字体"决定**，
/// 加载到就显示中文，没加载到就整表回退纯 ASCII 英文，于是**任何机器上都不会出现缺字 `?`**。
///
/// 新增标签的流程（缺一项测试即失败，见 `tests/ui_text_test.cpp`）：
///   1. 在本枚举里加一项（放在 `kCount` 之前）；
///   2. 在两个标签表 `kUiLabelsEnglish` / `kUiLabelsChinese` 的**同一位置**各加一项；
///   3. 面板里通过 `UiText(...)` 使用，**不得**直接写字符串字面量。
enum class UiLabel : int {
    // ---- 系统面板（Esc）----
    SystemPanelTitle = 0,  ///< 面板标题
    SectionDisplay,        ///< 分区：显示
    DisplayMode,           ///< 控件：显示模式
    ModeWindowed,          ///< 选项：窗口
    ModeFullscreen,        ///< 选项：全屏
    Resolution,            ///< 控件：分辨率
    SectionAudio,          ///< 分区：声音
    Volume,                ///< 控件：主音量
    VolumeFormat,          ///< 主音量数值格式（`%d / 100`）
    SectionPerformance,    ///< 分区：性能（T17）
    FrameRateCap,          ///< 控件：帧率上限
    FrameRateCapFormat,    ///< 帧率上限数值格式（`%d Hz`）
    SectionQuit,           ///< 分区：退出
    QuitGame,              ///< 按钮：退出游戏

    // ---- 调试面板（F1，只读）----
    DebugPanelTitle,              ///< 面板标题
    SectionTiming,                ///< 分区：时间步
    FrameTime,                    ///< 行标签：帧时间
    FrameTimeFormat,              ///< 帧时间数值格式
    Fps,                          ///< 行标签：FPS
    FpsFormat,                    ///< FPS 数值格式
    FixedSteps,                   ///< 行标签：本帧固定步
    FixedStepsFormat,             ///< 固定步数值格式
    SectionCharacter,             ///< 分区：角色
    Position,                     ///< 行标签：位置
    PositionFormat,               ///< 位置数值格式
    Cell,                         ///< 行标签：所在格
    CellFormat,                   ///< 所在格数值格式
    State,                        ///< 行标签：状态
    StateFormat,                  ///< 状态数值格式
    ValueYes,                     ///< 取值：是
    ValueNo,                      ///< 取值：否
    ValueReady,                   ///< 取值：就绪
    ValueNotReady,                ///< 取值：未就绪
    SectionCameraOrb,             ///< 分区：相机 / 光球（T27 起替代"相机 / 笔刷"）
    CameraOrientation,            ///< 行标签：朝向
    CameraOrientationFormat,      ///< 朝向数值格式
    MouseCapture,                 ///< 行标签：鼠标捕获
    MouseCaptureOn,               ///< 取值：捕获开
    MouseCaptureOff,              ///< 取值：捕获关
    ExplosionRadius,              ///< 行标签：爆炸半径（T27：光球的破坏半径）
    ExplosionRadiusFormat,        ///< 爆炸半径数值格式
    SectionWorldPhysics,          ///< 分区：世界 / 物理
    LoadedTiles,                  ///< 行标签：已加载 tile
    DirtyTiles,                   ///< 行标签：本帧重网格单元数（爆炸造成的）
    TileBodies,                   ///< 行标签：地表碰撞体 tile
    CountFormat,                  ///< 计数数值格式（`%zu`）
    Orbs,                         ///< 行标签：光球（活动 / 上限）
    OrbCountFormat,               ///< 光球计数格式（`%zu / %zu`）
    VolumeBlocks,                 ///< 行标签：可挖体积块（T61：**常驻**集合，随玩家窗口变化）
    VolumeBlocksFormat,           ///< 可挖体积块格式（`%zu（已挖 %zu；待办 %zu、留驻 %zu）`，T61 起含常驻调度）
    VolumeBodies,                 ///< 行标签：体积碰撞体（T28）
    CollapseMoved,                ///< 行标签：累计塌落体素（T29）

    // ---- 调试面板：渲染开销与帧时间分解（T24）----
    SectionRenderCost,            ///< 分区：渲染开销
    DrawCalls,                    ///< 行标签：Draw Call 数
    Triangles,                    ///< 行标签：三角形数
    Vertices,                     ///< 行标签：顶点数
    TextureVram,                  ///< 行标签：纹理显存
    MeshVram,                     ///< 行标签：**网格缓冲显存**（V0.7 H0 起纳入记账；顶点 + 索引 + 骨骼矩阵）
    TextureVramFormat,            ///< 纹理显存数值格式（`%.2f MB`）
    SectionCpuFrameTime,          ///< 分区：CPU 帧时间分解
    CpuLogicStep,                 ///< 行标签：逻辑步（物理 + 相机）
    CpuUiBuild,                   ///< 行标签：UI 构建
    CpuRenderSubmit,              ///< 行标签：渲染提交
    SwapchainWait,                ///< 行标签：等待交换链（T38；混在渲染提交里会掩盖"在空等 GPU"）
    MillisecondsFormat,           ///< 毫秒数值格式（`%.2f ms`）
    SectionGpuPassTime,           ///< 分区：各 pass GPU 时间
    GpuPassTime,                  ///< 行标签：各 pass GPU 时间
    GpuTimeUnavailable,           ///< 取值：GPU 时间不可用（SDL3_gpu 无时间戳查询）

    // ---- 调试面板：测试模式横幅（T85）----
    SectionTestMode,          ///< 分区：测试模式
    TestModeAuto,             ///< 取值：自动测试（勿动键鼠）
    TestModeManual,           ///< 取值：人工测试
    TestModeNoItems,          ///< 取值：未提供人工验收项
    TestModeItemNonAscii,     ///< 取值：该项含非 ASCII 且无 CJK 字体（回退提示）

    // ---- 加载画面（启动加载；见 SKILL「不冻结画面」）----
    LoadingTitle,             ///< 标题：正在生成世界
    LoadingHint,              ///< 提示：画面不会卡住，可继续操作窗口
    LoadingProgressFormat,    ///< 进度百分比格式（`%.0f%%`）
    LoadingStageTextures,     ///< 阶段：材质贴图
    LoadingStageEnvironment,  ///< 阶段：环境贴图（T67 的 HDRI 解码 + IBL 烘焙）
    LoadingStageTerrainTiles, ///< 阶段：地形 tile
    LoadingStageDigVolumes,   ///< 阶段：可挖体积
    LoadingStageCollision,    ///< 阶段：碰撞体
    LoadingStageMeshUpload,   ///< 阶段：网格上传
    LoadingStageFinalize,     ///< 阶段：收尾

    // ---- 常驻 HUD（屏幕左上角坐标显示）----
    HudCoordinates,           ///< HUD 标题：坐标
    HudCoordinatesFormat,     ///< HUD 坐标数值格式（`X %.1f  Y %.1f  Z %.1f`）
    HudCellFormat,            ///< HUD 所在格格式（`cell (%d, %d, %d)`）
    PortalPromptFormat,       ///< HUD 传送门提示格式（V3/V9；`%s` = 门名或目标世界 id；按 `E` **打开菜单**）

    // ---- 传送门交互菜单（V9；所有者 2026-10-06：走近门按 E 出菜单）----
    PortalDefaultName,           ///< 缺省门名（配置未给 `portal_name`）：神秘传送门
    PortalMenuTitle,             ///< 菜单窗口标题：传送门
    PortalMenuTitleFormat,       ///< 菜单标题格式（`%s → %s`；门名 → 秘境名）
    PortalMenuEnter,             ///< 动作：进入
    PortalMenuReset,             ///< 动作：重置秘境（**仅肉鸽秘境**）
    PortalMenuCancel,            ///< 动作：取消
    PortalMenuGenerationFormat,  ///< 秘境已生成次数（`第 %u 次生成`）
    PortalMenuSessionHint,       ///< 说明：已存入存档槽 ⇒ 跨启动仍进入同一个（重置才会换）
    PortalResetDoneFormat,       ///< 重置完成（`已重置秘境 [%s]：新种子 %llu`）

    // ---- 坐标拾取辅助（V0.5 E2；HUD 一行）----
    PlacementHintFormat,         ///< HUD 摆放辅助提示（`摆放（F2）：当前类型「%s」……`；`%s` = 类型 id（纯 ASCII））
    PlacementPickedFormat,       ///< HUD 最近一次摆放 / 删除反馈（`上次摆放：%s`；`%s` 恒为纯 ASCII）
    EditLayerUnsavedFormat,      ///< HUD 未保存改动计数（V0.10 / S9；`未保存改动：%d 处（F5 或面板保存）`）

    // ---- 物件选择器与摆放模式（V0.5 E3；见 ADR 0032）----
    ObjectPaletteTitle,           ///< 面板标题
    ObjectPaletteCategoryHeader,  ///< 一级列标题：仓库（类别）
    ObjectPaletteTypeHeader,      ///< 二级列标题：模型
    ObjectPaletteEnter,           ///< 按钮：进入摆放模式
    ObjectPaletteSave,            ///< 按钮：保存**全部**改动到可编辑层（V0.10 / S9：明示"全部"）
    ObjectPaletteUnsavedFormat,   ///< 面板：未保存改动计数（V0.10 / S9；`未保存改动：%d 处`）
    ObjectPaletteCancel,          ///< 按钮：取消
    PlacementModeHintFormat,      ///< 摆放模式横幅（`%s` = 当前类型 id）
    PlacementSaveFailedFormat,    ///< 保存失败（`%s` = 原因）
    ObjectPalettePreviewHeader,   ///< V0.5 E4 预览区标题
    ObjectPalettePreviewHint,     ///< V0.5 E4 预览区操作提示（自动旋转 + 拖动转向）
    ObjectPalettePreviewEmpty,    ///< V0.5 E4 预览不可用（该类型几何为空）

    // ---- 成套建筑摆放 / 室内变暗调参（V0.9；ADR 0036）----
    PlacementBuildingHintFormat,   ///< 建筑摆放模式横幅（`%s` = 建筑 id（纯 ASCII）、`%s` = 落点模式**显示名**、`%.2f` = 待放变暗值）
    PlacementSelectedHintFormat,   ///< 选中态横幅（`%s` = 被指向的建筑 id、`%.2f` = 该建筑当前的室内变暗值）

    // ---- 落点模式显示名（V0.10 / S5；`T` 循环的 4 个选项）----
    LandingModeSink,      ///< 落点模式 ② 向下半埋
    LandingModeFlatOnly,  ///< 落点模式 ④ 落地必须平整
    LandingModeFlatten,   ///< 落点模式 ① 压平地形
    LandingModeFill,      ///< 落点模式 ③ 填充地形

    // ---- 摆放控制（V0.11；**运行期可改**的开关 + 键位提示，见 SKILL「运行期可修改优先」）----
    SectionPlacementControls,      ///< 分区：摆放控制（运行期可改）
    PlacementRotateHoldFormat,     ///< 旋转长按模式状态（`%s` = 是 / 否）+ 键位提示
    PlacementNeighborSnapFormat,   ///< 邻居优先吸附状态（`%s` = 是 / 否）+ 键位提示
    PlacementGridSnapFormat,       ///< 世界网格吸附状态（`%s` = 是 / 否）+ 键位提示

    // ---- 修改模式（V0.11 / I4 修订；`F2` 面板入口 + 屏幕中央指示器）----
    ObjectPaletteModify,          ///< 按钮：进入**修改模式**（选中已有物件并拖动调整）
    /// V0.11 / A8：按钮「**重复上次**」（`F3` 让位给修改模式后，本能力改由面板按钮触发）。
    ObjectPaletteRepeatLast,
    ModifyModeIndicator,          ///< 指示器：修改模式（未选中）
    ModifySelectedIndicator,      ///< 指示器：修改模式（已选中）

    kCount  ///< 标签总数（必须保持在最后）
};

/// 标签总数（`kUiLabelCount` 同时是两张表的固定长度）。
inline constexpr int kUiLabelCount = static_cast<int>(UiLabel::kCount);

/// 英文标签表：**必须全部为纯 ASCII**，作为"没有 CJK 字体"时的回退文案。
///
/// `tests/ui_text_test.cpp` 会逐项断言本表每个字符串都是纯 ASCII；这也是"面板上不会出现 `?`"
/// 的可执行证据。
inline constexpr std::array<const char*, kUiLabelCount> kUiLabelsEnglish = {
    "System (Esc)",
    "Display",
    "Display mode",
    "Windowed",
    "Fullscreen",
    "Resolution (windowed only)",
    "Audio",
    "Master volume",
    "%d / 100",
    "Performance",
    "Frame rate cap",
    "%d Hz",
    "Quit",
    "Quit Game",
    "V0.1 Debug Panel",
    "Timing",
    "Frame time",
    "%.2f ms (P50 %.2f / P95 %.2f / P99 %.2f ms)",
    "FPS",
    "%.1f",
    "Fixed steps",
    "%d (dt = %.5f s)",
    "Character",
    "Position",
    "(%.2f, %.2f, %.2f) blocks",
    "Cell",
    "(%d, %d, %d)",
    "State",
    "Grounded: %s  Physics: %s  Flying: %s",
    "yes",
    "no",
    "ready",
    "not ready",
    "Camera / Orb",
    "Orientation",
    "yaw %.1f deg  pitch %.1f deg  dist %.2f blocks",
    "Mouse capture",
    "on (relative mode; Esc release / click recapture)",
    "off (cursor visible)",
    "Explosion radius",
    "%.1f blocks",
    "World / Physics",
    "Loaded tiles",
    "Re-meshed (frame)",
    "Terrain colliders",
    "%zu",
    "Orbs",
    "%zu / %zu",
    "Diggable volume blocks",
    "%zu (carved %zu; pending %zu, kept %zu)",
    "Volume colliders",
    "Collapsed voxels (total)",
    "Render Cost",
    "Draw calls",
    "Triangles",
    "Vertices",
    "Texture VRAM",
    "Mesh VRAM",
    "%.2f MB",
    "CPU frame time",
    "Logic (physics + camera)",
    "UI build",
    "Render submit",
    "Wait for swapchain",
    "%.2f ms",
    "GPU passes",
    "Pass GPU time",
    "unavailable (SDL3_gpu has no timestamp queries)",
    "Test mode",
    "AUTOMATED TEST RUNNING - do NOT touch the keyboard or mouse",
    "MANUAL TEST - please verify the items below",
    "(no manual test items; see the launch command)",
    "(item is non-ASCII and no CJK font is loaded; see the launch command or console log)",
    "Generating world",
    "The view keeps updating while the world is generated; the window stays responsive",
    "%.0f%%",
    "Material textures",
    "Environment (HDRI / IBL)",
    "Terrain tiles",
    "Diggable volumes",
    "Collision bodies",
    "Mesh upload",
    "Finalizing",
    "Coordinates",
    "X %.1f  Y %.1f  Z %.1f",
    "cell (%d, %d, %d)",
    "Near '%s' - press E to open menu",
    "Mysterious Portal",
    "Portal",
    "%s -> %s",
    "Enter",
    "Reset realm",
    "Cancel",
    "generation %u",
    "Saved to the save slot: the same realm persists across restarts (reset re-rolls it)",
    "Realm [%s] reset: new seed %llu",
    "Place (F2): type '%s' - press F2 to open the object palette",
    "Last place: %s",
    "Unsaved changes: %d (F5 or the palette Save button writes them)",
    "Object palette (F2)",
    "Warehouse (category)",
    "Model",
    "Enter placement mode",
    "Save all to edit layer",
    "Unsaved changes: %d",
    "Cancel",
    "PLACEMENT MODE: '%s'  |  Q/E rotate  |  LMB place  |  RMB delete  |  F5 save  |  Esc exit",
    "Save failed: %s",
    "Preview",
    "spins automatically - drag (any direction) to rotate; release resets & resumes",
    "no preview (empty geometry)",
    "PLACE BUILDING: '%s'  landing=%s  interior-darkening=%.2f  |  Q/E rotate  |  LMB place  |  RMB delete  |  T landing mode  |  [ ] darkening  |  F5 save  |  Esc exit",
    "SELECTED BUILDING '%s'  interior-darkening=%.2f  ([ ] to adjust, F5 to save)",
    "sink (half-buried)",
    "flat ground only",
    "flatten terrain",
    "fill terrain",
    "Placement (runtime toggles; keys apply live)",
    "Rotate hold-to-turn: %s  [Z]",
    "Snap prefer neighbor: %s  [X]",
    "Snap world grid: %s  [B]",
    "Modify Mode (F3)",
    "Repeat last (type + facing)",
    "Modify mode: click an object to select  (F3/Esc exit; RMB look, MMB pan, wheel zoom, RMB+WASD fly, Delete remove)",
    "Selected: drag the object body or the centre square = free move, arrows = single axis, ring = rotate  (RMB+WASD fly, Delete removes)",
};

/// 中文标签表：仅当**成功加载 CJK 字体**时启用（此时不可能缺字）。
///
/// 纯格式串（如 `%d / 100`）在中英两表中相同，故测试允许"中文项与英文项一致"，
/// 但要求中文表中确实存在非 ASCII 项，防止整列误填成英文。
inline constexpr std::array<const char*, kUiLabelCount> kUiLabelsChinese = {
    "系统面板",
    "显示",
    "显示模式",
    "窗口",
    "全屏",
    "分辨率（仅窗口模式）",
    "声音",
    "主音量",
    "%d / 100",
    "性能",
    "帧率上限",
    "%d Hz",
    "退出",
    "退出游戏",
    "V0.1 调试面板",
    "时间步",
    "帧时间",
    "%.2f ms（P50 %.2f / P95 %.2f / P99 %.2f ms）",
    "FPS",
    "%.1f",
    "本帧固定步",
    "%d（dt = %.5f s）",
    "角色",
    "位置",
    "(%.2f, %.2f, %.2f) 格",
    "所在格",
    "(%d, %d, %d)",
    "状态",
    "着地：%s  物理：%s  飞行：%s",
    "是",
    "否",
    "就绪",
    "未就绪",
    "相机 / 光球",
    "朝向",
    "yaw %.1f°  pitch %.1f°  距离 %.2f 格",
    "鼠标捕获",
    "开（相对模式，Esc 释放 / 点击重捕获）",
    "关（光标可见）",
    "爆炸半径",
    "%.1f 格",
    "世界 / 物理",
    "已加载 tile",
    "本帧重网格单元",
    "地表碰撞体 tile",
    "%zu",
    "光球（活动 / 上限）",
    "%zu / %zu",
    "可挖体积块",
    "%zu（已挖 %zu；待办 %zu、留驻 %zu）",
    "体积碰撞体",
    "累计塌落体素",
    "渲染开销",
    "Draw Call 数",
    "三角形数",
    "顶点数",
    "纹理显存",
    "网格缓冲显存",
    "%.2f MB",
    "CPU 帧时间分解",
    "逻辑步（物理 + 相机）",
    "UI 构建",
    "渲染提交",
    "等交换链（GPU / 呈现）",
    "%.2f ms",
    "各 pass GPU 时间",
    "各 pass GPU 时间",
    "不可用（SDL3_gpu 无时间戳查询）",
    "测试模式",
    "自动测试进行中：请勿操作键盘 / 鼠标",
    "人工测试：请逐项确认以下内容",
    "（未提供人工验收项，请见启动命令）",
    "（该项含非 ASCII 文本且未加载 CJK 字体；请见启动命令或控制台日志）",
    "正在生成世界",
    "生成期间画面持续刷新、窗口保持响应；世界就绪后自动进入",
    "%.0f%%",
    "材质贴图",
    "环境贴图（HDRI / IBL）",
    "地形 tile",
    "可挖体积",
    "碰撞体",
    "网格上传",
    "收尾",
    "坐标",
    "X %.1f  Y %.1f  Z %.1f",
    "格 (%d, %d, %d)",
    "靠近「%s」：按 E 打开菜单",
    "神秘传送门",
    "传送门",
    "%s → %s",
    "进入",
    "重置秘境",
    "取消",
    "第 %u 次生成",
    "已存入存档槽：跨启动仍进入同一个（重置才会换）",
    "已重置秘境 [%s]：新种子 %llu",
    "摆放（F2）：当前类型「%s」—— 按 F2 打开物件选择器",
    "上次摆放：%s",
    "未保存改动：%d 处（F5 或面板「保存全部」写盘）",
    "物件选择器（F2）",
    "仓库（类别）",
    "模型",
    "进入摆放模式",
    "保存全部到可编辑层",
    "未保存改动：%d 处",
    "取消",
    "摆放模式：「%s」  |  Q/E 旋转  |  左键放下  |  右键删除  |  F5 保存  |  Esc 退出",
    "保存失败：%s",
    "预览",
    "自动旋转 · 上下左右拖动可旋转 · 松开归位并恢复自转",
    "无预览（该类型几何为空）",
    "摆放建筑：「%s」  落点=%s  室内变暗=%.2f  |  Q/E 旋转  |  左键放下  |  右键删除  |  T 落点模式  |  [ ] 变暗  |  F5 保存  |  Esc 退出",
    "选中建筑「%s」  室内变暗=%.2f（[ ] 调整，F5 保存）",
    "向下半埋",
    "落地必须平整",
    "压平地形",
    "填充地形",
    "摆放控制（运行期可改；按下面的键切换）",
    "旋转长按模式：%s（按 Z 切换）",
    "邻居优先吸附：%s（按 X 切换）",
    "世界网格吸附：%s（按 B 切换）",
    "修改模式（F3）",
    "重复上次（类型 + 朝向）",
    "修改模式：点击物体选中（F3/Esc 退出；右键环视 · 中键平移 · 滚轮推拉 · 右键+WASD 飞 · Delete 删除）",
    "已选中：拖物体本体 或 中心方块 = 任意方向平移 · 拖箭头 = 单轴 · 拖环 = 旋转（右键+WASD 飞 · Delete 删除）",
};

/// 纯函数：判断字符串是否**只含 ASCII 字节**（`cjkFontAvailable = false` 时的硬约束）。
///
/// 空串视为只含 ASCII。不读全局状态、不分配内存。
[[nodiscard]] inline bool IsAsciiOnly(const char* text) noexcept {
    if (text == nullptr) {
        return true;
    }
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != 0; ++cursor) {
        if (*cursor >= 0x80U) {
            return false;
        }
    }
    return true;
}

/// 标签缝：返回 `label` 在指定语言下的文本。
///
/// `cjk` 为 true（已加载 CJK 字体）返回中文表项，为 false 返回**纯 ASCII** 英文表项。
/// 越界索引返回空串（防御性，正常调用不会发生）。
[[nodiscard]] inline const char* UiText(UiLabel label, bool cjk) noexcept {
    const int index = static_cast<int>(label);
    if (index < 0 || index >= kUiLabelCount) {
        return "";
    }
    const std::size_t offset = static_cast<std::size_t>(index);
    return cjk ? kUiLabelsChinese[offset] : kUiLabelsEnglish[offset];
}

}  // namespace vx
