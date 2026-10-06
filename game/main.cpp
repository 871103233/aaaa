// V0.1 地表世界运行闭环：窗口 + 高度场 tile 生成 / 网格化 + Jolt 角色胶囊物理 +
// 第三人称相机 + 固定步长主循环 + 笔刷挖堆 + ImGui 调试面板。
//
// 分层与依赖方向（SKILL §2）：platform → engine core → world → game。
// 本文件位于最上层 game/，只消费下层接口：
//   - 事件 / 输入只在平台层（`Window::pump_events`）发生，这里只读 `InputMap` 的动作；
//   - 世界坐标用整数与 `double`（tile 坐标 + 1/16 格定点高度 + Jolt 的 `double` 位置），
//     上传 GPU 前做**相机相对偏移**转 float（红线 6）；
//   - 逻辑与物理按固定步长 1/60 s 推进，渲染只用插值系数 alpha，绝不把它写回模拟状态（红线 11）。

#include "character_animation.hpp"
#include "character_facing.hpp"
#include "character_mesh.hpp"
#include "character_model.hpp"
#include "character_movement.hpp"
#include "core/clock.hpp"
#include "core/fixed_step.hpp"
#include "core/frame_limiter.hpp"
#include "core/log.hpp"
#include "debug_overlay.hpp"
#include "destruction_queue.hpp"
#include "gameplay_input.hpp"
#include "generation/level_manifest.hpp"
#include "generation/map_preset.hpp"
#include "generation/terrain_params.hpp"
#include "input/input_map.hpp"
#include "mouse_capture.hpp"
#include "object/object_layer.hpp"
#include "object/object_mesh.hpp"
#include "object/object_scatter.hpp"  // V8：程序化散布（纯函数）
#include "object/object_support.hpp"
#include "orb.hpp"
#include "out_of_bounds.hpp"
#include "physics/physics_world.hpp"
#include "platform/command_line.hpp"
#include "platform/settings.hpp"
#include "platform/window.hpp"
#include "portal_interaction.hpp"
#include "render/camera.hpp"
#include "render/environment.hpp"
#include "render/frustum.hpp"
#include "render/lighting_table.hpp"
#include "render/mesh_renderer.hpp"
#include "render/model_loader.hpp"
#include "render/shadow_cascade.hpp"
#include "render/texture_loader.hpp"
#include "rigid_collapse.hpp"
#include "terrain/material_table.hpp"
#include "terrain/material_textures.hpp"
#include "terrain/terrain_collision.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"
#include "terrain/world_bounds.hpp"
#include "premade/premade_map.hpp"
#include "premade/premade_terrain_source.hpp"
#include "save/world_instance_save.hpp"
#include "shell/surface_shell.hpp"
#include "water/river.hpp"
#include "test_mode.hpp"
#include "ui_text.hpp"
#include "world_fingerprint.hpp"
#include "world_manager.hpp"
#include "dig/collapse_table.hpp"
#include "dig/destruction_table.hpp"
#include "dig/dig_region.hpp"
#include "dig/dig_volume.hpp"
#include "dig/projectile_table.hpp"
#include "dig/terrain_brush.hpp"
#include "dig/volume_collapse.hpp"
#include "dig/volume_collision.hpp"
#include "streaming/dig_volume_residency.hpp"
#include "streaming/terrain_tile_build_pipeline.hpp"
#include "streaming/terrain_tile_residency.hpp"
#include "streaming/volume_build_pipeline.hpp"

#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

/// V5：为一个**新建的秘境实例**取一个种子 —— 这是全流程里**唯一**的非确定性输入；
/// 一旦记入 `WorldManager` 的实例账本，之后的生成 / 流式 / 物理**全部**由它决定（红线 7）。
///
/// 为什么用 `std::random_device`：MSVC 上它是 OS 提供的可信熵源（不是 `rand()`）——  // vx-allow: no-rand
/// SKILL 红线 7 禁止的是"**生成**依赖随机数"，而不是"**种子**由随机数取得"。
[[nodiscard]] std::uint64_t RollInstanceSeed() {
    std::random_device  device;
    const std::uint64_t high = static_cast<std::uint64_t>(device()) << 32U;
    const std::uint64_t low  = static_cast<std::uint64_t>(device());
    const std::uint64_t seed = high ^ low;
    return (seed == 0U) ? 1U : seed;  // 避开 0（合法，但日志里不好认）
}

// ---------------------------------------------------------------
// V10：秘境存档槽（单槽自动；只存**秘境绑定** —— 世界 id + 实例种子 + 已重置次数）
// 口径见 docs/plans/v0.5.md §1.13.1（所有者 2026-10-06：单槽 / 仅绑定 / `%APPDATA%\...\saves\`）。
// ---------------------------------------------------------------

/// 本机**秘境存档槽**路径：`<设置目录>/saves/instances.toml`（与 `settings.toml` 同根）。
///
/// 前置条件：SDL 已初始化（`SystemSettingsPath` 用 `SDL_GetPrefPath`，它保证**根目录**存在）⇒ 只补建 `saves\`。
[[nodiscard]] std::filesystem::path WorldInstanceSavePath() {
    return vx::SystemSettingsPath().parent_path() / "saves" / vx::kWorldInstanceSaveFileName;
}

/// 把 `WorldManager` 的秘境实例账本**快照**成存档内容（按**注册顺序**遍历 ⇒ 文件内容确定，便于比对）。
[[nodiscard]] vx::WorldInstanceSave BuildWorldInstanceSave(const vx::WorldManager& manager) {
    vx::WorldInstanceSave save;
    for (const std::string& id : manager.Order()) {
        if (const vx::WorldInstance* instance = manager.FindInstance(id); instance != nullptr) {
            save.instances.push_back(vx::SavedWorldInstance { id, instance->seed, instance->generation });
        }
    }
    return save;
}

// ---------------------------------------------------------------
// 场景参数（默认加载 3×3 tile 的预设地图 test_range，范围由地图文件决定）
// ---------------------------------------------------------------

constexpr const char* kDefaultMapFile        = "assets/maps/test_range.toml";  ///< 默认预设地图（T11）
constexpr int         kFallbackSpawnHeight   = 64;   ///< 出生列无地表数据时的回退高度（格）

/// 鼠标环绕灵敏度：**弧度 / 像素**。每累积 1 像素的相对位移，yaw / pitch 变化 0.0022 弧度。
///
/// 该数值只在**相对鼠标模式**（T14：光标隐藏、位移改为相对量）下才有意义：此时屏幕边界不再截断位移，
/// 视角可以持续转动。换算看**累计位移**：从正前方（pitch = 0）转到单侧极限 ±89°（≈1.553 rad）
/// 约需 1.553 / 0.0022 ≈ 706 像素；跨过完整 ±89° 区间（≈3.107 rad）约需 1412 像素。
/// 若鼠标未被捕获，光标到屏幕边即止，累计位移到不了 706 像素，±89° 便永远摸不到——
/// 这正是 T14 要修的现象之一。此值经人工实测手感可接受，**不要**为了该现象改动它。
constexpr float kLookSensitivity    = 0.0022F;
constexpr float kWalkSpeed          = 9.0F;                       ///< 行走速度（格/秒）
constexpr float kSprintSpeed        = 20.0F;                      ///< 冲刺速度（格/秒）
constexpr float kFlySpeed           = 28.0F;                      ///< 飞行速度（格/秒）；飞行中按 Shift 加倍
constexpr double kRebaseDistance    = 24.0;                       ///< 渲染原点重定基阈值（格）
constexpr float kCameraFollowDistance = 14.0F;                    ///< 第三人称相机跟随距离（格）

/// **帧尖峰（hitch）打点阈值**（毫秒）：= 2 × 帧预算（60 Hz ⇒ 33 ms）。
///
/// 判据出自 [references/performance-and-hitches.md](../../.trae/skills/voxel-engine-dev-standards/references/performance-and-hitches.md)
/// §0 的"尖峰型"一行；超过阈值即打印一条**可定位**的日志（三相 CPU / draw call / 提交网格数 /
/// 是否在等交换链）—— 这是 SKILL「卡顿消除」第五硬规则"观测先于结论"的落地。
constexpr double kHitchThresholdMs = 33.0;

/// 尖峰日志的最小间隔（毫秒）：持续低帧时避免把日志刷爆（观测本身不能制造新的卡顿）。
constexpr double kHitchLogMinIntervalMs = 200.0;

/// **延后破坏工作的每帧预算**（毫秒，T37）。
///
/// 预算用尽即让出本帧（剩余留到后续帧）；**单个单位超预算时仍至少做一件**，保证进度。
/// 实测单个体积块的"重网格 + 上传"约 5~9 ms、"碰撞体重建"约 9 ms（debug），
/// 故最坏帧会多花约 9 ms —— 仍在 60 Hz 的一帧（16.7 ms）之内，不构成冻结。
constexpr double kDestructionBudgetMs = 3.0;

// ---------------------------------------------------------------
// 角色物理参数（T7 验收口径：20 格/秒冲刺不穿地形；1 格台阶可自动上步）
// ---------------------------------------------------------------

constexpr float kGravity                    = 24.0F;  ///< 重力加速度（格/秒²）
/// T33：倒塌整体的渲染网格池 —— 每个槽位的顶点数上限（4 的倍数）。
/// 单个整体的外表面超过它时**只截断外观**（物理与回写不受影响）并告警；池在加载期建好（见 `RigidCollapseRuntime`）。
constexpr std::size_t kCollapseMeshCapacityVerts = 98304U;

/// T46（[ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md) 决策四）：网格池**槽位数**。
///
/// 为什么是 16 而不是 4：岩石类残骸落定后**保留几何体**（不回写）⇒ 槽位**不再随落定释放**。
/// 4 槽会在第 5 次倒塌时用尽（然后本次不抽出任何整体 = "炸了不塌"）；项目所有者选定"**永久保留 + 扩池**"。
/// 代价 = 每槽 98304 顶点 + 147456 索引 ≈ 3.18 MB ⇒ 16 槽约 **51 MB 显存**（启动日志会打印实测值）。
/// 池满时的兜底：把**最旧**的保留残骸惰性回写腾位（见 `RetireOldestRetained`）。
constexpr std::size_t kCollapseMeshSlots = 16U;
constexpr float kCharacterRadius            = 0.3F;   ///< 胶囊半径（格）
constexpr float kCharacterCylinderHalfHeight = 0.6F;  ///< 胶囊圆柱段半高（格）
constexpr float kCharacterMaxSlopeDegrees   = 50.0F;  ///< 可行走最大坡度（度）
constexpr float kCharacterStepUpHeight      = 1.0F;   ///< **自动**上台阶高度（格）
constexpr float kSpawnClearance             = 0.5F;   ///< 出生点离地高度（格）

/// 角色**总高**（格）：由胶囊几何推导（当前 2 × (0.6 + 0.3) = 1.80 格）。
/// 跳跃高度按设计规格取本值的 `vx::kJumpApexHeightRatio`（60%）倍，故改身高即自动改跳跃。
constexpr float kCharacterHeight = 2.0F * (kCharacterCylinderHalfHeight + kCharacterRadius);

/// 起跳初速度（格/秒）：由**重力与角色身高推导**（公式见 `vx::JumpVelocityForHeight`）。
/// 当前 24.0 与 1.80 → 7.2 格/秒，对应最高点 1.08 格 = 身高的 60%。**不得**写死。
/// 非编译期常量（含平方根），启动期求值一次。
const float kJumpSpeed = vx::JumpVelocityForHeight(kGravity, kCharacterHeight);

/// 本帧采样到的移动指令（供本帧所有固定逻辑步复用；模拟量只在帧边界读取一次）。
struct MoveCommand {
    float forward  = 0.0F;  ///< +1 前、-1 后
    float strafe   = 0.0F;  ///< +1 右、-1 左
    float speed    = 0.0F;  ///< 地面移动速度（格 / 秒）
    float vertical = 0.0F;  ///< 飞行竖直输入：+1 上升、-1 下降（T12）
    float flySpeed = 0.0F;  ///< 飞行速度（格 / 秒）
    bool  jump     = false; ///< 本帧是否请求起跳
};

/// T24：CPU 相位计时器（复用核心层单调时钟，红线：不用 `system_clock`）。
/// 用法：`Begin()` → 相位工作 → `EndMs()` 得到本相位毫秒数；内部两次 `Tick()`，首值丢弃。
class PhaseTimer {
public:
    void Begin() noexcept { (void)m_clock.Tick(); }
    [[nodiscard]] double EndMs() noexcept { return m_clock.Tick() * 1000.0; }

private:
    vx::Clock m_clock;
};

/// T24：上一帧的 CPU 时间分解（毫秒），供 F1 面板显示。
struct CpuFrameCost {
    double logicMs  = 0.0;  ///< 固定步循环（物理 + 相机）
    double uiMs     = 0.0;  ///< ImGui 帧开始 + 面板构建
    double renderMs = 0.0;  ///< 渲染提交（`RenderFrame` 及其内部上传）
};

/// 定位构建期产出的 Shader 目录：可执行文件在 <build>/bin/，Shader 在 <build>/assets/shaders/。
[[nodiscard]] std::filesystem::path ResolveShaderDir(const char* argv0) {
    const std::filesystem::path exeDir = std::filesystem::path(argv0).parent_path();

    const std::filesystem::path candidates[] = {
        exeDir / "assets" / "shaders",
        exeDir.parent_path() / "assets" / "shaders",
        exeDir.parent_path().parent_path() / "assets" / "shaders",
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate / "mesh.vert.spv")) {
            return candidate;
        }
    }
    return candidates[1];  // 回退到最可能的位置，让报错信息更有指向性
}

/// 仓库内已提交资源（如材质表）的路径；口径与 tests/CMakeLists.txt 的 VOXEL_SOURCE_DIR 一致。
[[nodiscard]] std::filesystem::path SourceAssetPath(const char* relative) {
#ifdef VOXEL_SOURCE_DIR
    return std::filesystem::path(VOXEL_SOURCE_DIR) / relative;
#else
    return std::filesystem::path(relative);
#endif
}

/// 同上；`relative` 为 `std::filesystem::path`（T66：材质表 `[textures].root` 就是该类型）。
[[nodiscard]] std::filesystem::path SourceAssetPath(const std::filesystem::path& relative) {
    return SourceAssetPath(relative.string().c_str());
}

/// V1/V2a：把 `--world=<裸 id 或 清单路径>` 判定为"像路径"（含分隔符或 `.toml` 结尾）。
[[nodiscard]] bool WorldArgumentLooksLikePath(const std::string& argument) {
    return argument.find('/') != std::string::npos || argument.find('\\') != std::string::npos ||
           (argument.size() > 5U && argument.compare(argument.size() - 5U, 5U, ".toml") == 0);
}

/// V1：把 `--world=<裸 id 或 清单路径>` 解析成清单文件路径。
/// `world_a` ⇒ `assets/maps/world_a.toml`；含路径分隔符或以 `.toml` 结尾 ⇒ 按仓库相对路径。
[[nodiscard]] std::filesystem::path ResolveWorldManifestPath(const std::string& argument) {
    if (WorldArgumentLooksLikePath(argument)) {
        return SourceAssetPath(argument);
    }
    return SourceAssetPath(std::string("assets/maps/") + argument + ".toml");
}

/// 一个网格的**世界空间** AABB（世界范围 ≤ 512 格，`float` 足以精确表示整数坐标）。
/// `valid == false` 表示没有包围盒（空网格 / 未上传），剔除时按"可见"处理（宁可多提交）。
struct WorldAabb {
    glm::vec3 min { 0.0F };
    glm::vec3 max { 0.0F };
    bool      valid = false;
};

/// 由**块内局部顶点** + 该块的世界原点求世界 AABB（上传时算一次，之后每帧只做剔除判定）。
[[nodiscard]] WorldAabb BoundsOfVertices(const std::vector<vx::MeshVertex>& vertices, const glm::dvec3& worldOrigin) {
    WorldAabb bounds;
    if (vertices.empty()) {
        return bounds;  // `valid` 保持 false
    }
    glm::vec3 minimum(std::numeric_limits<float>::max());
    glm::vec3 maximum(std::numeric_limits<float>::lowest());
    for (const vx::MeshVertex& vertex : vertices) {
        const glm::vec3 world(static_cast<float>(worldOrigin.x + static_cast<double>(vertex.position[0])),
                              static_cast<float>(worldOrigin.y + static_cast<double>(vertex.position[1])),
                              static_cast<float>(worldOrigin.z + static_cast<double>(vertex.position[2])));
        minimum = glm::min(minimum, world);
        maximum = glm::max(maximum, world);
    }
    bounds.min   = minimum;
    bounds.max   = maximum;
    bounds.valid = true;
    return bounds;
}

/// W7-S3b：地表 **LOD 分环**（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策二）。
///
/// Ring 0 = `0..8` tile（≈ 512 m，**步长 1**，全分辨率）→ Ring 1 = `9..16`（512–1024 m，步长 2）
/// → Ring 2 = `17..32`（1024–2048 m，步长 4）。距离口径 = **Chebyshev tile 距离**（与
/// `TerrainTileWindow::TileDistanceFromCenter` 同源 ⇒ CPU 判定与着色器 morph 判定同源）。
/// 常驻半径 = 最外环 32 + `kTerrainResidencyPrefetchTiles`（= 33 tile ≈ 2112 m）⇒ **视距 2048 m < 常驻半径**
/// （雾盖住流式边界，ADR 0024 硬要求）。
/// **不变量**：常驻量 = 窗口级（`(2·33+1)²` = 4489 上限），**与世界总大小无关**（10km 与 1km 同级）。
constexpr vx::TerrainLodRings kGameTerrainLodRings { { 8, 16, 32 }, { 0, 1, 2 } };

/// 上传（或重传）一个已网格化 tile 的 GPU 网格。
///
/// T41：**不再依赖渲染原点** —— 顶点就是 tile **局部**坐标（0..64 格，由地表网格化器产出），
/// "这块 tile 在世界哪里"由随网格登记的**原点**（= tile 世界原点）在绘制时补上（偏移 = 原点 − 渲染原点）。
/// 因此渲染原点重定基**不会**再触发任何上传（这正是 T41 要消除的卡顿）。
/// `boundsOut` 非空时写出该网格的**世界空间** AABB（T39：每帧视锥剔除用，上传时算一次）。
///
/// T78：地表 tile **一律走带光栅化深度偏移的管线变体**（`depthBiased = true`）。原因是层间接管
/// （ADR 0011 / T61）在**常驻集合的边界环**上让地表四边形仍由地表网格绘制，而体积同时绘制同一层地表
/// （共面）⇒ z-fighting 闪烁；给地表一个正深度偏移即可让体积面稳定胜出（见 `mesh_renderer.*` T78 注释）。
void UploadTileMesh(vx::MeshRenderer& renderer, vx::MeshHandle& handle, const vx::TerrainWorld& world,
                    const vx::TileCoord& coord, WorldAabb* boundsOut = nullptr) {
    if (boundsOut != nullptr) {
        *boundsOut = WorldAabb {};
    }
    const vx::TerrainTileMesh* tileMesh = world.FindMesh(coord.x, coord.z);
    if (tileMesh == nullptr) {
        return;
    }
    const glm::dvec3 tileOrigin(static_cast<double>(vx::TileOriginColumn(coord.x)), 0.0,
                                static_cast<double>(vx::TileOriginColumn(coord.z)));
    if (boundsOut != nullptr) {
        *boundsOut = BoundsOfVertices(tileMesh->mesh.vertices, tileOrigin);
    }
    // W7-S3b：CDLOD morph 参数按 **LOD 档**取（与 `world/streaming/terrain_tile_residency.hpp` 的
    // `TerrainLodMorphRangeForLevel` 同源）：最外环 morphStep = 0（没有更粗的环可 morph）⇒ 不启用。
    const vx::TerrainLodMorphRange morph =
        vx::TerrainLodMorphRangeForLevel(kGameTerrainLodRings, tileMesh->lodLevel);
    if (handle.IsValid()) {
        // **T75 快路径**：地表 tile 的网格拓扑固定（65×65 高度场）或"被体积接管后**变小**" ⇒ 容量通常够用，
        // 于是复用同一对缓冲"提交即走"（旧路径 = `ReleaseMesh` + `UploadMesh` = **等 2 次 fence**）。
        if (renderer.UpdateMeshGeometry(handle, tileMesh->mesh, tileOrigin)) {
            renderer.SetMeshLodMorph(handle, morph.morphStep, morph.startDistance, morph.endDistance);
            return;
        }
        // 容量不够（该 tile 曾被体积接管、网格变小，现在恢复成整张地表）⇒ 重建；**不静默**。
        // T82 之后这里应当**不可达**（见下"按满地表上界预留"）——保留为安全网。
        VX_LOG_WARN("地表 tile (%d, %d) 网格超出上传时的容量 ⇒ 重建 GPU 缓冲（T75 兜底路径；此后该 tile 回到快路径）",
                    coord.x, coord.z);
        renderer.ReleaseMesh(handle);
        handle = vx::MeshHandle {};
    }
    // **按该 LOD 的满地表上界预留（T82 / W7-S3b）**：只有**索引数**会随层间接管（ADR 0011）升降，
    // 上界 = `TerrainLodIndexCount(本 tile 的 LOD 档)`；只要本次网格是"部分地表"（被接管），
    // 就按这个上界建缓冲 ⇒ 该 tile 之后无论接管如何翻转都走快路径、**永不重建**。
    // 为什么不在**所有** tile 上预留：常驻集合是窗口级（10km 下至多 4489 个 tile），按 LOD 上界预留
    // 会把显存推到不可接受（LOD0 上界 ≈ 98 KB/索引 + 135 KB/顶点）——而"部分地表"的 tile 只可能落在
    // 可挖区（中心 4×4 tile）内 ⇒ **至多 16 个**，代价有上界。
    // W7-S3b 修正：上界**随 LOD 档变化**（否则 LOD2 的 tile 也会被预留成 LOD0 的 24576 个索引）。
    // 口径提醒：`reserve*Count` 是**总容量**（实现取 `max(本次数量, reserve)`），不是"额外预留"。
    const std::uint32_t lodIndexBound = static_cast<std::uint32_t>(vx::TerrainLodIndexCount(tileMesh->lodLevel));
    const std::uint32_t reserveIndices =
        (tileMesh->mesh.indices.size() < static_cast<std::size_t>(lodIndexBound)) ? lodIndexBound : 0U;
    handle = renderer.UploadMesh(tileMesh->mesh, tileOrigin, /*emissive=*/false,
                                 /*reserveVertexCount=*/0, reserveIndices, /*depthBiased=*/true);
    renderer.SetMeshLodMorph(handle, morph.morphStep, morph.startDistance, morph.endDistance);
}

// ---------------------------------------------------------------------------
// W7-S3a：地表 tile 的**常驻集合**（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md) 决策一）
//
// 三个并行数组（同下标 = 同一个 tile）：`coords` / `handles` / `bounds`。由 `TerrainTileScheduler` 决定
// 谁该常驻；本层的四个小函数负责"把世界数据变成可渲染 / 可碰撞的资源"与"卸载时回收"。
// 为什么用并行数组而不是 map：渲染循环要按视锥剔除**逐 tile** 遍历，数组连续、确定序、稳态零分配；
// 卸载用**交换删除**（下标会变，但所有引用都在同一帧内即时解析，不留悬空下标）。
// ---------------------------------------------------------------------------

/// 常驻集合的资源绑定（避免六七个参数一路传下去）。
struct TileResidencyResources {
    vx::TerrainWorld&           world;
    vx::TerrainCollision&       collision;
    vx::MeshRenderer&           renderer;
    std::vector<vx::TileCoord>& coords;   ///< 常驻 tile 坐标（与后三者**同下标**）
    std::vector<vx::MeshHandle>& handles;  ///< GPU 网格句柄
    std::vector<WorldAabb>&     bounds;    ///< T39：世界 AABB（剔除用）
    /// W7-S3b：该 tile **当前是否装有高度场碰撞体**（0/1）。用于把"碰撞体随窗口建 / 撤"限制成**状态跃迁**，
    /// 而不是每帧重扫都重做一遍（Jolt 的高度场体构建是毫秒级 ⇒ 必须是跃迁 + 分帧）。
    std::vector<std::uint8_t>&  collisionActive;
};

/// 把一个 tile 追加进常驻集合（**只登记**；世界数据由 `TerrainTileScheduler::Step` 负责生成 / 卸载）。
/// 返回新下标。
[[nodiscard]] std::size_t AppendResidentTile(TileResidencyResources& res, const vx::TileCoord& coord) {
    res.coords.push_back(coord);
    res.handles.push_back(vx::MeshHandle {});
    res.bounds.push_back(WorldAabb {});
    res.collisionActive.push_back(0U);
    return res.coords.size() - 1U;
}

/// 上传下标 `index` 的 tile 网格（并刷新其世界 AABB）。空网格 = 不可见 ⇒ 句柄保持无效。
///
/// W7-S3b：上传完成后**释放该 tile 的 CPU 侧网格**（`TerrainWorld::ReleaseTileMeshCpu`）——
/// 释放后只留高度 + `meshEmpty` 标记（重网格从高度重建，接管判据读标记）⇒ 地形 CPU 常驻从 ≈160 MB 压回预算内。
void UploadResidentTile(TileResidencyResources& res, std::size_t index) {
    UploadTileMesh(res.renderer, res.handles[index], res.world, res.coords[index], &res.bounds[index]);
    res.world.ReleaseTileMeshCpu(res.coords[index].x, res.coords[index].z);
}

/// 同步下标 `index` 的 tile 的**高度场碰撞体**（ADR 0012 接管）：可见面全归体积 / 地表壳 ⇒ 交出；
/// 否则建高度场。判据与 ADR 0011 的四边形跳过**同源**（同一次 `BuildTerrainMesh`）。
///
/// W7-S3b：`wantCollision == false`（该 tile 超出 `kTerrainCollisionRadiusTiles`）⇒ 直接**移除**碰撞体并返回
/// `false`（碰撞只在玩家附近有意义；否则 4489 个高度场静态体会压垮 Jolt 的宽相位与内存）。
/// 返回是否**保留高度场碰撞体**（`false` = 未保留：已交出，或超出碰撞半径）。
[[nodiscard]] bool SyncResidentTileCollision(TileResidencyResources& res, std::size_t index,
                                             bool wantCollision) {
    const vx::TileCoord& coord = res.coords[index];
    if (!wantCollision) {
        res.collision.RemoveTile(coord.x, coord.z);
        res.collisionActive[index] = 0U;
        return false;
    }
    const vx::TerrainTileMesh* tileMesh = res.world.FindMesh(coord.x, coord.z);
    // W7-S3b：读 `meshEmpty` 标记（**不是** `indices.empty()`）—— 上传后 CPU 侧网格已释放，`indices` 恒为空。
    const bool                 empty    = (tileMesh != nullptr) && tileMesh->meshEmpty;
    if (empty) {
        res.collision.RemoveTile(coord.x, coord.z);
        res.collisionActive[index] = 0U;
        return false;
    }
    (void)res.collision.SyncTile(res.world, coord.x, coord.z);
    res.collisionActive[index] = 1U;
    return true;
}

/// 常驻集合里 `coord` 的下标；不存在返回 `false`。O(常驻数)（常驻集合是窗口级，量小）。
[[nodiscard]] bool FindResidentTileIndex(const TileResidencyResources& res, const vx::TileCoord& coord,
                                         std::size_t& outIndex) {
    for (std::size_t i = 0; i < res.coords.size(); ++i) {
        if (res.coords[i] == coord) {
            outIndex = i;
            return true;
        }
    }
    return false;
}

/// 把 `coord` 移出常驻集合：释放 GPU 网格与碰撞体、卸载世界数据（高度 + 网格），再做**交换删除**。
/// 返回是否原本常驻（`false` = 不在集合里，无操作）。
bool RemoveResidentTile(TileResidencyResources& res, const vx::TileCoord& coord) {
    std::size_t index = 0;
    if (!FindResidentTileIndex(res, coord, index)) {
        return false;
    }
    if (res.handles[index].IsValid()) {
        res.renderer.ReleaseMesh(res.handles[index]);
    }
    res.collision.RemoveTile(coord.x, coord.z);
    (void)res.world.UnloadTile(coord.x, coord.z);

    const std::size_t last = res.coords.size() - 1U;
    if (index != last) {
        res.coords[index]          = res.coords[last];
        res.handles[index]         = res.handles[last];
        res.bounds[index]          = res.bounds[last];
        res.collisionActive[index] = res.collisionActive[last];
    }
    res.coords.pop_back();
    res.handles.pop_back();
    res.bounds.pop_back();
    res.collisionActive.pop_back();
    return true;
}

/// 把绝对世界空间相机求值结果平移到渲染原点附近：`eye` / `target` / `view` 全部减去渲染原点。
/// 上传的顶点已是相对同一原点的坐标，故绘制结果与绝对世界空间一致，但 float 只承载小数值（红线 6）。
[[nodiscard]] vx::CameraView RelativeCameraView(const vx::CameraView& view, const glm::dvec3& renderOrigin) {
    const glm::vec3 origin = glm::vec3(renderOrigin);  // 渲染原点取整，float 可精确表示

    vx::CameraView relative;
    relative        = view;
    relative.eye    = view.eye - origin;
    relative.target = view.target - origin;
    relative.view   = glm::lookAt(relative.eye, relative.target, glm::vec3(0.0F, 1.0F, 0.0F));
    relative.viewProjection = view.projection * relative.view;
    return relative;
}

/// 视锥剔除判据（T39）：世界 AABB → 渲染空间 → 与相机视锥做**保守**相交。
///
/// 为什么不能只按相机视锥剔除：**影子可以落在"看不到投射体"的地方**（高塔在画面外、影子在画面内）。
/// 只按视锥剔除会重现"阴影随视角消失"—— 那正是 B6 / B8 修过的缺陷。故这里先把 AABB 沿**太阳方向
/// 的反向**扫掠到地面（= 该物体最坏情况下能投影到的区域），取"物体 ∪ 影子落点"的并集再判可见性：
/// 保守（可能多留几个网格），但**不会丢阴影**。
///
/// 渲染空间：上传的顶点是**相机相对**坐标，故 AABB 也要减去渲染原点（红线 6）。
[[nodiscard]] bool VisibleToCamera(const vx::Frustum& frustum, const WorldAabb& bounds, const glm::dvec3& renderOrigin,
                                   const glm::vec3& sunDirection) {
    if (!bounds.valid) {
        return true;  // 无包围盒 ⇒ 保守提交
    }
    glm::vec3 minimum = bounds.min;
    glm::vec3 maximum = bounds.max;

    // 影子的水平位移 ≈ (太阳方向的水平分量) × (高度 / 太阳高度的正弦)；竖直方向落到地面（y = 0）。
    const float     sunY  = std::max(sunDirection.y, 1.0e-3F);  // 太阳近地平线时钳一个下限，避免扫掠发散
    const float     top   = std::max(maximum.y, 0.0F);
    const float     reach = top / sunY;
    const glm::vec3 offset(-sunDirection.x * reach, -top, -sunDirection.z * reach);
    minimum = glm::min(minimum, minimum + offset);
    maximum = glm::max(maximum, maximum + offset);

    const glm::vec3 origin = glm::vec3(renderOrigin);  // 渲染原点取整，float 可精确表示
    return vx::FrustumIntersectsAabb(frustum, minimum - origin, maximum - origin);
}

/// 把主角胶囊的**局部**顶点（脚底为原点）搬到**渲染相对**空间：`渲染相对 = 脚底 + 局部 − 渲染原点`。
/// 与地表 / 体积网格的分工（T41）：后者的顶点是**网格局部**坐标、位置由逐网格 uniform 补上；
/// 动态网格没有固定原点，故仍按本式在 CPU 侧烘焙（世界定位用 `double` 累加后再落回 `float`，红线 6），
/// 并在刷新顶点时把 `origin` 一并设为**当前渲染原点**（偏移恒 0）。就地改写 `vertices`，避免每帧堆分配。
void UpdateCharacterRenderVertices(std::vector<vx::MeshVertex>& vertices, const vx::MeshData& local,
                                   const glm::dvec3& feet, const glm::dvec3& renderOrigin) {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const vx::MeshVertex& source = local.vertices[i];
        vx::MeshVertex&       target = vertices[i];
        target                       = source;  // 法线与材质权重是静态的，只需重算位置
        target.position[0] = static_cast<float>(feet.x + static_cast<double>(source.position[0]) - renderOrigin.x);
        target.position[1] = static_cast<float>(feet.y + static_cast<double>(source.position[1]) - renderOrigin.y);
        target.position[2] = static_cast<float>(feet.z + static_cast<double>(source.position[2]) - renderOrigin.z);
    }
}

/// 把光球球体的**局部**顶点（球心为原点）搬到**渲染相对**空间：先按 `alpha` 在上一 / 当前逻辑步位置之间
/// 插值（与主角、相机同一套渲染插值约定，红线 11：插值只用于渲染），再减去渲染原点（与主角同约定，T41）。
void UpdateOrbRenderVertices(std::vector<vx::MeshVertex>& vertices, const vx::MeshData& local,
                             const glm::dvec3& previousCenter, const glm::dvec3& center, double alpha,
                             const glm::dvec3& renderOrigin) {
    const glm::dvec3 interpolated = previousCenter + (center - previousCenter) * alpha;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const vx::MeshVertex& source = local.vertices[i];
        vx::MeshVertex&       target = vertices[i];
        target                       = source;
        target.position[0] = static_cast<float>(interpolated.x + static_cast<double>(source.position[0]) - renderOrigin.x);
        target.position[1] = static_cast<float>(interpolated.y + static_cast<double>(source.position[1]) - renderOrigin.y);
        target.position[2] = static_cast<float>(interpolated.z + static_cast<double>(source.position[2]) - renderOrigin.z);
    }
}

/// 推进一个固定逻辑步：把移动指令按相机 yaw 转到世界方向，交给 Jolt `CharacterVirtual` 求解
/// （重力 / 上坡 / **自动上台阶**都在物理层内完成），再让第三人称相机跟随角色。
///
/// T12 飞行模式：关闭重力（`gravity = 0`）并直接给竖直速度，使角色可自由升 / 降、悬停；
/// 切回普通模式时由调用方清零速度，故不会残留速度、也不会穿过地形（碰撞仍在生效）。
///
/// T54：起跳门槛 = **可行走地面**（`CharacterState::walkableGround`，不含"站在过陡坡上"）——
/// 缺陷"贴着垂直岩壁能一直跳"的机制就是此前用了语义更宽的 `onGround`。同时按业内规范补上
/// **土狼时间**与**跳跃缓冲**两个容差窗口（口径与实现见 `vx::AdvanceJumpAssist`）。
void StepCharacter(vx::PhysicsWorld& physics, vx::PhysicsWorld::CharacterHandle character,
                   vx::ThirdPersonCamera& camera, const MoveCommand& command, bool flying,
                   vx::JumpAssist& jumpAssist) {
    const vx::PhysicsWorld::CharacterState state = physics.GetCharacterState(character);

    // 移动方向由**纯函数**给出，基向量语义（W = 相机前、D = 相机右）由单测锁定：
    // 相机右向 = cross(前向, 上方)，此前误写成其相反数，导致 A/D 反向（缺陷 B1）。
    glm::vec3 direction = vx::CameraRelativeMoveDirection(camera.Yaw(), command.forward, command.strafe);

    glm::vec3   velocity = state.velocity;
    const float lengthSq = glm::dot(direction, direction);
    velocity.x           = 0.0F;
    velocity.z           = 0.0F;
    if (lengthSq > 0.0F) {
        direction /= std::sqrt(lengthSq);
    }

    if (flying) {
        const float speed = (lengthSq > 0.0F) ? command.flySpeed : 0.0F;
        velocity.x        = direction.x * speed;
        velocity.z        = direction.z * speed;
        velocity.y        = command.vertical * command.flySpeed;
        physics.SetCharacterVelocity(character, velocity);
        physics.MoveCharacter(character, static_cast<float>(vx::kFixedDt), glm::vec3(0.0F));  // 无重力
        jumpAssist.Reset();  // 飞行中不积累容差 ⇒ 切回普通模式的瞬间不会凭空起跳
    } else {
        if (lengthSq > 0.0F) {
            velocity.x = direction.x * command.speed;
            velocity.z = direction.z * command.speed;
        }
        // T54：土狼时间 + 跳跃缓冲（固定步推进）。门槛是 `walkableGround`，**不是** `onGround`。
        if (vx::AdvanceJumpAssist(jumpAssist, state.walkableGround, command.jump,
                                  static_cast<float>(vx::kFixedDt))) {
            velocity.y = kJumpSpeed;  // 竖直分量由玩法层给冲量，重力由物理层在步内累加
        }
        physics.SetCharacterVelocity(character, velocity);
        physics.MoveCharacter(character, static_cast<float>(vx::kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));
    }

    const vx::PhysicsWorld::CharacterState after = physics.GetCharacterState(character);
    camera.Advance(glm::vec3(static_cast<float>(after.position.x), static_cast<float>(after.position.y),
                             static_cast<float>(after.position.z)));
}

/// 光球发射时的枪口前移量（格）：从角色胸口沿瞄准方向前移，避免弹丸生成在胶囊内部。
constexpr float kMuzzleForwardOffset = 1.2F;

/// 枪口高度（角色总高的比例）：约胸口位置。
constexpr float kMuzzleHeightRatio = 0.75F;

/// 相机视线方向 = 屏幕中心"准星"的方向。
///
/// 第三人称下视线由 `eye → target` 给出：相机可能被避障 / 离地间隙抬高，因此**不能**用 yaw / pitch
/// 直接算（那只是"期望朝向"）。这里走与渲染同一套 `Evaluate`，保证弹道方向与玩家看到的画面一致。
[[nodiscard]] glm::vec3 AimDirection(const vx::ThirdPersonCamera& camera, const vx::ITerrainQuery& terrain) {
    const vx::CameraView view = camera.Evaluate(1.0, &terrain);
    const glm::vec3     delta = view.target - view.eye;
    const float         lengthSq = glm::dot(delta, delta);
    if (!(lengthSq > 0.0F)) {
        return glm::vec3(0.0F, 0.0F, 1.0F);  // 不可达：相机保证 eye ≠ target（见 kCameraMinDistance）
    }
    return delta / std::sqrt(lengthSq);
}

/// T27：把地表高度场与可挖体积合成弹道查询契约。
///
/// 分流规则与爆炸一致：**区域内以体积为准**（已挖掉的地方就是空的 ⇒ 光球能飞进洞里），
/// 区域外以地表高度场为准（`y <= 地表高度` 即实心）。
///
/// T46（[ADR 0017](../../docs/adr/0017-landing-by-material-rigid-vs-granular.md)）的"保留残骸也要挡光球"由本类承担；
/// T48（[ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策三）：
/// **动态刚体**（倒塌中的整体，含"掉落中"与"保留中"两态）一律走**物理场景查询** ——
/// 它们的体素已被抽出（体积里是空的），只按密度判定会被误判为空气 ⇒ 光球穿过去（BUG4）。
class GameOrbWorldQuery final : public vx::IOrbWorldQuery {
public:
    GameOrbWorldQuery(const vx::TerrainWorld& terrain, const vx::DigVolumeWorld& volumes,
                      const vx::PhysicsWorld& physics) noexcept
        : m_terrain(terrain), m_volumes(volumes), m_physics(physics) {}

    [[nodiscard]] bool IsSolid(double x, double y, double z) const override {
        // 地形与体积按现行"谁画谁挡同源"的口径（ADR 0011 / 0012）。
        // T48 起**不再**在这里手工判定保留残骸 —— 那由 `SegmentHitsDynamic` 的物理查询负责。
        if (m_volumes.IsInsideRegion(x, y, z)) {
            return m_volumes.IsSolid(x, y, z);
        }
        float surface = 0.0F;
        if (!m_terrain.QueryHeight(static_cast<float>(x), static_cast<float>(z), surface)) {
            return false;  // 无地形数据：不阻挡
        }
        return y <= static_cast<double>(surface);
    }

    /// T48：动态刚体走物理查询 —— 命中点落在**真实凸包表面**（不再是手工 OBB 近似），
    /// 且**覆盖"飞行中"这一态**（无需任何状态记账）。`outBody` 让玩法层知道"是哪个整体被击中"。
    [[nodiscard]] bool SegmentHitsDynamic(const glm::dvec3& from, const glm::dvec3& to, glm::dvec3& outPoint,
                                          std::uint32_t& outBody) const override {
        const vx::PhysicsWorld::RayCastHit hit = m_physics.RayCastDynamic(from, to);
        if (!hit.hit) {
            return false;
        }
        outPoint = hit.point;
        outBody  = hit.body;
        return true;
    }

private:
    const vx::TerrainWorld&   m_terrain;
    const vx::DigVolumeWorld& m_volumes;
    const vx::PhysicsWorld&   m_physics;
};

/// 相机避障 / 安全网的采样步长（格）：固定步长 ⇒ 结果确定（红线 7），无分配。
constexpr float kCameraQueryStepBlocks = 0.5F;

/// 缺陷修复（人工实测第 7 轮）：相机的**组合**地形查询 —— 地表高度场 + 可挖体积。
///
/// **为什么必须组合**：体积挖出的洞在地表高度场里**仍然显示为实心**（爆炸只改体积密度、不改高度场）。
/// 相机若只查高度场，站在洞里的角色会把相机顶到"旧地表"之上 ⇒ 视角退化为俯视。
/// 分流口径与 `GameOrbWorldQuery`（弹道）完全一致：**区域内以体积为准**（ADR 0011 / 0012 的
/// 「谁来画 / 谁来挡必须同源」原则 —— 这里是第三种消费者：谁来"挡相机"）。
class GameCameraQuery final : public vx::ITerrainQuery {
public:
    GameCameraQuery(const vx::TerrainWorld& terrain, const vx::DigVolumeWorld& volumes) noexcept
        : m_terrain(terrain), m_volumes(volumes) {}

    [[nodiscard]] bool QueryHeight(float worldX, float worldZ, float& outHeight) const override {
        // 区域内由体积承担地形（ADR 0012）：该列**没有**"地表高度"这一说（洞顶不是地面），
        // 故不报高度；相对地，`IsSolid` 会按体积回答"这里到底挡不挡"。
        if (m_volumes.IsInsideRegion(static_cast<double>(worldX), 0.0, static_cast<double>(worldZ))) {
            return false;
        }
        return m_terrain.QueryHeight(worldX, worldZ, outHeight);
    }

    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override {
        const double x = static_cast<double>(point.x);
        const double y = static_cast<double>(point.y);
        const double z = static_cast<double>(point.z);
        if (m_volumes.IsInsideRegion(x, y, z)) {
            return m_volumes.IsSolid(x, y, z);  // 洞内为空 ⇒ 相机不被顶出，视角保持水平跟随
        }
        float height = 0.0F;
        return m_terrain.QueryHeight(point.x, point.z, height) && y <= static_cast<double>(height);
    }

    [[nodiscard]] bool QueryObstruction(const glm::vec3& from, const glm::vec3& to,
                                        float& outSafeT) const override {
        outSafeT = 1.0F;
        const float length = glm::length(to - from);
        if (!(length > 0.0F)) {
            return false;
        }
        const int   steps    = std::max(1, static_cast<int>(std::ceil(length / kCameraQueryStepBlocks)));
        float       lastSafe = 0.0F;
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            if (IsSolid(glm::mix(from, to, t))) {
                outSafeT = lastSafe;  // 返回"最后一个安全点"的比例
                return true;
            }
            lastSafe = t;
        }
        return false;
    }

    /// W6f：**球投射探针** —— 把相机近似为半径 `radius` 的球，而非一个点。
    ///
    /// 做法：沿线段**固定步长**前进，每步检查**中心 + 垂直于轴的 4 个环点**（两个正交方向 × 正负），
    /// 任一实心即视为遮挡，返回**最后一个安全比例**。
    /// 为什么用"步进 + 环点"而不是解析胶囊求交：只依赖既有的 `IsSolid` 点查询（世界层不必再暴露新接口），
    /// 且固定步长 / 固定环方向 ⇒ 结果确定（红线 7）。`radius <= 0` 时逐字退回 `QueryObstruction`。
    [[nodiscard]] bool QueryObstructionWithRadius(const glm::vec3& from, const glm::vec3& to, float radius,
                                                  float& outSafeT) const override {
        if (!(radius > 0.0F)) {
            return QueryObstruction(from, to, outSafeT);
        }
        outSafeT = 1.0F;
        const glm::vec3 delta  = to - from;
        const float     length = glm::length(delta);
        if (!(length > 0.0F)) {
            return false;
        }
        // 与轴正交的两个单位方向：任取一个不平行于轴的参考轴做叉乘（轴的 |y| 接近 1 时换参考轴）。
        const glm::vec3 axis = delta / length;
        const glm::vec3 reference =
            (std::abs(axis.y) < 0.9F) ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);
        const glm::vec3 tangent   = glm::normalize(glm::cross(reference, axis));
        const glm::vec3 bitangent = glm::cross(axis, tangent);
        const glm::vec3 ring[4]   = { tangent * radius, -tangent * radius, bitangent * radius, -bitangent * radius };

        const int steps    = std::max(1, static_cast<int>(std::ceil(length / kCameraQueryStepBlocks)));
        float     lastSafe = 0.0F;
        for (int i = 1; i <= steps; ++i) {
            const float     t      = static_cast<float>(i) / static_cast<float>(steps);
            const glm::vec3 center = glm::mix(from, to, t);
            bool            blocked = IsSolid(center);
            for (int r = 0; r < 4 && !blocked; ++r) {
                blocked = IsSolid(center + ring[r]);
            }
            if (blocked) {
                outSafeT = lastSafe;
                return true;
            }
            lastSafe = t;
        }
        return false;
    }

private:
    const vx::TerrainWorld&   m_terrain;
    const vx::DigVolumeWorld& m_volumes;
};

/// 上传（或重传）一个可挖体积块的 GPU 网格。该块无表面（全实心 / 全空）时释放旧网格。
///
/// T41：与地表 tile 同约定 —— 顶点就是块**局部**坐标（0..32 格，由 Surface Nets 产出），
/// "这块在世界哪里"由随网格登记的**原点**（= 块世界原点）在绘制时补上；**不再依赖渲染原点**，
/// 故渲染原点重定基不会触发上传。
/// `boundsOut` 非空时写出该网格的**世界空间** AABB（T39：每帧视锥剔除用，上传时算一次）。
void UploadVolumeMesh(vx::MeshRenderer& renderer, vx::MeshHandle& handle, const vx::DigVolumeWorld& volumes,
                      const vx::BlockCoord& coord, WorldAabb* boundsOut = nullptr) {
    if (boundsOut != nullptr) {
        *boundsOut = WorldAabb {};
    }

    const vx::MeshData* blockMesh = volumes.FindMesh(coord);
    if (blockMesh == nullptr || blockMesh->vertices.empty() || blockMesh->indices.empty()) {
        // 无表面（全实心 / 全空）：**就地置空**（`usedIndexCount = 0` ⇒ 本帧不可见）。
        // T75：旧路径在这里会 `ReleaseMesh` + 下一次重建缓冲；现在复用同一对缓冲、零上传。
        if (handle.IsValid()) {
            (void)renderer.UpdateMeshGeometry(handle, vx::MeshData {}, glm::dvec3(0.0));
        }
        return;
    }

    const glm::dvec3 blockOrigin(static_cast<double>(vx::BlockOriginBlocks(coord.x)),
                                 static_cast<double>(vx::BlockOriginBlocks(coord.y)),
                                 static_cast<double>(vx::BlockOriginBlocks(coord.z)));
    if (boundsOut != nullptr) {
        *boundsOut = BoundsOfVertices(blockMesh->vertices, blockOrigin);
    }

    // **T75 快路径**：挖除会改变顶点 / 索引数，但**复用同一对 GPU 缓冲**"提交即走"
    // （旧路径 = `ReleaseMesh` + `UploadMesh` = 建 2 个 transfer buffer + 2 个 GPU 缓冲 + **等 2 次 fence**）。
    if (handle.IsValid() && renderer.UpdateMeshGeometry(handle, *blockMesh, blockOrigin)) {
        return;
    }
    if (handle.IsValid()) {
        // 容量不够（挖出的腔体比首次上传时更大）⇒ 必须重建；**不静默**（SKILL：禁止默默降级）。
        VX_LOG_WARN("可挖体积块 (%d, %d, %d) 网格超出上传时的容量（%zu 顶点 / %zu 索引）⇒ 重建 GPU 缓冲并按 2× 预留"
                    "（T75 兜底路径；此后该块回到非阻塞快路径）",
                    coord.x, coord.y, coord.z, blockMesh->vertices.size(), blockMesh->indices.size());
        renderer.ReleaseMesh(handle);
        handle = vx::MeshHandle {};
    }
    // 预留 2×：挖洞只会让 SN 网格继续变大，预留后后续重网格都走快路径（容量按缓冲实际大小记账）。
    handle = renderer.UploadMesh(*blockMesh, blockOrigin, /*emissive=*/false,
                                 static_cast<std::uint32_t>(blockMesh->vertices.size() * 2U),
                                 static_cast<std::uint32_t>(blockMesh->indices.size() * 2U));
}

/// 一个**常驻**可挖体积块在游戏层的槽位（T61）：常驻集合是**动态**的 ⇒ 不能再用"按块坐标下标索引的
/// 平行数组"（数组一增删，所有下标全错）。这里改成**按块坐标索引**的表。
struct VolumeSlot {
    vx::MeshHandle handle {};  ///< 该块的 GPU 网格（无表面时保持无效）
    WorldAabb      bounds {};  ///< T39：世界空间 AABB（视锥剔除用；上传时更新）
};

/// V0b/V0c：一个已接线的**物件实例**在游戏层的槽位（ADR 0004 层③）。
///
/// 生命周期（`plans/v0.5.md` §1.5）：**静态**（默认：静态三角网碰撞体）→ 失去支撑 / 被爆炸命中 ⇒
/// **转动态刚体**（掉落 / 被炸飞）→ 若可破坏则**被摧毁**（网格 + 碰撞体 + 实体一并释放）。
/// 渲染网格是**未旋转**的局部几何（位姿由 `SetMeshTransform` 施加）；静态碰撞体把朝向**烘进顶点**
/// （静态体没有旋转接口），动态刚体则用 `ConvexHullDesc::rotation`。
struct ObjectSlot {
    std::uint32_t                id = 0;           ///< `ObjectLayer` 的稳定 id（0 = 已移除）
    const vx::ObjectType*        type = nullptr;   ///< 类型（生命周期 = 所属 `ObjectTable`）
    vx::MeshHandle               handle {};        ///< GPU 网格（上传失败 / 已摧毁时无效）
    WorldAabb                    bounds {};        ///< 世界空间 AABB（视锥剔除）
    vx::MeshData                 localMesh {};     ///< 局部网格（转动态凸包用；底面中心为原点）
    vx::PhysicsWorld::BodyHandle body {};          ///< 当前碰撞体（静态三角网 / 动态凸包；0 = 无）
    glm::dvec3                   position { 0.0 }; ///< 当前**底面中心**世界坐标
    float                        yawDegrees = 0.0F;
    bool                         dynamic = false;  ///< 是否已转动态刚体（转后不再转回，由 Jolt 休眠）
    bool                         removed = false;  ///< 是否已被摧毁（句柄已释放）
};

/// 支撑探测的**下探深度**（格）：探测点取"底面中心 − 该深度"。小于它 ⇒ 视为脚下已是空的。
constexpr double kObjectSupportProbeDepth = 0.25;

/// 支撑检查的**固定步间隔**（成本与物件数成正比，故不每步做；见 V0c 的已知限制）。
constexpr int kObjectSupportCheckIntervalSteps = 10;

/// 被爆炸"炸飞"的初速度（格/秒；仅用于**不可破坏**物件被炸开）。
constexpr float kObjectBlastSpeedBlocksPerSecond = 7.0F;

/// 物件质量（**占位口径**，只影响掉落 / 被炸飞的手感；正式数据随 kit 资产引入）。
[[nodiscard]] constexpr float ObjectPlaceholderMass(vx::ObjectAssetKind kind) noexcept {
    switch (kind) {
        case vx::ObjectAssetKind::DirtPile:
            return 120.0F;
        case vx::ObjectAssetKind::Stone:
            return 400.0F;
        case vx::ObjectAssetKind::Crate:
            return 60.0F;
    }
    return 120.0F;
}

/// 把网格顶点摊平成 `ConvexHullDesc` / `MeshDesc` 需要的"3 float / 顶点"紧凑缓冲。
[[nodiscard]] std::vector<float> FlattenObjectPositions(const vx::MeshData& mesh) {
    std::vector<float> positions;
    positions.reserve(mesh.vertices.size() * 3U);
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        positions.push_back(vertex.position[0]);
        positions.push_back(vertex.position[1]);
        positions.push_back(vertex.position[2]);
    }
    return positions;
}

/// **静态 ⇒ 动态**：把物件的静态三角网碰撞体换成**动态凸包刚体**
/// （失支撑掉落与被爆炸炸飞的**共同入口**；与 ADR 0015/0017 的"失支撑 ⇒ 动态刚体"同一口径）。
/// `initialVelocity` = 初速度（格/秒；零 = 单纯掉落）。凸包构建失败 ⇒ 保持静态并 WARN（不静默）。
[[nodiscard]] bool ConvertObjectToDynamic(ObjectSlot& slot, vx::PhysicsWorld& physics,
                                          const glm::vec3& initialVelocity) {
    if (slot.removed || slot.dynamic || slot.type == nullptr) {
        return false;
    }
    const std::vector<float> positions = FlattenObjectPositions(slot.localMesh);
    if (positions.size() < 12U) {  // 凸包至少需要 4 个点
        VX_LOG_WARN("物件 [%s] 顶点不足，无法转动态刚体", slot.type->id.c_str());
        return false;
    }

    vx::PhysicsWorld::ConvexHullDesc hull;
    hull.positions      = positions.data();
    hull.pointCount     = slot.localMesh.vertices.size();
    hull.originX        = slot.position.x;
    hull.originY        = slot.position.y;
    hull.originZ        = slot.position.z;
    hull.mass           = ObjectPlaceholderMass(slot.type->kind);
    hull.friction       = 0.6F;
    hull.restitution    = 0.0F;
    hull.linearVelocity = initialVelocity;
    hull.rotation       = glm::angleAxis(glm::radians(slot.yawDegrees), glm::vec3(0.0F, 1.0F, 0.0F));

    const vx::PhysicsWorld::BodyHandle body = physics.AddDynamicConvexHull(hull);
    if (body == 0) {
        VX_LOG_WARN("物件 [%s] 转动态刚体失败（凸包构建失败）⇒ 保持静态", slot.type->id.c_str());
        return false;
    }
    if (slot.body != 0) {
        physics.RemoveBody(slot.body);
    }
    slot.body    = body;
    slot.dynamic = true;
    return true;
}

/// **摧毁**一个物件：释放 GPU 网格与碰撞体、从 `ObjectLayer` 移除实体。
/// 前置条件：调用方已确认"总开关打开 且 `type.destructible`"（见 `BlastObjects`）。
void DestroyObjectSlot(ObjectSlot& slot, vx::ObjectLayer& layer, vx::PhysicsWorld& physics,
                       vx::MeshRenderer& renderer) {
    if (slot.removed) {
        return;
    }
    if (slot.handle.IsValid()) {
        renderer.ReleaseMesh(slot.handle);
    }
    if (slot.body != 0) {
        physics.RemoveBody(slot.body);
    }
    if (slot.id != 0) {
        (void)layer.Remove(slot.id);
    }
    slot.handle  = vx::MeshHandle {};
    slot.body    = 0;
    slot.removed = true;
}

/// 一次爆炸对物件的**按类型分流**（`plans/v0.5.md` §1.5）：
///   - `destructible_enabled && type.destructible` ⇒ **摧毁消失**；
///   - 否则 ⇒ **只被炸飞**（静态 ⇒ 转动态刚体并给向外初速；已是动态 ⇒ 仅唤醒）。
void BlastObjects(std::vector<ObjectSlot>& slots, const vx::ObjectTable& table, vx::ObjectLayer& layer,
                  vx::PhysicsWorld& physics, vx::MeshRenderer& renderer, const glm::dvec3& center,
                  float radiusBlocks) {
    for (ObjectSlot& slot : slots) {
        if (slot.removed || slot.type == nullptr) {
            continue;
        }
        // 以物件**中心**（底面中心 + 半高）到爆心的距离判定，再用**包围球半径**放宽（保守，宁可多算）。
        const glm::dvec3 objectCenter(
            slot.position.x, slot.position.y + static_cast<double>(slot.type->halfExtentY), slot.position.z);
        const float halfDiagonal = std::sqrt(slot.type->halfExtentX * slot.type->halfExtentX +
                                             slot.type->halfExtentY * slot.type->halfExtentY +
                                             slot.type->halfExtentZ * slot.type->halfExtentZ);
        const glm::dvec3 delta    = objectCenter - center;
        const double     distance = std::sqrt(glm::dot(delta, delta));
        if (distance > static_cast<double>(radiusBlocks) + static_cast<double>(halfDiagonal)) {
            continue;  // 不在本次爆炸影响范围内
        }

        if (table.destructibleEnabled && slot.type->destructible) {
            const std::string typeId = slot.type->id;  // `slot.type` 在摧毁后仍有效，但先留一份便于日志
            DestroyObjectSlot(slot, layer, physics, renderer);
            VX_LOG_INFO("物件 [%s] 被爆炸**摧毁**（网格 + 碰撞体 + 实体一并释放）", typeId.c_str());
            continue;
        }

        // 不可破坏（或总开关关闭）：只被炸飞 —— 向外 + 略微向上。
        glm::vec3 direction(0.0F, 1.0F, 0.0F);
        if (distance > 1.0e-3) {
            direction = glm::normalize(glm::vec3(static_cast<float>(delta.x),
                                                 static_cast<float>(delta.y + static_cast<double>(halfDiagonal)),
                                                 static_cast<float>(delta.z)));
        }
        const glm::vec3 velocity = direction * kObjectBlastSpeedBlocksPerSecond;
        if (slot.dynamic) {
            // 已是动态：唤醒即可（本阶段 `PhysicsWorld` 没有"设置线速度"接口 ⇒ 不再补冲量，登记为已知限制）。
            physics.ActivateBody(slot.body);
        } else if (ConvertObjectToDynamic(slot, physics, velocity)) {
            VX_LOG_INFO("物件 [%s] 不可破坏 ⇒ 被爆炸**炸飞**（转动态刚体，初速 %.1f 格/秒）",
                        slot.type->id.c_str(), static_cast<double>(kObjectBlastSpeedBlocksPerSecond));
        }
    }
}

/// **支撑检查**（每 `kObjectSupportCheckIntervalSteps` 个固定步一次）：底面探测点全为空 ⇒ 失去支撑
/// ⇒ 静态物件转动态刚体（**掉落**）。判据用与弹道相同的"高度场 + 可挖体积"点查询（"谁画谁挡同源"）。
void CheckObjectSupports(std::vector<ObjectSlot>& slots, const GameOrbWorldQuery& solidQuery,
                         vx::PhysicsWorld& physics) {
    for (ObjectSlot& slot : slots) {
        if (slot.removed || slot.dynamic || slot.type == nullptr) {
            continue;  // 已是动态 ⇒ 由 Jolt 负责重力
        }
        const auto probes =
            vx::ObjectSupportProbes(slot.type->halfExtentX, slot.type->halfExtentZ, slot.yawDegrees);
        bool solidFlags[vx::kObjectSupportProbeCount] = {};
        for (std::size_t i = 0; i < probes.size(); ++i) {
            solidFlags[i] = solidQuery.IsSolid(slot.position.x + static_cast<double>(probes[i].x),
                                               slot.position.y - kObjectSupportProbeDepth,
                                               slot.position.z + static_cast<double>(probes[i].z));
        }
        if (!vx::ObjectHasSupport(solidFlags, probes.size()) &&
            ConvertObjectToDynamic(slot, physics, glm::vec3(0.0F))) {
            VX_LOG_INFO("物件 [%s] 失去支撑（脚下地表 / 体积已被移除）⇒ 转动态刚体**掉落**",
                        slot.type->id.c_str());
        }
    }
}

/// `BlockCoord` 的坐标哈希（T79⑤）：三个整数分量各按大素数混合后再异或。
///
/// 为什么不用 `std::hash` 组合：块坐标只有 ±32 量级（1 km / 32 格），分量取值域很小，
/// 简单的"左移 + 异或"会在这类规整坐标上产生大量冲突（同一 tile 里的块会挤进同一个桶）。
/// 大素数乘法的散列质量对这种"规则网格"足够，且**确定性**（不含随机种子 ⇒ `unordered_map` 的
/// 桶序在同一进程内可复现；本表的迭代顺序另由稠密数组保证，见 `VolumeSlotTable`）。
struct BlockCoordHash {
    [[nodiscard]] std::size_t operator()(const vx::BlockCoord& coord) const noexcept {
        const std::size_t hx = static_cast<std::size_t>(static_cast<std::uint32_t>(coord.x)) * 73856093U;
        const std::size_t hy = static_cast<std::size_t>(static_cast<std::uint32_t>(coord.y)) * 19349663U;
        const std::size_t hz = static_cast<std::size_t>(static_cast<std::uint32_t>(coord.z)) * 83492791U;
        return hx ^ (hy << 1U) ^ (hz << 2U);
    }
};

/// 常驻体积块槽位表（T61 建立，T79⑤ 换实现）：**按坐标升序的稠密数组 + 坐标哈希索引**。
///
/// 为什么换掉 `std::map`（T77 清单第 14 条）：这张表的两个热点都是"每次查一个块坐标"——
///   ① `ResidentQuadFilter::SkipQuad` 对**每个地表四边形的 4 个角**各查一次（一次 tile 重网格 = 64×64×4 ≈ 1.6 万次）；
///   ② 每帧剔除对全部常驻块各查一次。
/// `std::map` 在这里是 O(log n) + 树节点指针追逐；换成 **O(1) 哈希查 + 连续内存**后两处都变便宜。
///
/// **迭代顺序不变**（关键，红线 7）：稠密数组恒按 `BlockCoord` 升序（插入用 `lower_bound` 保序），
/// 与 `std::map` 的遍历顺序**完全一致** ⇒ 剔除与提交顺序、以及所有以本表为输入的日志与统计
/// **逐位不变**。删除按数组搬移（O(n)，n = 常驻块数，本阶段 441）—— 与一次建块（≈22 ms）相比可忽略，
/// 且建 / 卸本来就按每帧 1 个动作推进（ADR 0020 决策四）。
class VolumeSlotTable {
public:
    using Entry     = std::pair<vx::BlockCoord, VolumeSlot>;
    using Container = std::vector<Entry>;
    using iterator  = Container::iterator;
    using const_iterator = Container::const_iterator;

    /// 查找：未命中返回 `end()`。O(1)（哈希）。
    [[nodiscard]] iterator Find(const vx::BlockCoord& coord) noexcept {
        const auto found = m_index.find(coord);
        return (found == m_index.end()) ? m_entries.end()
                                        : (m_entries.begin() + static_cast<std::ptrdiff_t>(found->second));
    }
    [[nodiscard]] const_iterator Find(const vx::BlockCoord& coord) const noexcept {
        const auto found = m_index.find(coord);
        return (found == m_index.end()) ? m_entries.end()
                                        : (m_entries.begin() + static_cast<std::ptrdiff_t>(found->second));
    }

    /// 是否常驻（`SkipQuad` 的判据；O(1)）。
    [[nodiscard]] bool Contains(const vx::BlockCoord& coord) const noexcept {
        return m_index.find(coord) != m_index.end();
    }

    /// **按坐标升序插入**（已存在则无操作）—— 表内**键存在 == 该块常驻**（T61 的唯一事实来源）。
    void Insert(const vx::BlockCoord& coord) {
        if (Contains(coord)) {
            return;
        }
        const auto position = std::lower_bound(m_entries.begin(), m_entries.end(), coord,
                                               [](const Entry& entry, const vx::BlockCoord& key) {
                                                   return entry.first < key;
                                               });
        const std::size_t index = static_cast<std::size_t>(position - m_entries.begin());
        m_entries.insert(position, Entry { coord, VolumeSlot {} });
        ReindexFrom(index);
    }

    /// 按坐标删除；未命中返回 false。
    bool Erase(const vx::BlockCoord& coord) {
        const auto found = m_index.find(coord);
        if (found == m_index.end()) {
            return false;
        }
        const std::size_t index = found->second;
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
        m_index.erase(found);
        ReindexFrom(index);
        return true;
    }

    [[nodiscard]] std::size_t Size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool        Empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] iterator       begin() noexcept { return m_entries.begin(); }
    [[nodiscard]] iterator       end() noexcept { return m_entries.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return m_entries.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_entries.end(); }

private:
    /// 从 `index` 起重建哈希索引（插入 / 删除后，其后每一项的下标都会平移）。
    void ReindexFrom(std::size_t index) {
        for (std::size_t i = index; i < m_entries.size(); ++i) {
            m_index[m_entries[i].first] = i;
        }
    }

    Container                                                     m_entries;  ///< 按坐标升序（= `std::map` 的遍历顺序）
    std::unordered_map<vx::BlockCoord, std::size_t, BlockCoordHash> m_index;   ///< 坐标 → 稠密下标
};

/// 世界列 / 高度 → 所属体积块坐标（块边长 32 格；向下取整，负坐标也正确）。
[[nodiscard]] vx::BlockCoord BlockOfWorldPoint(int worldX, int worldY, int worldZ) noexcept {
    const auto floorDiv = [](int value, int divisor) {
        const int quotient  = value / divisor;
        const int remainder = value % divisor;
        return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? (quotient - 1) : quotient;
    };
    return vx::BlockCoord { floorDiv(worldX, vx::kVolumeBlockSize), floorDiv(worldY, vx::kVolumeBlockSize),
                            floorDiv(worldZ, vx::kVolumeBlockSize) };
}

/// 上传（或重传）表里某个体积块的 GPU 网格；块不在表里 ⇒ 无操作。
void UploadVolumeMeshAt(VolumeSlotTable& slots, vx::MeshRenderer& renderer, const vx::DigVolumeWorld& volumes,
                        const vx::BlockCoord& coord) {
    const auto found = slots.Find(coord);  // T79⑤：O(1) 哈希查（原 `std::map::find`）
    if (found == slots.end()) {
        return;
    }
    UploadVolumeMesh(renderer, found->second.handle, volumes, coord, &found->second.bounds);
}

/// **层间交接过滤器 = 当前常驻集合**（T61 / [ADR 0020](../../docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策三）。
///
/// 判据与 `DigRegionTable::SkipQuad` **同形**（四角**全部**可挖才跳过地表四边形），但输入从
/// "静态标记区域"换成"**当前常驻**的体积块" ⇒ 窗口外的地表照旧由地表网格绘制，**不会出现空洞**。
class ResidentQuadFilter final : public vx::ITerrainQuadFilter {
public:
    explicit ResidentQuadFilter(const VolumeSlotTable& slots) noexcept : m_slots(slots) {}

    [[nodiscard]] bool SkipQuad(const vx::TerrainQuad& quad) const override {
        for (int corner = 0; corner < 4; ++corner) {
            const vx::BlockCoord block =
                BlockOfWorldPoint(quad.columnX[corner], static_cast<int>(std::floor(quad.height[corner])),
                                  quad.columnZ[corner]);
            if (!m_slots.Contains(block)) {
                return false;  // 有一角不常驻 ⇒ 地表照旧画（层间接管只发生在常驻集合内）
            }
        }
        return true;
    }

private:
    const VolumeSlotTable& m_slots;
};

/// W4：**地表壳区域的四边形过滤器** —— 四边形**四角全部**落在壳区域内时跳过（改由壳网格绘制）。
///
/// 判据与 `ResidentQuadFilter` / ADR 0011 **同形**；因为壳区域是**按 tile 对齐**的有界区域，
/// "被跳过的四边形"恰好整块落在若干 tile 内 ⇒ 这些 tile 的网格变空 ⇒ 既有的
/// `CollisionBodies` 阶段会**自动**把它们的高度场碰撞体交出去（见该阶段注释），无需另写一条接管路径。
class ShellQuadFilter final : public vx::ITerrainQuadFilter {
public:
    explicit ShellQuadFilter(const vx::SurfaceShellRegion& region) noexcept : m_region(region) {}

    [[nodiscard]] bool SkipQuad(const vx::TerrainQuad& quad) const override {
        for (int corner = 0; corner < 4; ++corner) {
            if (!m_region.ContainsColumn(quad.columnX[corner], quad.columnZ[corner])) {
                return false;  // 有一角在区域外 ⇒ 地表照旧画
            }
        }
        return true;
    }

private:
    const vx::SurfaceShellRegion& m_region;
};

/// 级联过滤器：任一子过滤器要求跳过即跳过（可挖体积接管 ∪ 地表壳接管）。
class CompositeQuadFilter final : public vx::ITerrainQuadFilter {
public:
    CompositeQuadFilter(const vx::ITerrainQuadFilter& first, const vx::ITerrainQuadFilter& second) noexcept
        : m_first(first), m_second(second) {}

    [[nodiscard]] bool SkipQuad(const vx::TerrainQuad& quad) const override {
        return m_first.SkipQuad(quad) || m_second.SkipQuad(quad);
    }

private:
    const vx::ITerrainQuadFilter& m_first;
    const vx::ITerrainQuadFilter& m_second;
};

/// W4：把一个**地表壳块**的网格登记为静态三角网碰撞体（[ADR 0012](../../docs/adr/0012-collision-takeover-by-volumes.md) 同构）。
///
/// 与渲染**共用同一份 `MeshData`** ⇒ "谁画谁挡"同源、不可能漂移。块内顶点是局部坐标，世界定位由
/// `origin*` 以 `double` 承担（红线 6）。空网格 = 无操作（该块没有可见表面）。
void AddShellBlockCollider(vx::PhysicsWorld& physics, const vx::MeshData& mesh, const vx::BlockCoord& block) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    // `MeshDesc` 需要"3 个 float / 顶点"的紧凑位置缓冲；`MeshVertex` 是交错布局 ⇒ 这里摊平一次。
    std::vector<float> positions;
    positions.reserve(mesh.vertices.size() * 3U);
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        positions.push_back(vertex.position[0]);
        positions.push_back(vertex.position[1]);
        positions.push_back(vertex.position[2]);
    }
    vx::PhysicsWorld::MeshDesc desc;
    desc.positions     = positions.data();
    desc.vertexCount   = mesh.vertices.size();
    desc.indices       = mesh.indices.data();
    desc.triangleCount = mesh.indices.size() / 3U;
    desc.originX       = static_cast<double>(vx::BlockOriginBlocks(block.x));
    desc.originY       = static_cast<double>(vx::BlockOriginBlocks(block.y));
    desc.originZ       = static_cast<double>(vx::BlockOriginBlocks(block.z));
    (void)physics.AddMesh(desc);
}

/// T27：爆炸写回世界所需的全部句柄与缓冲（避免十几个参数一路传下去）。
struct WorldEditContext {
    vx::TerrainWorld&                  world;
    vx::DigVolumeWorld&                volumes;
    vx::TerrainCollision&              collision;
    vx::VolumeCollision&               volumeCollision;  ///< T28：体积块的三角网碰撞体提供者
    const vx::CollapseSpec&            collapse;         ///< T29：塌落规则（来自 `collapse.toml`）
    const vx::DestructionSpec&         destruction;      ///< T31：伤害预算的换算系数（来自 `destruction.toml`）
    vx::MeshRenderer&                  renderer;
    const std::vector<vx::TileCoord>&  tileCoords;
    std::vector<vx::MeshHandle>&       tileHandles;
    VolumeSlotTable&                   volumeSlots;   ///< T61：**动态**常驻集合（按块坐标索引；含 GPU 网格与 AABB）
    std::vector<WorldAabb>&            tileBounds;    ///< T39：地表 tile 的世界 AABB（剔除用；上传时更新）
    vx::PendingDestruction&            pending;       ///< T37：延后破坏队列（爆炸只入队，重活按帧预算做）
    vx::PhysicsWorld&                  physics;       ///< T33：倒塌整体的动态刚体建在它上面
    vx::RigidCollapseRuntime&          rigidCollapse; ///< T33：倒塌整体的运行时（刚体 + 渲染网格池）
    std::size_t                        totalCollapseVoxels = 0;  ///< 累计失去支撑 / 回写的体素数（面板）
};

/// T27：一次爆炸的结果（供日志、面板与"角色被埋"救场判定使用）。
struct DetonationOutcome {
    bool        terrainChanged = false;  ///< 是否改动了**地表高度场**（区域外爆炸；需要重判角色是否被埋）
    std::size_t remeshed       = 0;      ///< 重网格的单元数（tile 或体积块）
    std::size_t collapsedVoxels = 0;     ///< T29：本次塌落移动的实心体素数
};

/// T27：在 `point` 处执行一次爆炸。分流（与 ADR 0004 的分层一致）：
///   - **点在可挖区域内** ⇒ 在体积里挖一个球腔 —— **这才是"从山的侧面射入 → 给山挖一个洞"**，
///     纯高度场在数学上做不到（无法表达横向洞穴）；区域内的地表网格本就由体积接管渲染（ADR 0011），
///     故**不动**高度场数据（那里的高度字段已不参与渲染与命中）；挖完再做一次**塌落**（T29 / ADR 0012）：
///     失去支撑的实心体沿本列下落成碎堆；
///   - **点在区域外** ⇒ 用爆破剖面挖地表坑（坑体 + 外环抛土），并重建受影响 tile 的碰撞体。
DetonationOutcome Detonate(WorldEditContext& context, const glm::dvec3& point, const vx::ProjectileSpec& spec) {
    DetonationOutcome outcome;

    if (context.volumes.IsInsideRegion(point.x, point.y, point.z)) {
        // T30：分段计时 —— 先量出真实的耗时分布，再据此优化。
        // T37 起当帧只做「挖除 + 塌落」两段，其余（重网格 / 上传 / 碰撞体重建）交给延后队列。
        PhaseTimer carveTimer;
        PhaseTimer collapseTimer;

        carveTimer.Begin();
        std::vector<vx::BlockCoord> dirty;
        vx::VoxelBounds              carved;  // T30：被改动采样的世界范围 —— 塌落邻域只围绕它展开
        // T31（ADR 0013 §二）：区域内的破坏改为**伤害预算**驱动 —— 预算 = damage × 换算系数（点），
        // 自爆心向外逐格³ 扣减该格材质的 toughness（软的先被挖掉、硬的留在原地）；
        // `explosionRadiusBlocks` 退化为**候选范围上界**（真正的挖除范围由预算决定）。
        const int budgetPoints = static_cast<int>(std::lround(
            static_cast<double>(spec.damage) * static_cast<double>(context.destruction.pointsPerCubicBlock)));
        if (!context.volumes.CarveByDamage(point, spec.explosionRadiusBlocks, budgetPoints, dirty, &carved)) {
            return outcome;  // 预算耗尽 / 范围内无可挖实心（空气或全不可破坏材质）⇒ 无改动
        }
        const double carveMs = carveTimer.EndMs();

        // T33：挖除后做一次「支撑检查 → 连通分量分组 → 抽出」，失去支撑的**每个整体**转成 Jolt 动态刚体。
        // 判据读的是**密度**（挖除阶段已写好），与网格化无关，故可以留在当帧；凸包构建与回写的成本见日志。
        collapseTimer.Begin();
        // T46（ADR 0017 决策四）：网格池被"**保留中的岩石残骸**"占满时先**腾位**（最旧优先惰性回写）——
        // 否则 `maxUnits = FreeSlots() = 0` ⇒ 本次爆炸**不抽出任何整体**（看起来就是"炸了却不塌"）。
        if (context.rigidCollapse.FreeSlots() == 0) {
            vx::CollapseWriteback retiredOldest;
            if (context.rigidCollapse.RetireOldestRetained(context.physics, context.volumes, context.renderer,
                                                           retiredOldest)) {
                dirty.insert(dirty.end(), retiredOldest.dirty.begin(), retiredOldest.dirty.end());
                VX_LOG_INFO("倒塌网格池已满 ⇒ 腾位：最旧的岩石残骸惰性体素化（T46）：回写 %zu 个（丢弃 %zu）",
                            retiredOldest.writtenVoxels, retiredOldest.droppedVoxels);
            }
        }
        // T43（ADR 0016 决策一）：把**爆心与爆炸半径**一并交给塌落 —— 失去支撑的整体由此获得"被炸飞"的
        // 冲量（逐体素向外、按距离线性衰减 ⇒ 越靠近爆心飞得越快，并因此产生翻滚角速度），
        // 而不再只靠人工倾斜方向。半径 = 本次爆炸半径（与挖除同一口径）。
        vx::CollapseSeed collapseSeed;
        collapseSeed.bounds    = carved;
        collapseSeed.epicenter = point;
        collapseSeed.radius    = spec.explosionRadiusBlocks;
        // T46：每次爆炸最多抽出几个整体仍由 `max_active_units` 控制（池有 16 槽，但一次爆炸不该全占了）。
        const std::size_t maxUnitsThisBlast =
            std::min(context.rigidCollapse.FreeSlots(), static_cast<std::size_t>(context.collapse.maxActiveUnits));
        vx::CollapsePlan collapsePlan = vx::ApplyCollapse(context.volumes, collapseSeed, context.collapse,
                                                         maxUnitsThisBlast);
        std::size_t spawnedUnits = 0;
        for (vx::CollapseUnit& unit : collapsePlan.units) {
            const std::size_t voxelCount = unit.voxels.size();
            // T46 起记录落地口径；T50 起每个整体都是**材质一致**的子块（判据 = 该子块的材质），
            // 表面材质直方图自此**只作诊断**（"玩家看到的皮是什么材质"），不再参与判定。
            const bool rigidDebris   = unit.rigidDebris;
            const int  shownRock     = unit.surfaceMaterialCounts[2];
            const int  shownDirt     = unit.surfaceMaterialCounts[1];
            const int  shownGrass    = unit.surfaceMaterialCounts[0];
            const int  shownSand     = unit.surfaceMaterialCounts[3];
            if (context.rigidCollapse.Spawn(context.physics, context.volumes, context.renderer, context.collapse,
                                            std::move(unit))) {
                ++spawnedUnits;
                VX_LOG_INFO("倒塌整体已刚体化（T33 / T50 子块）：体素 %zu 个 ⇒ 交给 Jolt 求解倾斜 / 旋转 / 碰撞；"
                            "落地口径 = %s（诊断：**表面**材质 cell 数 岩 %d / 土 %d / 草 %d / 沙 %d）",
                            voxelCount, rigidDebris ? "**保留几何体**（不回写，T46）" : "回写并与地面融合（T46）",
                            shownRock, shownDirt, shownGrass, shownSand);
            }
        }
        const double collapseMs = collapseTimer.EndMs();

        outcome.collapsedVoxels = collapsePlan.unsupportedVoxels;
        if (!collapsePlan.dirty.empty()) {
            dirty.insert(dirty.end(), collapsePlan.dirty.begin(), collapsePlan.dirty.end());
        }

        // T37：**重网格 / GPU 上传 / 三角网碰撞体重建**（实测每块 5~9 ms，一次爆炸波及 6~7 块 ≈ 100 ms）
        // 一律入队，由 `ProcessPendingDestruction` 按每帧预算推进 —— 当帧只付"挖除 + 抽出 + 刚体化"的成本。
        // 语义已确认：接受"全部脏块分帧"（洞口 / 倒塌残骸可在 1~N 帧内补齐）。
        outcome.remeshed = dirty.size();
        context.pending.MergeVolumeBlocks(dirty);

        VX_LOG_INFO("爆炸（当帧 = 挖除 + 抽出 + 刚体化）：中心 (%.1f, %.1f, %.1f) 半径 %.1f 格"
                    "（伤害 %.0f 点 ⇒ 预算 %d 点）；挖除 %.2f ms / 倒塌 %.2f ms"
                    "（邻域 %zu 采样 / 连通域 %zu 体素%s、失去支撑 %zu 体素、整体 %zu 个、已刚体化 %zu 个、因上限跳过 %zu 个、"
                    "清除小碎片 %zu 个 / %zu 体素；**保留中岩石残骸 %zu 个 / 自由槽位 %zu**）"
                    "⇒ 入队 %zu 个块的延后工作（重网格 + 碰撞体；队内现共 %zu 个单位）",
                    point.x, point.y, point.z, static_cast<double>(spec.explosionRadiusBlocks),
                    static_cast<double>(spec.damage), budgetPoints, carveMs, collapseMs,
                    collapsePlan.regionSamples, collapsePlan.domainVoxels,
                    collapsePlan.domainNarrowed ? "（已按连通域收窄，T49）" : "（**回退固定窗口**）",
                    collapsePlan.unsupportedVoxels, collapsePlan.units.size(),
                    spawnedUnits, collapsePlan.skippedUnits, collapsePlan.deletedUnits, collapsePlan.deletedVoxels,
                    context.rigidCollapse.RetainedUnits(), context.rigidCollapse.FreeSlots(),
                    dirty.size(), context.pending.PendingUnits());
        return outcome;
    }

    // T52（2026-09-28，项目所有者指定"岩石完全无法挖洞"）：**整片地表都是不可破坏材质 ⇒ 整坑不挖**。
    // 口径：区域内的不可破坏由体积侧的 `CarveByDamage` / 碎块补丁雕刻保证；区域外只有高度场，
    // 无法表达"只挖土不挖岩"，故这里做**整片判定**（坑覆盖到的每一列都不可破坏 ⇒ 不挖），
    // 而不是逐列过滤 —— 逐列过滤会在坑面上留下"柱子"、破坏 [ADR 0013](../docs/adr/0013-destructible-elements.md)
    // §二.5 的"边界一阶连续"。查不到材质的列（未覆盖 / 无地表）**不算**不可破坏（不替它做判定）。
    {
        const int radius = static_cast<int>(std::ceil(static_cast<double>(spec.explosionRadiusBlocks)));
        bool      anyDiggable = false;
        for (int dz = -radius; dz <= radius && !anyDiggable; ++dz) {
            for (int dx = -radius; dx <= radius && !anyDiggable; ++dx) {
                const double ox = static_cast<double>(dx) + 0.5;
                const double oz = static_cast<double>(dz) + 0.5;
                if (ox * ox + oz * oz > static_cast<double>(radius) * static_cast<double>(radius)) {
                    continue;  // 圆盘之外
                }
                std::uint8_t slot = vx::kNoMaterialSlot;
                if (!context.world.QueryDigMaterialSlot(static_cast<float>(point.x + dx),
                                                        static_cast<float>(point.z + dz), slot)) {
                    anyDiggable = true;
                    continue;
                }
                const bool indestructible =
                    context.volumes.Materials().Layer((slot == vx::kNoMaterialSlot) ? 0 : static_cast<int>(slot))
                        .indestructible;
                if (!indestructible) {
                    anyDiggable = true;
                }
            }
        }
        if (!anyDiggable) {
            VX_LOG_DEBUG("光球爆炸（地表爆破）：该片地表全是**不可破坏材质**（T52：岩石无法挖洞）⇒ 不挖；"
                         "中心 (%.1f, %.1f, %.1f)",
                         point.x, point.y, point.z);
            return outcome;
        }
    }

    vx::BrushPose brush;
    brush.centerX = static_cast<float>(point.x);
    brush.centerZ = static_cast<float>(point.z);
    brush.radius  = spec.explosionRadiusBlocks;

    const vx::BrushResult result = vx::ApplyTerrainCrater(context.world, brush, spec.explosionDepthBlocks,
                                                         spec.explosionRimBlocks, spec.explosionRadiusBlocks,
                                                         spec.explosionFalloff);
    if (result.changedColumns == 0) {
        return outcome;
    }

    outcome.terrainChanged = true;
    outcome.remeshed       = result.dirtyTiles.size();
    // T37：地表爆破的重网格 / 上传 / 碰撞体重建同样延后 —— `ApplyTerrainCrater` 只改高度列，本身很便宜。
    // "被体积接管的 tile 不重建高度场碰撞体"的既有判据（ADR 0012）由处理器的 `TileCollision` 分支保留。
    context.pending.MergeTiles(result.dirtyTiles);
    VX_LOG_DEBUG("光球爆炸（地表爆破）：中心 (%.1f, %.1f, %.1f)，坑半径 %.1f 格；改动 %zu 列 ⇒ 入队 %zu 个 tile 的"
                 "延后工作（重网格 + 碰撞体；队内现共 %zu 个单位）",
                 point.x, point.y, point.z, static_cast<double>(spec.explosionRadiusBlocks), result.changedColumns,
                 result.dirtyTiles.size(), context.pending.PendingUnits());
    return outcome;
}

/// T37：延后破坏工作的**执行器** —— 按每帧预算推进队列（SKILL「不冻结画面」）。
///
/// 顺序完全由 `PendingDestruction` 决定（先全部"重网格 + 上传"，再全部"重建碰撞体"），
/// 因此"分帧做完"与"当帧一次做完"的世界状态**逐位相同**（红线 7 / 11）。
struct DestructionProcessor {
    /// 处理至多 `budgetMs` 毫秒的延后工作，返回本帧处理的单位数（供面板显示"本帧重网格单元"）。
    ///
    /// **每帧至少处理一个单位**：单个单位（一个体积块的重网格 + 上传，或一次碰撞体重建）实测 5~9 ms，
    /// 可能超过预算，但让出本帧没有任何意义（进度会停），故"超预算也做一件"是刻意的取舍。
    std::size_t Process(WorldEditContext& context, double budgetMs) {
        if (context.pending.Empty()) {
            return 0;
        }
        vx::Clock                    clock;
        std::size_t                  processed = 0;
        vx::PendingDestruction::Unit unit;
        while (context.pending.TakeNext(unit)) {
            Apply(context, unit);
            ++processed;
            if (clock.Tick() * 1000.0 >= budgetMs) {
                break;
            }
        }
        if (context.pending.Empty()) {
            VX_LOG_DEBUG("延后破坏处理完成：本帧处理 %zu 个单位，队列已排空（洞口 / 塌落至此补齐）", processed);
        }
        return processed;
    }

private:
    void Apply(WorldEditContext& context, const vx::PendingDestruction::Unit& unit) {
        switch (unit.kind) {
            case vx::PendingDestruction::UnitKind::VolumeRemesh: {
                (void)context.volumes.RemeshBlock(unit.block);
                UploadVolumeMeshAt(context.volumeSlots, context.renderer, context.volumes, unit.block);
                break;
            }
            case vx::PendingDestruction::UnitKind::TileRemesh: {
                m_tileScratch.clear();
                m_tileScratch.push_back(unit.tile);
                (void)context.world.RemeshDirtyTiles(m_tileScratch);
                const std::size_t index = TileIndex(context, unit.tile);
                if (index < context.tileHandles.size()) {
                    UploadTileMesh(context.renderer, context.tileHandles[index], context.world, unit.tile,
                                   &context.tileBounds[index]);
                    // W7-S3b：上传后释放 CPU 侧网格（与常驻路径同口径）。
                    context.world.ReleaseTileMeshCpu(unit.tile.x, unit.tile.z);
                }
                break;
            }
            case vx::PendingDestruction::UnitKind::VolumeCollision:
                // T28：重建受影响块的三角网碰撞体 —— 不做这一步就会出现"看得见的新洞、走不进去"。
                (void)context.volumeCollision.SyncBlock(context.volumes, unit.block);
                // T46（ADR 0017 决策二）：**碰撞体一变，支撑条件就可能变了** —— 唤醒与它相交的保留岩石残骸。
                // 不唤醒的话，Jolt 的休眠体不会因"脚下静态形状被改写"自动醒来 ⇒ 岩石会悬空不动。
                context.rigidCollapse.AwakenIntersecting(unit.block, context.physics);
                break;
            case vx::PendingDestruction::UnitKind::TileCollision: {
                // 既有判据（ADR 0012）：**被体积接管的 tile 不重建高度场碰撞体**，
                // 否则会把刚交出去的隐形高度场又装回来（角色会被挡在自己挖的洞口外）。
                const vx::TerrainTileMesh* tileMesh = context.world.FindMesh(unit.tile.x, unit.tile.z);
                if (tileMesh == nullptr) {
                    break;
                }
                if (!tileMesh->meshEmpty) {
                    (void)context.collision.SyncTile(context.world, unit.tile.x, unit.tile.z);
                } else {
                    // T61：常驻集合随玩家移动 ⇒ **接管状态会翻转**（走远卸载 ⇒ 地表重新由高度场绘制）。
                    // 判据与启动期同源（"该 tile 的地表网格已无面"），故这里把高度场碰撞体**撤掉** ——
                    // 反方向（从"有面"到"无面"）也要走这一步，否则残留的隐形高度场会挡住新挖的洞口。
                    context.collision.RemoveTile(unit.tile.x, unit.tile.z);
                }
                break;
            }
        }
    }

    /// tile 坐标 → 句柄 / 包围盒数组下标；找不到返回"越界值"（调用方据此跳过）。
    [[nodiscard]] static std::size_t TileIndex(const WorldEditContext& context, const vx::TileCoord& tile) {
        for (std::size_t i = 0; i < context.tileCoords.size(); ++i) {
            if (context.tileCoords[i] == tile) {
                return i;
            }
        }
        return context.tileHandles.size();
    }

    /// 复用缓冲：单 tile 重网格需要一个单元素 vector，避免每单位分配一次（红线 10）。
    std::vector<vx::TileCoord> m_tileScratch;
};

/// 缺陷 B2 的物理侧（T27 复用）：**地表被抬高后**把被埋住的角色顶回地面。
///
/// Jolt 的静态高度场在 `SetShape` 之后不会把 `CharacterVirtual` 推出去：角色一旦被抬高的地表埋住，
/// 支撑判定失效，它会在重力下穿过高度场、带着相机钻到地下（画面只剩清屏色）。爆破的外环会抬高地形，
/// 故地表爆炸后必须做一次该检查（幂等：不满足"低于地表"时无副作用）。
void LiftCharacterIfBuried(vx::PhysicsWorld& physics, vx::PhysicsWorld::CharacterHandle character,
                           vx::ThirdPersonCamera& camera, const vx::TerrainWorld& world) {
    const vx::PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
    float                                    surface = 0.0F;
    if (!world.QueryHeight(static_cast<float>(state.position.x), static_cast<float>(state.position.z), surface) ||
        state.position.y >= static_cast<double>(surface)) {
        return;
    }
    const glm::dvec3 lifted(state.position.x, static_cast<double>(surface), state.position.z);
    physics.SetCharacterPosition(character, lifted);
    camera.SnapTo(glm::vec3(static_cast<float>(lifted.x), static_cast<float>(lifted.y), static_cast<float>(lifted.z)));
}

/// 应用帧率上限（T17）：目标 == 刷新率 → 垂直同步 + 关掉睡眠限帧；低于刷新率 → 睡眠限帧。
///
/// 立即生效（启动与滑块改动都走这里），并把"目标 / 方式 / 呈现模式 / 刷新率"记入日志。
void ApplyFrameRateCap(vx::Window& window, vx::FrameLimiter& limiter, int targetFps, int refreshRate) {
    const vx::FrameCapMode mode = vx::ChooseFrameCapMode(targetFps, refreshRate);
    if (mode == vx::FrameCapMode::VSync) {
        limiter.SetTargetFps(0);  // 垂直同步已限住，不再叠加睡眠
        (void)window.SetVSync(true);
        VX_LOG_INFO("帧率上限 → %d（垂直同步；呈现模式 %s，显示器刷新率 %d Hz）", targetFps,
                    window.PresentModeName(), refreshRate);
    } else {
        (void)window.SetVSync(false);  // 呈现模式交给低延迟的 MAILBOX / IMMEDIATE
        limiter.SetTargetFps(targetFps);
        VX_LOG_INFO("帧率上限 → %d（睡眠限帧；呈现模式 %s，显示器刷新率 %d Hz）", targetFps,
                    window.PresentModeName(), refreshRate);
    }
}

// ---------------------------------------------------------------
// 启动加载（T36 / SKILL「不冻结画面」）：窗口与加载画面尽早出现，长任务按帧切分
// ---------------------------------------------------------------

/// 加载期每帧的重计算预算（毫秒）。
///
/// 远小于一帧（60 Hz ≈ 16.7 ms）⇒ 加载画面以接近刷新率持续刷新，玩家看到的是"进度在走"
/// 而不是"画面停住"。这是 SKILL「不冻结画面」在启动加载上的量化落地：**不许**在渲染帧内
/// 同步跑完一个明显超一帧预算的任务。
constexpr double kLoadWorkBudgetMs = 8.0;

/// 加载阶段（顺序即**实际执行顺序**，也是进度顺序）。
enum class LoadStage : int {
    Textures = 0,     ///< 程序生成材质贴图 + 上传（最重，实测约 1.9 s）
    Environment,      ///< 环境贴图（T67）：HDRI 解码 + IBL 三件套烘焙（ADR 0021 实测约数百毫秒）
    TerrainTiles,     ///< 地表 tile 生成与网格化
    DiggableVolumes,  ///< 可挖体积密度填充与等值面网格化
    CollisionBodies,  ///< 地表高度场 / 可挖体积三角网碰撞体
    Finalize,         ///< 收尾（世界边界 / 出生点 / 角色 / 相机 / 系统面板数据 / 渲染原点）
    MeshUpload,       ///< 网格上传（tile + 体积 + 角色 + 光球）
    kCount
};

/// 各阶段的进度权重（无量纲，只需相对大小；量级取自 T36 的启动耗时分解，环境贴图按 ADR 0021 的预估量级）。
inline constexpr double kLoadStageWeights[static_cast<int>(LoadStage::kCount)] = { 1.9, 0.8, 2.0, 1.6, 1.0, 0.1, 0.6 };

/// 材质贴图分步生成的**每批行数**：256² 下单行约 0.8 ms，故 6 行 ≈ 5 ms，落在预算内。
constexpr std::size_t kTextureRowsPerSlice = 6;

/// 可挖体积分步初始化的**每批步数**：一步 = 一个块的密度填充或网格化（约 1~5 ms），故取 1。
constexpr std::size_t kVolumeInitStepsPerSlice = 1;

/// T61 / [ADR 0020](../../docs/adr/0020-dig-volume-vertical-band-and-dynamic-residency.md) 决策二：
/// 可挖体积**常驻窗口的半径**（单位：tile）。`2` ⇒ 5×5 tile ≈ 320 m 见方（所有者 2026-09-29 确认）。
/// 窗口之外只能炸地表坑、**挖不出三维洞**（ADR 0020 后果 1，切换条件已登记）。
constexpr int kDigVolumeWindowRadiusTiles = 2;

/// W4 / [ADR 0023](../../docs/adr/0023-world-representation-v2-hybrid-shell.md)：**地表壳的近场区域**
/// （tile 坐标，闭区间）。选在**与可挖区域不重叠**的位置（可挖区 = 中心 3×3 tile ≈ 列 `[-64, 160)`；
/// 本区 = 列 `[192, 384)`）⇒ 避免"可挖体积接管"与"地表壳接管"两套机制在同一处打架。
/// 列范围 = `[kShellTileMin * 64, (kShellTileMax + 1) * 64)`。
/// **全图铺开属 W7 流式**（ADR 0024），W4 只在近场落地。
constexpr int kShellTileMin = 3;
constexpr int kShellTileMax = 5;

/// T61：**每帧最多做几个"建块 / 卸块"动作**（与 `kVolumeInitStepsPerSlice` 同口径，一个动作 ≈ 1~5 ms）。
/// 新建块的**碰撞体**与因此受影响的 **tile 重网格**不在这里同步做 —— 它们入 `PendingDestruction` 队列，
/// 由既有的每帧预算（`kDestructionBudgetMs`）摊平（ADR 0020 决策四）。
constexpr std::size_t kVolumeResidencyActionsPerFrame = 1;

/// T81 / ADR 0022：每帧最多**安装**几个 worker 算好的块。
///
/// 安装本身很便宜（几次 `move` + 一次 GPU 网格上传 + 入延后队列），成本的大头是随后的上传与
/// 碰撞体 / tile 重网格（都走既有预算队列）⇒ 取 4 与 `kMeshUploadsPerSlice` 同口径。
/// 上界明确 ⇒ 主线程单帧成本有界（SKILL 第四节：新增每帧工作必须"有上界"）。
constexpr std::size_t kVolumeBuildsInstalledPerFrame = 4;

/// 网格上传的**每批个数**：单个网格的上传是一次阻塞拷贝，取 4 使其 ≲ 5 ms。
constexpr std::size_t kMeshUploadsPerSlice = 4;

/// W7-S3b：需要**高度场碰撞体**的最大 Chebyshev tile 距离（= Ring 0 外边界 + 预取环）。
///
/// 为什么收敛（而不是给每个常驻 tile 都建）：Jolt 的静态体随常驻量线性增长，4489 个高度场体
/// 在宽相位与内存上都不可接受；而**碰撞只在玩家附近有意义**（角色 / 弹道 / 爆炸都发生在近场）。
constexpr int kTerrainCollisionRadiusTiles = 9;

/// W7-S3b：**加载期**每个分片最多安装几个地表 tile。
/// 安装 = 预取缓存命中后只做 move + 过滤（生成 / 网格化已下沉 worker）⇒ 单帧成本很低；
/// 调大以缩短加载时间（仍是**有上界**的固定预算；上传另在 `MeshUpload` 阶段分片）。
constexpr std::size_t kTerrainTilesPerLoadSlice = 8;

/// W7-S3b：**运行期**每帧最多建 / 卸几个地表 tile（安装 = move + 过滤 + GPU 上传；**有上界**）。
constexpr std::size_t kTerrainResidencyActionsPerFrame = 4;

/// W7-S3b：**运行期**每帧最多提交几个 **relod** 重网格任务（提交廉价；网格化已下沉 worker）。
/// 无 worker（同步回退）时该预算会直接变成主线程重建次数 ⇒ 调用方按 `HasWorkers()` 收敛（见主循环）。
constexpr std::size_t kTerrainRelodPerFrame = 6;

/// W7-S3b：**预取提前量**上界（tile）= "已提交 + 已暂存但尚未安装"的总数上限。
/// 有界 ⇒ 预取缓存的内存有上界，且 worker 不会无限跑在安装之前（每帧成本只与窗口有关，ADR 0024 决策一）。
constexpr std::size_t kTerrainPrefetchLookahead = 192;

/// W7-S3b：**每帧最多提交**几个 worker 构建任务（提交本身廉价，但避免一次提交整窗造成单帧尖峰）。
constexpr std::size_t kTerrainPrefetchSubmitsPerFrame = 64;

/// W7-S3b：**每帧最多上传**几个 relod 重网格结果（上传是一次阻塞拷贝，有上界）。
constexpr std::size_t kTerrainRelodUploadsPerFrame = 4;

/// W7-S3b：**每帧最多建 / 撤几个地表 tile 的碰撞体**（Jolt 高度场体构建是毫秒级 ⇒ 必须分帧、有上界）。
constexpr std::size_t kTerrainCollisionActionsPerFrame = 2;

/// W7-S3b：tile 是否需要**高度场碰撞体**（Chebyshev 距离 <= `kTerrainCollisionRadiusTiles`）。
/// 距离口径与 LOD 分环**同源**（`TerrainTileWindow::TileDistanceFromCenter`）。
[[nodiscard]] bool NeedsTerrainCollision(const vx::TerrainTileWindow& window,
                                         const vx::TileCoord&         coord) noexcept {
    return window.TileDistanceFromCenter(coord) <= kTerrainCollisionRadiusTiles;
}

/// 阶段 → 加载画面上的文字标签（经 `UiText` 取值，故无 CJK 字体时也不会出现缺字）。
[[nodiscard]] vx::UiLabel LoadStageLabel(LoadStage stage) noexcept {
    switch (stage) {
        case LoadStage::Textures:
            return vx::UiLabel::LoadingStageTextures;
        case LoadStage::Environment:
            return vx::UiLabel::LoadingStageEnvironment;
        case LoadStage::TerrainTiles:
            return vx::UiLabel::LoadingStageTerrainTiles;
        case LoadStage::DiggableVolumes:
            return vx::UiLabel::LoadingStageDigVolumes;
        case LoadStage::CollisionBodies:
            return vx::UiLabel::LoadingStageCollision;
        case LoadStage::Finalize:
            return vx::UiLabel::LoadingStageFinalize;
        case LoadStage::MeshUpload:
            return vx::UiLabel::LoadingStageMeshUpload;
        case LoadStage::kCount:
            break;
    }
    return vx::UiLabel::LoadingTitle;
}

/// 加载画面控制器：在长任务之间**照常出帧**（窗口保持响应、进度持续刷新）。
///
/// 这是 SKILL「不冻结画面」在启动加载上的落地：先给画面（加载画面 + 真实进度），再给结果
/// （世界就绪后自然替换）。加载期只画叠加层、不提交任何世界网格 —— 材质纹理与相机尚未就绪。
struct LoadingScreen {
    vx::Window&       window;
    vx::MeshRenderer& renderer;
    vx::DebugOverlay& overlay;
    vx::InputMap&     input;  ///< 只作为 `pump_events` 的接收者；加载期不消费任何动作
    SDL_FColor        clearColor { 0.0F, 0.0F, 0.0F, 1.0F };
    bool              quitRequested = false;

    /// 出一帧加载画面（并处理本帧窗口事件）。返回 false 表示用户请求关闭窗口。
    [[nodiscard]] bool Pump(LoadStage stage, double stageFraction) {
        if (quitRequested) {
            return false;
        }
        if (!window.pump_events(input)) {
            quitRequested = true;
            return false;
        }
        input.BeginFrame();  // 与主循环同构：每帧只采样一次动作，避免加载期的按键边沿滞留到进入游戏后

        // 总进度 = 各阶段权重的前缀和 + 当前阶段的**加权部分进度**（只有一项分母，故只需相对大小）。
        const int    index        = static_cast<int>(stage);
        const double fraction     = std::clamp(stageFraction, 0.0, 1.0);
        double       weightsDone  = 0.0;
        double       weightsTotal = 0.0;
        for (int i = 0; i < static_cast<int>(LoadStage::kCount); ++i) {
            weightsTotal += kLoadStageWeights[i];
            if (i < index) {
                weightsDone += kLoadStageWeights[i];
            } else if (i == index) {
                weightsDone += kLoadStageWeights[i] * fraction;
            }
        }

        overlay.SetLoadingStatus(LoadStageLabel(stage), static_cast<float>(weightsDone / weightsTotal));
        overlay.BeginFrame();
        overlay.BuildLoadingUI();
        overlay.EndFrame();
        (void)renderer.RenderFrame(nullptr, 0, clearColor, &overlay);
        return true;
    }

    /// 在每帧 `kLoadWorkBudgetMs` 的预算内推进一个分片任务，预算用尽时出一帧加载画面。
    ///
    /// `step()` 处理一小批工作并返回**本阶段进度** `[0, 1]`（`1.0` = 完成）。每帧至少调用一次
    /// `step()`，因此进度必然单调前进；预算是"上限"而非"配额"。
    template <typename StepFn>
    [[nodiscard]] bool Run(LoadStage stage, StepFn&& step) {
        vx::Clock clock;
        while (true) {
            const double fraction = step();
            if (fraction >= 1.0) {
                return Pump(stage, 1.0);
            }
            if (clock.Tick() * 1000.0 >= kLoadWorkBudgetMs) {
                if (!Pump(stage, fraction)) {
                    return false;
                }
                (void)clock.Tick();  // 忘掉出加载画面的耗时，重新开始计预算
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    // T85：以 **UTF-8** 取回启动参数（Windows 经宽字符命令行还原，见 `platform/command_line.*`），
    // 解析测试模式（自动 / 人工 + 人工验收项）；**开关参数（`--` 前缀）绝不能当成位置参数**。
    const std::vector<std::string> arguments = vx::CommandLineArgumentsUtf8(argc, argv);
    const vx::TestModeInfo         testMode  = vx::ParseTestModeFromArguments(arguments);
    std::filesystem::path          shaderDir = ResolveShaderDir(argv[0]);
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        if (!vx::IsOptionArgument(arguments[i])) {
            shaderDir = std::filesystem::u8path(arguments[i]);
            break;
        }
    }

    // W7-S4：`--map=<相对仓库根路径>` 选择预设地图（缺省 = `kDefaultMapFile`）。
    // 为什么需要：10km 大世界（S4 的流式 / LOD 实测）与 1km 手工测试场（W4~W6 的目视验收）
    // 必须能共存，否则改默认地图就会让既有人工验收项失去场景。
    std::string mapFile = kDefaultMapFile;
    for (const std::string& argument : arguments) {
        constexpr const char* kMapPrefix = "--map=";
        if (argument.rfind(kMapPrefix, 0) == 0) {
            mapFile = argument.substr(std::char_traits<char>::length(kMapPrefix));
        }
    }

    // V1（ADR 0028 §一）：`--world=<裸 id 或 清单路径>` —— 从**世界清单**驱动启动。
    // 裸 id（如 `world_a`）⇒ `assets/maps/world_a.toml`；含路径分隔符或以 `.toml` 结尾 ⇒ 按仓库相对路径。
    // 未给出时仍走 `--map=`（回退，保持既有验收场景不变）。
    std::string worldManifestFile;
    for (const std::string& argument : arguments) {
        constexpr const char* kWorldPrefix = "--world=";
        if (argument.rfind(kWorldPrefix, 0) == 0) {
            worldManifestFile = argument.substr(std::char_traits<char>::length(kWorldPrefix));
        }
    }

    // W7-S4：`--autofly=<秒>` —— **确定性自动化飞行**（仅测试用；缺省 0 = 不启用）。
    // 为什么需要：W7 的验收判据要求"10km 飞越全图"的实测证据，而本环境无法用
    // `tools/vx_perf_input.ps1` 向游戏注入按键（注入只到达**前台**窗口，CI / 无头会话抢不到，
    // 见该脚本的 focus guard 说明）。本开关让"飞越"可脚本化复现，且**不影响任何缺省行为**。
    double autoFlySeconds = 0.0;
    for (const std::string& argument : arguments) {
        constexpr const char* kAutoFlyPrefix = "--autofly=";
        if (argument.rfind(kAutoFlyPrefix, 0) == 0) {
            try {
                autoFlySeconds = std::stod(argument.substr(std::char_traits<char>::length(kAutoFlyPrefix)));
            } catch (const std::exception&) {
                autoFlySeconds = 0.0;  // 非法值 = 关闭（测试开关，静默回退不改变玩法）
            }
        }
    }

    // V2b：`--switch-test=<世界 id>@<秒>`（**可重复给出**，按秒数先后触发）—— **确定性自动化世界切换**（仅测试用）。
    // 为什么需要：正式触发是 V3 的"走近交互点按 F"，但本环境无法向前台窗口注入按键（同 `--autofly` 的理由）
    // ⇒ 用"到点即请求切换"的测试开关，让"切换不冻结 / 有进度 / **卸载不留残** / 可复现"可被脚本化验证。
    // 秒数按**会话时钟**（跨世界累计的墙钟）计：`--switch-test=a@20 --switch-test=b@40` 即可连续切换两个世界。
    std::vector<std::pair<double, std::string>> switchTests;
    for (const std::string& argument : arguments) {
        constexpr const char* kSwitchPrefix = "--switch-test=";
        if (argument.rfind(kSwitchPrefix, 0) != 0) {
            continue;
        }
        const std::string value = argument.substr(std::char_traits<char>::length(kSwitchPrefix));
        const std::size_t at    = value.find('@');
        if (at == std::string::npos || at == 0U || at + 1U >= value.size()) {
            VX_LOG_WARN("`--switch-test` 取值非法（应为 <世界 id>@<秒>）：%s ⇒ 忽略", value.c_str());
            continue;
        }
        double seconds = 0.0;
        try {
            seconds = std::stod(value.substr(at + 1U));
        } catch (const std::exception&) {
            VX_LOG_WARN("`--switch-test` 的秒数非法：%s ⇒ 忽略", value.c_str());
            continue;
        }
        switchTests.emplace_back(seconds, value.substr(0U, at));
    }
    std::sort(switchTests.begin(), switchTests.end(),
              [](const std::pair<double, std::string>& left, const std::pair<double, std::string>& right) {
                  return left.first < right.first;
              });

    // T85：把测试模式打进日志（自动测试 ⇒ 勿动键鼠；人工测试 ⇒ 逐条列出验收项）。
    // 即便本机未加载 CJK 字体、面板不渲染非 ASCII 动态文本，日志里仍有完整信息。
    if (testMode.mode == vx::TestMode::Auto) {
        VX_LOG_INFO("测试模式：**自动测试**（--auto-test）—— 请勿操作键盘 / 鼠标；F1 面板显示对应横幅");
    } else if (!testMode.manualItems.empty()) {
        VX_LOG_INFO("测试模式：**人工测试** —— 本次需人工确认 %zu 项：", testMode.manualItems.size());
        for (const std::string& item : testMode.manualItems) {
            VX_LOG_INFO("  人工验收项：%s", item.c_str());
        }
    } else {
        VX_LOG_INFO("测试模式：**人工测试**（未提供 --manual-test 项；可用 --manual-test=\"项1;项2\" 指定）");
    }

    try {
        const vx::TerrainMaterialTable materials =
            vx::TerrainMaterialTable::LoadFromFile(SourceAssetPath("assets/config/materials.toml"));

        // W3：地表生成参数（含**地貌分区**：山川 / 平原 / 丘陵）。与材质表**同源解析**；
        // 加载失败（缺失 / 语法错 / 校验不过 / schema_version 不符）抛异常 → 启动失败（**禁止静默回退**）。
        // 离线烘焙（tools/baker）加载**同一份**文件 ⇒ 预制地图与游戏世界同源。
        const vx::TerrainGenerationParams terrainParams =
            vx::TerrainGenerationParams::LoadFromFile(SourceAssetPath("assets/config/terrain.toml"));
        VX_LOG_INFO("地表生成参数已加载：地貌分区 %s（掩罩频率 %.4f；平原/丘陵/山川 幅度 ×%.2f / ×%.2f / ×%.2f）",
                    terrainParams.landform.enabled ? "**启用**" : "关闭",
                    static_cast<double>(terrainParams.landform.frequency),
                    static_cast<double>(terrainParams.landform.plainsAmplitudeScale),
                    static_cast<double>(terrainParams.landform.hillsAmplitudeScale),
                    static_cast<double>(terrainParams.landform.mountainsAmplitudeScale));
        VX_LOG_INFO("洞穴网络（W5 地表壳）：%s（频率 %.4f；隧道半径 %.2f；最大雕刻 %.1f 格）",
                    terrainParams.caves.enabled ? "**启用**" : "关闭",
                    static_cast<double>(terrainParams.caves.frequency),
                    static_cast<double>(terrainParams.caves.tunnelRadius),
                    static_cast<double>(terrainParams.caves.carveStrengthBlocks));

        // T21a / T21c：光照与雾配置。与材质表**同源解析**（同一个 SourceAssetPath，同一个 toml++），
        // 加载失败（缺失 / 语法错 / 校验不过 / schema_version 不符）抛异常 → 外层 catch → 启动失败，
        // 与材质表口径一致：**禁止静默回退**。
        const vx::LightingTable lighting =
            vx::LightingTable::LoadFromFile(SourceAssetPath("assets/config/lighting.toml"));
        VX_LOG_INFO("光照配置已加载：太阳方向 (%.2f, %.2f, %.2f)（**由地表指向太阳**）强度 %.2f；"
                    "天空强度 %.2f；雾 %s（密度 %.4f /格，高度衰减 %.4f /格，雾色 (%.2f, %.2f, %.2f)）",
                    static_cast<double>(lighting.Sun().direction[0]),
                    static_cast<double>(lighting.Sun().direction[1]),
                    static_cast<double>(lighting.Sun().direction[2]), static_cast<double>(lighting.Sun().intensity),
                    static_cast<double>(lighting.Sky().intensity), lighting.Fog().enabled ? "启用" : "关闭",
                    static_cast<double>(lighting.Fog().density), static_cast<double>(lighting.Fog().heightFalloff),
                    static_cast<double>(lighting.Fog().color[0]), static_cast<double>(lighting.Fog().color[1]),
                    static_cast<double>(lighting.Fog().color[2]));

        // T21b：阴影配置日志（级数 / 分辨率 / 覆盖距离 / 显存预估与占比）。
        const vx::ShadowSettings& shadowSettings = lighting.Shadow();
        const double shadowMb = static_cast<double>(shadowSettings.cascadeCount) *
                                static_cast<double>(shadowSettings.resolution) *
                                static_cast<double>(shadowSettings.resolution) * 4.0 / (1024.0 * 1024.0);
        VX_LOG_INFO("阴影配置：%s（级数 %d，分辨率 %d²，覆盖 %.0f 格，split_lambda %.2f，"
                    "depth_bias %.4f，normal_offset %.3f 格，**投射体扩展下限 %.0f 格**，**级联混合 %.2f**）；"
                    "阴影图预估 %.2f MB（占 VRAM 预算 300 MB 的 %.1f%%）",
                    shadowSettings.enabled ? "启用" : "关闭", shadowSettings.cascadeCount, shadowSettings.resolution,
                    static_cast<double>(shadowSettings.maxDistance), static_cast<double>(shadowSettings.splitLambda),
                    static_cast<double>(shadowSettings.depthBias), static_cast<double>(shadowSettings.normalOffset),
                    static_cast<double>(shadowSettings.casterHeightMin), static_cast<double>(shadowSettings.cascadeBlend),
                    shadowMb, 100.0 * shadowMb / 300.0);

        // T8：**可挖区域标记表**（ADR 0006 的**数据文件**部分；程序化规则部分本轮未实现）。
        // 与其它配置表的唯一例外：**文件缺失返回空表、不报错**（ADR 0006 明确要求）；
        // 存在但解析 / 校验失败仍抛异常中止启动（禁止静默回退）。
        const vx::DigRegionTable digRegions =
            vx::DigRegionTable::LoadFromFile(SourceAssetPath("assets/config/dig_regions.toml"));
        if (digRegions.Empty()) {
            VX_LOG_INFO("可挖区域表为空：**本局没有任何可三维挖掘的区域** —— 光球命中只会在地表高度场上挖坑"
                        "（横向洞穴需要可挖区域，见 assets/config/dig_regions.toml）");
        } else {
            const double densityMb =
                static_cast<double>(digRegions.Blocks().size()) * 36.0 / 1024.0;  // 每块 33³ ≈ 36 KB（ADR 0008）
            VX_LOG_INFO("可挖区域表已加载（schema_version=%d）：%zu 个区域，共 %zu 个体积块（32³，密度数据约 %.2f MB）；"
                        "**竖向带宽**：地表以下 %d 格 / 地表以上 %d 格（0 = 不裁剪，T59 / ADR 0020）",
                        digRegions.SchemaVersion(), digRegions.Regions().size(), digRegions.Blocks().size(),
                        densityMb, digRegions.BandDownBlocks(), digRegions.BandUpBlocks());
            for (const vx::DigRegion& region : digRegions.Regions()) {
                VX_LOG_INFO("  区域 [%s]（%s，优先级 %d）：块 x∈[%d,%d] y∈[%d,%d] z∈[%d,%d] ⇒ 世界 x∈[%d,%d) y∈[%d,%d) "
                            "z∈[%d,%d)（min/max 已**向外吸附**到 32 格块边界）",
                            region.name.c_str(), region.diggable ? "diggable" : "sealed", region.priority,
                            region.blockMin.x, region.blockMax.x, region.blockMin.y, region.blockMax.y,
                            region.blockMin.z, region.blockMax.z, region.WorldMinX(), region.WorldMaxX(),
                            region.WorldMinY(), region.WorldMaxY(), region.WorldMinZ(), region.WorldMaxZ());
            }
        }

        // T27：弹丸规格表（光球）。与材质表 / 光照表**同源解析**；加载失败抛异常 → 启动失败。
        const vx::ProjectileTable projectiles =
            vx::ProjectileTable::LoadFromFile(SourceAssetPath("assets/config/projectiles.toml"));
        const vx::ProjectileSpec& orbSpec = projectiles.DefaultProjectile();
        VX_LOG_INFO("弹丸表已加载（schema_version=%d）：%zu 种弹丸，同时存在上限 %d；当前默认发射 [%s] —— "
                    "半径 %.2f 格 / 初速 %.1f 格-秒 / 重力系数 %.2f（角色重力 %.1f）/ 存活 %.1f 秒 / 射速冷却 %.2f 秒；"
                    "爆炸半径 %.1f 格（区域内三维挖除；区域外地表坑深 %.1f / 外环 %.1f / 衰减 %.2f）",
                    projectiles.SchemaVersion(), projectiles.Projectiles().size(), projectiles.MaxActive(),
                    orbSpec.id.c_str(), static_cast<double>(orbSpec.radius), static_cast<double>(orbSpec.speed),
                    static_cast<double>(orbSpec.gravityScale), static_cast<double>(kGravity),
                    static_cast<double>(orbSpec.lifetimeSeconds), static_cast<double>(orbSpec.fireIntervalSeconds),
                    static_cast<double>(orbSpec.explosionRadiusBlocks), static_cast<double>(orbSpec.explosionDepthBlocks),
                    static_cast<double>(orbSpec.explosionRimBlocks), static_cast<double>(orbSpec.explosionFalloff));

        // T29：塌落规则表（支撑检查与碎堆）。与其它配置表同源解析；加载失败抛异常 → 启动失败。
        const vx::CollapseTable collapseTable =
            vx::CollapseTable::LoadFromFile(SourceAssetPath("assets/config/collapse.toml"));
        const vx::CollapseSpec& collapseSpec = collapseTable.Spec();
        VX_LOG_INFO("倒塌规则已加载（schema_version=%d，T33 刚体化 / T43 真实感）：%s；悬挑上限 %.1f 格"
                    "（跨度超过即失去支撑）；邻域外扩 %d 块；落定阈值 %.2f 格-秒 / %.2f rad-秒 × %d 步；"
                    "爆心冲量 %.2f 格-秒（无冲量时退回人工不对称 %.2f rad-秒）；小碎片清除阈值 %d 体素；"
                    "活跃整体上限 %d（质量 / 摩擦 / 弹性改由材质表驱动，见 ADR 0016）",
                    collapseTable.SchemaVersion(), collapseSpec.enabled ? "启用" : "关闭",
                    static_cast<double>(collapseSpec.maxCantileverBlocks), collapseSpec.neighborhoodMarginBlocks,
                    static_cast<double>(collapseSpec.settleLinearSpeed),
                    static_cast<double>(collapseSpec.settleAngularSpeed), collapseSpec.settleSteps,
                    static_cast<double>(collapseSpec.impulseSpeed), static_cast<double>(collapseSpec.initialTiltSpeed),
                    collapseSpec.debrisDeleteMaxVoxels, collapseSpec.maxActiveUnits);

        // T31：全局破坏表（伤害预算的换算系数；器物字段待层 ③ 消费，见 ADR 0013）。
        const vx::DestructionTable destructionTable =
            vx::DestructionTable::LoadFromFile(SourceAssetPath("assets/config/destruction.toml"));
        const vx::DestructionSpec& destructionSpec = destructionTable.Spec();
        VX_LOG_INFO("破坏表已加载（schema_version=%d，T31 / ADR 0013）：换算系数 %.1f（预算 = 弹丸 damage × 本值，点）；"
                    "器物阈值 %.1f 点 / 破坏占位色 (%.2f, %.2f, %.2f)（**待层 ③ 消费**）",
                    destructionTable.SchemaVersion(), static_cast<double>(destructionSpec.pointsPerCubicBlock),
                    static_cast<double>(destructionSpec.propDamageThreshold),
                    static_cast<double>(destructionSpec.propBrokenTint[0]),
                    static_cast<double>(destructionSpec.propBrokenTint[1]),
                    static_cast<double>(destructionSpec.propBrokenTint[2]));

        // T11：从预设地图构建世界（种子 / 范围 / 地形编辑全部来自文件，不再硬编码）。
        // V1：`--world=` 时地形由**世界清单**（LevelManifest）引用的地形预设给出 ——
        //     清单只声明族 / 来源 / 策略，地形字段的**单一事实来源**仍是那份 `MapPreset`。
        // V2a：清单先进 `WorldManager`（**注册表 + 当前世界 + 切换请求状态机**，见 plans/v0.5.md §1.7）；
        //     `--world=<裸 id>` 走注册表（未知 id 直接报错并列出已注册的世界），`--world=<路径>` 注册该文件。
        const std::filesystem::path mapPath = SourceAssetPath(mapFile);
        vx::MapPreset               preset;
        std::string                 presetSourceLabel = mapPath.string();  // 日志用：地形实际来自哪里
        vx::WorldManager            worldManager;
        if (!worldManifestFile.empty()) {
            if (WorldArgumentLooksLikePath(worldManifestFile)) {
                // 路径形式：注册这一份并激活（`Register` 返回其 id）。
                worldManager.SetActive(worldManager.Register(SourceAssetPath(worldManifestFile)));
            } else {
                worldManager.RegisterAll({ SourceAssetPath("assets/maps/world_a.toml"),
                                           SourceAssetPath("assets/maps/world_b.toml"),
                                           SourceAssetPath("assets/maps/world_c.toml") });
                worldManager.SetActive(worldManifestFile);  // 未知 id ⇒ 抛（阶段计划 §1.7 的验收判据）
            }

            const vx::LevelManifest& manifest = worldManager.Active();
            preset                            = manifest.terrain;
            presetSourceLabel                 = "清单 " + ResolveWorldManifestPath(worldManifestFile).string();
            // V4（ADR 0026）：`source = premade` 的**预制文件不在启动期打开**（此处只做清单注册与日志）；
            // 实际的"打开 + 校验 + 注入地表数据源"发生在**每轮世界装载**时（见世界装载循环里的
            // `PremadeTerrainTileSource` 段）——这样切换世界也会按各自清单重新打开自己的预制文件。
            VX_LOG_INFO("世界清单（V1/V2a）：id=%s 名称=\"%s\" 族=%s 来源=%s；破坏=%s 持久化=%s 换种子=%s；"
                        "清单=%s 地形预设=%s（半径 %d×%d、种子 %llu、出生 (%.1f, %.1f)）；注册表 %zu 个世界",
                        manifest.id.c_str(), manifest.name.c_str(), vx::ToString(manifest.family),
                        vx::ToString(manifest.source), manifest.destructionEnabled ? "开" : "关",
                        manifest.persistent ? "是" : "否", manifest.randomizeSeedOnEntry ? "是" : "否",
                        ResolveWorldManifestPath(worldManifestFile).string().c_str(),
                        manifest.terrainPresetPath.c_str(), preset.tileRadiusX, preset.tileRadiusZ,
                        static_cast<unsigned long long>(preset.seed), preset.spawnX, preset.spawnZ,
                        worldManager.Count());
        } else {
            preset = vx::MapPreset::LoadFromFile(mapPath);
        }

        vx::Window window("Voxel Engine - V0.1 terrain", 1280, 720);

        // T15：系统设置（TOML，复用 toml++，见 ADR 0005）。必须在创建窗口之后读取——
        // 设置路径取自 `SDL_GetPrefPath`（需要 SDL 已初始化）。文件缺失 → 默认值（不报错）；
        // 存在但非法 → 明确抛错（被外层 try 捕获并报出，不静默回退）。
        const std::filesystem::path settingsPath = vx::SystemSettingsPath();
        VX_LOG_INFO("设置文件：%s", settingsPath.string().c_str());
        vx::SystemSettings systemSettings = vx::LoadSystemSettings(settingsPath);

        // T17：查询当前窗口所在显示器的刷新率（平台层是唯一 SDL 入口；未知 / 取不到时回退 60 Hz），
        // 并据它把**存储的**帧率上限重新钳制——换显示器 / 改刷新率后不会留下超出范围的非法值。
        const int displayRefreshRate = window.DisplayRefreshRate();
        systemSettings.frameRateCap = vx::ResolveFrameRateCap(systemSettings.frameRateCap, displayRefreshRate);

        VX_LOG_INFO("已加载设置：显示模式=%s，分辨率=%d x %d，主音量=%d，帧率上限=%d Hz，曝光=%.2f，MSAA=%d×",
                    (systemSettings.displayMode == vx::DisplayMode::Fullscreen) ? "fullscreen" : "windowed",
                    systemSettings.windowWidth, systemSettings.windowHeight, systemSettings.masterVolume,
                    systemSettings.frameRateCap, static_cast<double>(systemSettings.exposure),
                    systemSettings.msaaSamples);
        VX_LOG_INFO("显示器刷新率：%d Hz（未知时回退 %d Hz）", displayRefreshRate, vx::kFallbackRefreshRate);

        // ---- V10：读**秘境存档槽**（单槽自动；只存秘境绑定，见 plans/v0.5.md §1.13.1）----
        // 走 `--map=` 时注册表为空 ⇒ 无秘境可恢复，跳过（也避免把未知 id 的旧档刷成 WARN）。
        // 读档失败（语法错 / 未知版本）⇒ **抛**（被外层捕获并报出，不静默误读）——与 `settings.toml` 同口径。
        const std::filesystem::path worldInstanceSavePath = WorldInstanceSavePath();
        if (worldManager.Count() > 0U) {
            std::error_code mkdirError;
            std::filesystem::create_directories(worldInstanceSavePath.parent_path(), mkdirError);
            if (mkdirError) {
                // 不静默：目录建不出来 ⇒ 后续写入会失败（只 WARN，不影响本次游戏）。
                VX_LOG_WARN("秘境存档（V10）：无法创建目录 %s —— %s（写入将失败）",
                            worldInstanceSavePath.parent_path().string().c_str(), mkdirError.message().c_str());
            }
            const vx::WorldInstanceSave save = vx::LoadWorldInstanceSave(worldInstanceSavePath);  // 无档 ⇒ 空（不报错）
            std::size_t                 restored = 0;
            std::size_t                 skipped  = 0;
            for (const vx::SavedWorldInstance& record : save.instances) {
                std::string reason;
                if (worldManager.RestoreInstance(record.worldId, record.seed, record.generation, reason)) {
                    ++restored;
                    VX_LOG_INFO("秘境存档（V10）：恢复 [%s] 的绑定 —— 实例种子 %llu（第 %u 次生成）",
                                record.worldId.c_str(), static_cast<unsigned long long>(record.seed),
                                record.generation + 1U);
                } else {
                    ++skipped;
                    VX_LOG_WARN("秘境存档（V10）：跳过 [%s] —— %s（世界清单已变？）", record.worldId.c_str(),
                                reason.c_str());
                }
            }
            VX_LOG_INFO("秘境存档（V10）：%s；恢复 %zu 个实例绑定%s —— **跨启动保持**（重启仍进入同一个秘境）",
                        worldInstanceSavePath.string().c_str(), restored,
                        (skipped == 0U) ? "" : "（有跳过，见上）");
        }

        /// V10：把当前秘境实例账本**写回**存档槽（单槽；**原子替换**）。失败只 WARN —— **不影响游戏**
        /// （下次变更再试）；属**极低频非热路径**写入（首次进入 / 重置），与 `settings.toml` 同口径。
        const auto persistWorldInstances = [&worldManager, &worldInstanceSavePath](const char* trigger) {
            const vx::WorldInstanceSave save = BuildWorldInstanceSave(worldManager);
            try {
                vx::SaveWorldInstanceSave(worldInstanceSavePath, save);
                VX_LOG_INFO("秘境存档（V10）：已写入 %s（%zu 个实例；触发：%s）",
                            worldInstanceSavePath.string().c_str(), save.instances.size(), trigger);
            } catch (const std::exception& error) {
                VX_LOG_WARN("秘境存档（V10）：写入失败（触发：%s）⇒ 本次未持久化、下次变更再试：%s", trigger,
                            error.what());
            }
        };

        // 应用设置：窗口模式恢复客户区尺寸；全屏走桌面无边框全屏。
        if (vx::IsResolutionEditable(systemSettings.displayMode)) {
            (void)window.SetWindowSize(systemSettings.windowWidth, systemSettings.windowHeight);
        } else {
            (void)window.SetFullscreen(true);
        }
        vx::ApplyMasterVolumeGain(systemSettings.masterVolume);

        // T17：帧率限速的目标在**加载结束后**才生效（见下）；加载期先关掉垂直同步与睡眠限帧 ——
        // 否则每出一帧加载画面都要等一个垂直同步间隔，7 s 左右的重计算会被放大成十几秒。
        vx::FrameLimiter frameLimiter;
        (void)window.SetVSync(false);
        frameLimiter.SetTargetFps(0);
        VX_LOG_INFO("加载期呈现模式 → %s（不做垂直同步 / 限帧，让加载画面以工作节奏刷新；"
                    "世界就绪后恢复为 %d Hz 的目标）",
                    window.PresentModeName(), systemSettings.frameRateCap);

        // T36 / SKILL「不冻结画面」：**窗口、渲染器、ImGui 面板全部前置到这里**，之后所有长任务都
        // 在加载画面的帧之间分步推进。此前这些资源在重计算之后才创建，导致启动期只能黑屏干等。
        vx::MeshRenderer renderer(window.device(), window.handle(), shaderDir);

        // T20 / ADR 0010：把配置里的曝光交给色调映射通道（参数进配置，改值不需重编 Shader）。
        renderer.SetExposure(systemSettings.exposure);

        // T23 / ADR 0010 P3：把配置里的 MSAA 档位交给渲染器（引擎层不读配置文件；档位 = 1 时零额外开销）。
        renderer.SetMsaaSampleCount(static_cast<std::uint32_t>(systemSettings.msaaSamples));

        // 调试面板（T9）：基于 imgui 的 SDL3 + SDL3_gpu 后端；F1 开关。
        // T15：系统面板与它共用同一个 ImGui 上下文（由本对象托管），并通过事件转发变为**可交互**。
        // 它同时是**加载画面**的绘制者，故必须先于任何长任务建立。
        vx::DebugOverlay debugOverlay(window.device(), window.handle());
        debugOverlay.SetTestMode(testMode);  // T85：F1 面板顶部的测试模式横幅（自动 / 人工 + 验收项）

        // T15：把 SDL 事件转发给 ImGui 后端（全生命周期只安装一次）。平台层仍是唯一读事件队列的地方，
        // game/ 只是注册回调；未接这一步之前面板只读，正是因为事件从未到达 ImGui。
        window.SetEventCallback(&vx::DebugOverlay::OnSdlEvent, &debugOverlay);

        // T20 / T21a：主通道写 HDR 目标，清屏色须按**线性光**给出（色调映射通道最后编码到 sRGB）。
        // T21a 起不再写死：取配置里的**天空地平色**（sRGB 作者色 → 线性）。为什么是地平色而不是天顶色：
        // 清屏色就是"没有几何处的天空背景"，而远景地形会被雾混向**地平色**
        // （fog.color 缺失时默认 = sky.horizon_color），两者同源才能让远景与天空无缝衔接、无硬边。
        const vx::ColorRgb clearLinear = vx::SrgbToLinear(lighting.Sky().horizonColor);
        const SDL_FColor   clearColor { clearLinear[0], clearLinear[1], clearLinear[2], 1.0F };

        // 输入绑定（动作名 → 按键）。加载期只被 `pump_events` 填事件、不消费动作。
        vx::InputMap input;
        input.BindKey(vx::ActionId::MoveForward, SDL_SCANCODE_W);
        input.BindKey(vx::ActionId::MoveBackward, SDL_SCANCODE_S);
        input.BindKey(vx::ActionId::MoveLeft, SDL_SCANCODE_A);
        input.BindKey(vx::ActionId::MoveRight, SDL_SCANCODE_D);
        input.BindKey(vx::ActionId::Jump, SDL_SCANCODE_SPACE);
        input.BindKey(vx::ActionId::Sprint, SDL_SCANCODE_LSHIFT);
        input.BindKey(vx::ActionId::ToggleDebugPanel, SDL_SCANCODE_F1);
        input.BindKey(vx::ActionId::ToggleFly, SDL_SCANCODE_F);         // T12：飞行模式开关
        input.BindKey(vx::ActionId::FlyDown, SDL_SCANCODE_LCTRL);       // T12：飞行时下降
        input.BindKey(vx::ActionId::ToggleSystemPanel, SDL_SCANCODE_ESCAPE);  // T15：Esc 开关系统面板（语义已统一）
        input.BindKey(vx::ActionId::Interact, SDL_SCANCODE_E);                // V3：走近传送门按 E 触发切换
        input.BindMouseButton(vx::ActionId::Attack, SDL_BUTTON_LEFT);   // T27：左键 = 发射光球
        input.BindMouseAxis(vx::ActionId::LookX, vx::MouseAxis::X);
        input.BindMouseAxis(vx::ActionId::LookY, vx::MouseAxis::Y);

        LoadingScreen loading { window, renderer, debugOverlay, input, clearColor };

        // ---- 阶段 1：材质贴图（★ T66：**真实 CC0 资源优先，缺失则回落程序生成**）----
        //
        // 两种模式（语义差异见 `materials.toml` 的 `[textures]` 与 `mesh.frag` 的 `textureMode`）：
        //   ① **真实贴图**：`[textures].enabled = true` 且四层 × 四件套齐备 ⇒ 上传 1024² 真实资源
        //      （贴图是**绝对值**：粗糙度 / AO 直取贴图；层色 tint 由 CPU 置 1）。
        //   ② **程序生成（回落路径）**：缺任一资源 / 未启用 ⇒ **WARN + 程序生成占位贴图**（不静默、不崩）。
        // 两条路径都**分步推进**（解码 16 张 2048² JPEG + 降采样要数秒），期间照常出加载画面。
        bool                        realTextureMode = false;
        vx::MaterialTextureAssetSet materialAssets;
        if (materials.Textures().enabled) {
            // 资源**不入库**（所有者 2026-09-29 裁定，见 docs/plans/v0.3.md §1.1）：干净克隆下这里必然失败 ⇒
            // 只 WARN 并回落，不报错、不拦启动。
            vx::MaterialTextureAssetLoader assetLoader(
                materials,
                vx::MaterialTextureAssetSpec { SourceAssetPath(materials.Textures().root), materials.Textures().size });
            if (!loading.Run(LoadStage::Textures, [&assetLoader]() {
                    return assetLoader.Step(/*maxMaps=*/1) ? 1.0 : static_cast<double>(assetLoader.Progress());
                })) {
                VX_LOG_INFO("加载期收到退出请求（材质贴图阶段），退出");
                return EXIT_SUCCESS;
            }
            if (assetLoader.Failed()) {
                VX_LOG_WARN("真实美术贴图不可用 ⇒ **回落程序生成占位贴图**（不静默）：%s；"
                            "取资源：powershell -ExecutionPolicy Bypass -File tools\\fetch_assets.ps1"
                            "（来源 / 许可 / SHA-256 见 NOTICE.md「美术资源台账」）",
                            assetLoader.Reason().c_str());
            } else {
                materialAssets   = assetLoader.Take();
                realTextureMode  = true;
                VX_LOG_INFO("**真实美术贴图已加载**：%u² × %u 层 × [albedo / normal / roughness / AO]；"
                            "源图 %u² ⇒ 整数倍 box 降采样（确定性）；来源 / 许可 / SHA-256 见 NOTICE.md 台账；"
                            "**宏观变化改用该层 albedo 放大采样**（真实资源无 macro 图，不额外占显存）",
                            materialAssets.size, materialAssets.layerCount, materialAssets.sourceSize[0]);
            }
        } else {
            VX_LOG_INFO("材质表未启用 `[textures]`（或 `enabled = false`）⇒ 使用**程序生成**占位贴图");
        }

        if (!realTextureMode) {
            // T22 / ADR 0010 P2：程序生成的占位材质贴图（多尺度 albedo / 法线 + roughness + AO + 宏观变化），
            // 上传为五个 2D 纹理数组（albedo/normal/roughness/AO 各 4 层，macro 1 层），供片元着色器逐像素混合
            // 并做 PBR。无二进制资产、种子确定性。显存由 CreateTextureArray 自动计入 RenderStats::textureBytes。
            //
            // T36：一次跑完约 1.9 s，是启动期最重的一步 ⇒ 改为**分步**（每次几个像素行），期间照常出加载画面。
            // 分步与一次性生成**逐字节相同**（红线 7：只改变"何时可见"）。
            vx::MaterialTextureBuilder textureBuilder(preset.seed);
            if (!loading.Run(LoadStage::Textures, [&textureBuilder]() {
                    return textureBuilder.Step(kTextureRowsPerSlice)
                               ? 1.0
                               : static_cast<double>(textureBuilder.Progress());
                })) {
                VX_LOG_INFO("加载期收到退出请求（材质贴图阶段），退出");
                return EXIT_SUCCESS;
            }
            const vx::MaterialTextureSet materialTextures = textureBuilder.Take();
            const vx::TextureArrayDesc   albedoDesc { materialTextures.size, materialTextures.size,
                                                    materialTextures.layerCount, materialTextures.albedoRgba.data() };
            const vx::TextureArrayDesc   normalDesc { materialTextures.size, materialTextures.size,
                                                    materialTextures.layerCount, materialTextures.normalRgba.data() };
            const vx::TextureArrayDesc   roughnessDesc { materialTextures.size, materialTextures.size,
                                                       materialTextures.layerCount, materialTextures.roughnessRgba.data() };
            const vx::TextureArrayDesc   aoDesc { materialTextures.size, materialTextures.size,
                                                materialTextures.layerCount, materialTextures.aoRgba.data() };
            const vx::TextureArrayDesc   macroDesc { materialTextures.size, materialTextures.size,
                                                   vx::kMaterialMacroLayerCount, materialTextures.macroRgba.data() };
            const vx::TextureArrayHandle albedoTexture    = renderer.CreateTextureArray(albedoDesc);
            const vx::TextureArrayHandle normalTexture    = renderer.CreateTextureArray(normalDesc);
            const vx::TextureArrayHandle roughnessTexture = renderer.CreateTextureArray(roughnessDesc);
            const vx::TextureArrayHandle aoTexture        = renderer.CreateTextureArray(aoDesc);
            const vx::TextureArrayHandle macroTexture     = renderer.CreateTextureArray(macroDesc);
            renderer.SetSampledTextureArrays(albedoTexture, normalTexture, roughnessTexture, aoTexture, macroTexture);

            // 材质数组总量 = 每层第 0 级字节 × 4/3（mip）× 总层数；层数 = 4×4 + 1（macro）。
            const double mipFactor       = 4.0 / 3.0;
            const double baseBytes       = static_cast<double>(materialTextures.size) * materialTextures.size * 4.0;
            const double totalLayerCount = static_cast<double>(materialTextures.layerCount) * 4.0 +
                                           static_cast<double>(vx::kMaterialMacroLayerCount);
            VX_LOG_INFO("材质贴图已生成并上传：%u×%u，R8G8B8A8_UNORM；albedo/normal/roughness/AO 各 %u 层 + macro %u 层，"
                        "含 mip 约 %.2f MB 显存（第 0 级 %.2f MB/层）",
                        materialTextures.size, materialTextures.size, materialTextures.layerCount,
                        vx::kMaterialMacroLayerCount, baseBytes * mipFactor * totalLayerCount / (1024.0 * 1024.0),
                        baseBytes / (1024.0 * 1024.0));
        } else {
            // 真实贴图：四张数组（无 macro 资源 ⇒ macro 槽位**绑 albedo 数组**，着色器按宏观尺度采样该层 albedo）。
            const vx::TextureArrayDesc albedoDesc { materialAssets.size, materialAssets.size, materialAssets.layerCount,
                                                    materialAssets.albedoRgba.data() };
            const vx::TextureArrayDesc normalDesc { materialAssets.size, materialAssets.size, materialAssets.layerCount,
                                                    materialAssets.normalRgba.data() };
            const vx::TextureArrayDesc roughnessDesc { materialAssets.size, materialAssets.size,
                                                       materialAssets.layerCount, materialAssets.roughnessRgba.data() };
            const vx::TextureArrayDesc aoDesc { materialAssets.size, materialAssets.size, materialAssets.layerCount,
                                                materialAssets.aoRgba.data() };
            const vx::TextureArrayHandle albedoTexture    = renderer.CreateTextureArray(albedoDesc);
            const vx::TextureArrayHandle normalTexture    = renderer.CreateTextureArray(normalDesc);
            const vx::TextureArrayHandle roughnessTexture = renderer.CreateTextureArray(roughnessDesc);
            const vx::TextureArrayHandle aoTexture        = renderer.CreateTextureArray(aoDesc);
            renderer.SetSampledTextureArrays(albedoTexture, normalTexture, roughnessTexture, aoTexture,
                                             /*macro=*/albedoTexture);

            const double mipFactor = 4.0 / 3.0;
            const double baseBytes = static_cast<double>(materialAssets.size) * materialAssets.size * 4.0;
            VX_LOG_INFO("真实材质贴图已上传：%u×%u，R8G8B8A8_UNORM；四件套各 %u 层（**无 macro 数组**：macro 槽位绑 albedo），"
                        "含 mip 约 %.2f MB 显存（第 0 级 %.2f MB/层）",
                        materialAssets.size, materialAssets.size, materialAssets.layerCount,
                        baseBytes * mipFactor * 4.0 * static_cast<double>(materialAssets.layerCount) / (1024.0 * 1024.0),
                        baseBytes / (1024.0 * 1024.0));
        }

        // ---- 阶段 2：环境贴图（★ T67 / ADR 0021：天空 HDRI + IBL 三件套）----
        //
        // 语义：把"环境光"从**半球天空光的常量近似**换成**与画面里的天空同源**的 HDRI 光照
        // （漫反射 irradiance + 预过滤高光 + BRDF LUT —— 全部在加载期烘焙、运行期只采样）。
        // 资源**不入库**（所有者 2026-09-29 裁定，同 T66）：干净克隆 / 离线环境必然没有 HDRI ⇒
        // 只 `WARN` 并**回落**半球天空光（不静默、不崩）。
        //
        // 为什么分两步（解码 → 烘焙）交给 `loading.Run`：解码 2048×1024 的 `.hdr` 约 0.1 s、
        // 三条烘焙 pass 约数百毫秒（ADR 0021 后果 2）—— 之间照常出一帧加载画面（SKILL「不冻结画面」）。
        bool environmentIblReady = false;
        if (lighting.Environment().enabled) {
            const std::filesystem::path hdriPath = SourceAssetPath(lighting.Environment().hdri);
            vx::ImageRgb32f             hdri;
            std::string                 environmentFailure;

            int        step     = 0;
            const bool advanced = loading.Run(LoadStage::Environment, [&]() {
                if (step == 0) {
                    // 解码在 CPU 侧、不触碰 GPU（见 texture_loader.hpp 的线程约定）；
                    // `LoadImageHdr` 失败一律抛（不静默回退）⇒ 这里接住并转成"WARN + 回落"。
                    try {
                        hdri = vx::LoadImageHdr(hdriPath);
                    } catch (const std::exception& error) {
                        environmentFailure = error.what();
                    }
                } else if (step == 1 && environmentFailure.empty()) {
                    // 烘焙：三条 GPU 全屏 pass，一次提交 + 等栅栏（ADR 0021：加载期阻塞式一次性提交）。
                    // 计时是 ADR 0021 后果 2 的**实测回填义务**（预估"数百毫秒"，> 1 s 即须改分帧 / 下沉）。
                    vx::Clock  bakeClock;
                    const bool baked = renderer.BakeEnvironment(
                        vx::EnvironmentSource { hdri.pixels.data(), hdri.width, hdri.height });
                    const double bakeMs = bakeClock.Tick() * 1000.0;
                    if (baked) {
                        environmentIblReady = true;
                        VX_LOG_INFO("**环境贴图已烘焙**（IBL 三件套）：HDRI %ux%u（半精度上传、线性光）；"
                                    "irradiance %ux%u + 预过滤 %u 级 mip（%ux%u 起）+ BRDF LUT %u²；"
                                    "环境贴图显存 %.2f MB；烘焙耗时 %.0f ms",
                                    hdri.width, hdri.height, vx::kEnvironmentIrradianceWidth,
                                    vx::kEnvironmentIrradianceHeight, vx::kEnvironmentPrefilterMipCount,
                                    vx::kEnvironmentPrefilterBaseWidth, vx::kEnvironmentPrefilterBaseHeight,
                                    vx::kEnvironmentBrdfLutSize,
                                    static_cast<double>(renderer.EnvironmentTextureBytes()) / (1024.0 * 1024.0), bakeMs);
                    } else {
                        environmentFailure = "IBL 烘焙失败（详见上一行 ERROR 日志）";
                    }
                }
                ++step;
                return (step >= 2) ? 1.0 : 0.5;
            });
            if (!advanced) {
                VX_LOG_INFO("加载期收到退出请求（环境贴图阶段），退出");
                return EXIT_SUCCESS;
            }
            if (!environmentIblReady) {
                VX_LOG_WARN("环境贴图不可用 ⇒ **回落半球天空光**（不静默）：%s；"
                            "取资源：powershell -ExecutionPolicy Bypass -File tools\\fetch_assets.ps1"
                            "（来源 / 许可 / SHA-256 见 NOTICE.md「美术资源台账」）",
                            environmentFailure.c_str());
            }
        } else {
            VX_LOG_INFO("光照表未启用 `[environment]`（或 enabled = false）⇒ 环境光使用**半球天空光**"
                        "（ADR 0010 P1；打开 assets/config/lighting.toml 的 [environment] 段即可对比）");
        }

        // ================================================================================
        // V2b：**世界装载循环**（[ADR 0028](../docs/adr/0028-world-families-and-static-asset-first.md) 决策四"进程内真切世界"）——
        //   每轮 = 装配一个世界（**世界级状态全部是本轮局部变量**）并跑到"退出或切换"为止；
        //   **纹理 / 环境 IBL / 渲染器 / 窗口 / 面板留在循环之外复用**（重建它们既慢又违背 ADR 0028）。
        //   本轮末尾由 V2a 的 `WorldManager` 决定**退出**还是**切到下一个世界**（见本轮尾部的卸载 / 切换段）。
        //   ⚠️ 循环体**沿用原有缩进**（未再缩进一级）—— 以免产生万行级的纯空白 diff；语义与缩进无关。
        // ================================================================================
        bool        quitRequested  = false;  ///< 主循环要求退出（窗口关闭 / 面板"退出游戏"）
        std::size_t switchTestNext = 0;      ///< `--switch-test` 的下一个待触发项（**跨世界保持**）

        // 系统面板（T15，Esc）：面板就地编辑一份设置副本，主循环据此调用平台层与落盘。
        // V2b：**世界装载循环之外** —— 面板与用户设置是**跨世界共享**的状态（切换世界不重置它）。
        vx::SystemPanelContext panelContext;
        panelContext.settings           = systemSettings;
        panelContext.displayRefreshRate = displayRefreshRate;  // T17：帧率上限滑块的上界

        // 分辨率档位：取自 SDL 支持的显示模式（按尺寸去重、升序）。当前尺寸若不在列表里则补入，
        // 否则下拉框无法显示 / 回选当前值（例如窗口被手动缩放过）。
        std::vector<vx::DisplaySize> supportedResolutions = window.SupportedResolutions();
        const auto hasResolution = [&supportedResolutions](int width, int height) {
            return std::any_of(supportedResolutions.begin(), supportedResolutions.end(),
                               [width, height](const vx::DisplaySize& size) {
                                   return size.width == width && size.height == height;
                               });
        };
        if (!hasResolution(systemSettings.windowWidth, systemSettings.windowHeight)) {
            supportedResolutions.push_back(vx::DisplaySize { systemSettings.windowWidth, systemSettings.windowHeight });
            std::sort(supportedResolutions.begin(), supportedResolutions.end(),
                      [](const vx::DisplaySize& left, const vx::DisplaySize& right) {
                          if (left.width != right.width) {
                              return left.width < right.width;
                          }
                          return left.height < right.height;
                      });
        }
        panelContext.resolutions = &supportedResolutions;

        // V2b：**跨世界**的会话计时（只用于退出日志）——循环内的 `clock` 是每轮重建的帧计时器。
        vx::Clock sessionClock;

        for (;;) {
        // V2b：本轮要装配的世界 = `worldManager.Active()`（首轮由 `--world=` 决定；切换后由请求更新）。
        // 走 `--map=` 时注册表为空 ⇒ 保持外面那份 `MapPreset` 不变（行为与从前**逐位一致**）。
        const vx::LevelManifest* roundManifest = nullptr;
        if (worldManager.Count() > 0U) {
            preset            = worldManager.Active().terrain;
            presetSourceLabel = "清单 " + worldManager.ActiveId();
            roundManifest     = &worldManager.Active();
        }

        // ---- V5（[plans/v0.5.md](../docs/plans/v0.5.md) §1.13）：肉鸽秘境的**实例种子** ----
        // 口径（所有者 2026-10-06）：**首次进入 roll 一次 ⇒ 之后反复进入复用 ⇒ 重置（V9 菜单）才换**。
        // 只有**种子**被实例覆盖；半径 / 出生点 / 编辑仍以引用的地形预设为**单一事实来源**。
        if (roundManifest != nullptr && roundManifest->randomizeSeedOnEntry) {
            const bool existedBefore = worldManager.FindInstance(roundManifest->id) != nullptr;
            std::string instanceReason;
            if (!worldManager.EnsureInstance(roundManifest->id, RollInstanceSeed(), instanceReason)) {
                throw std::runtime_error("秘境实例化失败（世界 [" + roundManifest->id + "]）：" + instanceReason);
            }
            const vx::WorldInstance* instance = worldManager.FindInstance(roundManifest->id);
            if (instance == nullptr) {
                throw std::logic_error("秘境实例化后查不到实例（世界 [" + roundManifest->id + "]）");
            }
            preset.seed = instance->seed;
            VX_LOG_INFO("秘境实例（V5）：世界 [%s] 使用**实例种子 %llu**（第 %u 次生成）—— "
                        "**跨启动**复用同一种子（存档槽 V10；本次%s）⇒ 同一个世界；只有重置才会换新种子",
                        roundManifest->id.c_str(), static_cast<unsigned long long>(instance->seed),
                        instance->generation + 1U, existedBefore ? "沿用存档里的绑定" : "新建并写入存档");
            // V10：**首次**进入该秘境（实例由本次创建）⇒ 立刻持久化绑定（重置走 V9 菜单的另一条写入）。
            if (!existedBefore) {
                persistWorldInstances("首次进入肉鸽秘境");
            }
        }

        // ---- T61 / ADR 0020 决策二：**常驻集合 = 玩家窗口 ∩ 可挖区域** ----
        // 启动只常驻"出生点窗口"那批块；之后由 `DigVolumeScheduler` 随玩家移动建立 / 卸载。
        // 必须**先于** `LoadTile`：层间交接过滤器（ADR 0011）的输入已由"静态区域"换成"当前常驻集合"。
        // T80 / ADR 0020 决策二修订：启动常驻集合按**预取窗口**（= 活动窗口 + 预取环）算，
        // 与运行期调度器（构造时传入同一个预取宽度）口径一致 ⇒ 不会在第一步就出现"先建再卸"的抖动。
        const vx::DigVolumeWindow spawnWindow =
            vx::WindowForPlayerBlocks(preset.spawnX, preset.spawnZ, kDigVolumeWindowRadiusTiles);
        const vx::DigVolumeWindow spawnResidencyWindow = vx::WindowForPlayerBlocks(
            preset.spawnX, preset.spawnZ, kDigVolumeWindowRadiusTiles + vx::kResidencyPrefetchTiles);
        std::vector<vx::BlockCoord> initialVolumeCoords;
        for (const vx::BlockCoord& coord : digRegions.Blocks()) {
            if (spawnResidencyWindow.ContainsBlock(coord)) {
                initialVolumeCoords.push_back(coord);
            }
        }
        VolumeSlotTable volumeSlots;
        for (const vx::BlockCoord& coord : initialVolumeCoords) {
            volumeSlots.Insert(coord);
        }
        ResidentQuadFilter    residentQuadFilter(volumeSlots);
        vx::DigVolumeScheduler volumeScheduler(digRegions, kDigVolumeWindowRadiusTiles, vx::kResidencyPrefetchTiles);
        VX_LOG_INFO("可挖体积常驻窗口（ADR 0020 决策二 + T80 预取）：玩家 tile (%d, %d)：**活动窗口** ± %d（能挖三维洞）"
                    "、**常驻窗口** ± %d（含预取环宽 %d 格块）；启动常驻 %zu 块（区域表共 %zu 块）",
                    spawnWindow.centerTileX, spawnWindow.centerTileZ, spawnWindow.radiusTiles,
                    spawnResidencyWindow.radiusTiles, vx::kResidencyPrefetchTiles, initialVolumeCoords.size(),
                    digRegions.Blocks().size());
        std::vector<vx::BlockCoord> volumeResidencyChanged;  ///< 每帧调度产生的"建 / 卸"块（复用缓冲）
        std::vector<vx::BlockCoord> volumeCreated;           ///< 本帧新建的块（入延后队列，复用缓冲）
        std::vector<vx::TileCoord>  volumeTouchedTiles;      ///< 本帧接管状态翻转的 tile（入延后队列，复用缓冲）

        // ---- V4（[ADR 0026](../docs/adr/0026-premade-map-format-and-bake-tool.md)）：`source = premade` ⇒
        // 地表数据来自**离线烘焙的预制文件**（不再程序化生成）。**校验即抛**：文件缺失 / 魔数错 / 版本不符 /
        // （半径, 种子）与清单引用的地形预设不一致 ⇒ 直接失败，**不静默回退**到程序化。
        std::unique_ptr<vx::PremadeTerrainTileSource> premadeTerrainSource;
        if (roundManifest != nullptr && roundManifest->source == vx::WorldSource::Premade) {
            try {
                vx::PremadeMapReader reader = vx::PremadeMapReader::Open(roundManifest->premadeFilePath);
                if (reader.TileRadiusX() != preset.tileRadiusX || reader.TileRadiusZ() != preset.tileRadiusZ ||
                    reader.Seed() != preset.seed) {
                    throw std::runtime_error(
                        "预制文件的（半径 / 种子）与清单引用的地形预设不一致：文件 [" +
                        std::to_string(reader.TileRadiusX()) + "×" + std::to_string(reader.TileRadiusZ()) +
                        "，种子 " + std::to_string(reader.Seed()) + "]；清单 [" +
                        std::to_string(preset.tileRadiusX) + "×" + std::to_string(preset.tileRadiusZ) +
                        "，种子 " + std::to_string(preset.seed) + "]（说明该文件是用别的预设烘焙的）");
                }
                VX_LOG_INFO("预制地图已打开（V4 / ADR 0026）：世界 [%s] 的地表数据来自 %s"
                            "（schema %u、种子 %llu、tile 半径 [%d, %d]、块数 %zu）—— **不再程序化生成**",
                            roundManifest->id.c_str(), roundManifest->premadeFilePath.string().c_str(),
                            reader.SchemaVersion(), static_cast<unsigned long long>(reader.Seed()),
                            reader.TileRadiusX(), reader.TileRadiusZ(), reader.ChunkCount());
                premadeTerrainSource = std::make_unique<vx::PremadeTerrainTileSource>(std::move(reader));
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string("预制世界 [") + roundManifest->id + "] 的预制文件无法加载（" +
                                         roundManifest->premadeFilePath.string() + "）：" + error.what() +
                                         "；请先运行 tools\\bake_premade_maps.ps1 生成（ADR 0026）");
            }
        }

        vx::TerrainWorld world(preset.seed, materials, terrainParams);
        world.SetMapPreset(preset);  // 噪声先行、编辑覆盖其上（必须在 LoadTile 之前）
        // V4：注入地表数据源（`nullptr` ⇒ 程序化，与从前**逐位一致**）。必须在 LoadTile 之前。
        world.SetTileSource(premadeTerrainSource.get());
        // T8 层间交接（ADR 0011）＋ T61：判据 = **当前常驻集合**（ADR 0020 决策三），故必须在 LoadTile 之前设置。
        // W4：**地表壳**的近场区域 + 级联四边形过滤器（可挖体积接管 ∪ 地表壳接管）。
        // 区域按 tile 对齐 ⇒ 被跳过的四边形整块落在若干 tile 内 ⇒ 那些 tile 的网格变空 ⇒
        // `CollisionBodies` 阶段会自动交出它们的高度场碰撞体（与 ADR 0012 同一条路径）。
        vx::SurfaceShellRegion shellRegion;
        shellRegion.minColumnX = kShellTileMin * vx::kTerrainTileSize;
        shellRegion.maxColumnX = (kShellTileMax + 1) * vx::kTerrainTileSize;
        shellRegion.minColumnZ = kShellTileMin * vx::kTerrainTileSize;
        shellRegion.maxColumnZ = (kShellTileMax + 1) * vx::kTerrainTileSize;
        vx::SurfaceShellParams shellParams;
        ShellQuadFilter        shellQuadFilter(shellRegion);
        CompositeQuadFilter    compositeQuadFilter(residentQuadFilter, shellQuadFilter);
        world.SetQuadFilter(&compositeQuadFilter);

        // ---- W7-S3a：地表 tile 的**常驻集合**（ADR 0024 决策一）----
        // 地图范围（世界内**存在**的 tile）由预设 tile 半径决定；**常驻集合**只取"玩家窗口 + 预取环"⇒
        // 与世界总大小无关（10km 与 1km 的常驻量同级）。三个并行数组（同下标 = 同一 tile）作为常驻集合。
        std::vector<vx::TileCoord>   tileCoords;
        std::vector<vx::MeshHandle>  tileHandles;
        std::vector<WorldAabb>       tileBounds;
        std::vector<std::uint8_t>    tileCollisionActive;  ///< W7-S3b：与上面三个数组**同下标**（0/1 = 是否装有碰撞体）
        const std::size_t            worldTilesX = static_cast<std::size_t>(2 * preset.tileRadiusX + 1);
        const std::size_t           worldTilesZ = static_cast<std::size_t>(2 * preset.tileRadiusZ + 1);
        const std::size_t           residencyCap = static_cast<std::size_t>(
            2 * (kGameTerrainLodRings.radii[2] + vx::kTerrainResidencyPrefetchTiles) + 1);
        tileCoords.reserve(residencyCap * residencyCap);
        tileHandles.reserve(residencyCap * residencyCap);
        tileBounds.reserve(residencyCap * residencyCap);

        // W7-S3b：分环调度器（活动半径 = 最外环 32 tile；LOD 按 Chebyshev 环距离分配）。
        vx::TerrainTileScheduler tileScheduler(
            vx::MakeTerrainTileRange(-preset.tileRadiusX, -preset.tileRadiusZ, 2 * preset.tileRadiusX + 1,
                                     2 * preset.tileRadiusZ + 1),
            kGameTerrainLodRings, vx::kTerrainResidencyPrefetchTiles);
        std::vector<vx::TileCoord> terrainResidencyChanged;  ///< 每帧调度产生的"建 / 卸"tile（复用缓冲）
        std::vector<vx::TileCoord> terrainRelodChanged;      ///< 每帧调度产生的"LOD 切换"tile（复用缓冲）

        // ---- W7-S3b：地表 tile 构建**下沉 worker**（[ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md) 形态）----
        // 形态与可挖体积（T81）同源：worker 只跑纯函数（生成 + 网格化），主线程只做"收包 + 过滤 + 安装 + GPU 上传"。
        // 逐个 worker 各自持有由 `(seed, params)` 构造的 `TerrainNoiseGenerator`（不共用 `TerrainWorld` 的）。
        // 线程池不可用时自动回落同步路径（结果不变、只是尖峰回到从前；`TaskScheduler` 会 WARN 一次，不静默）。
        // V4：worker 侧同样优先读**预制数据源**（否则流式建块会绕过它、又变回程序化生成）。
        vx::TerrainTileBuildPipeline terrainBuildPipeline(preset.seed, terrainParams, preset.edits, 0,
                                                          premadeTerrainSource.get());
        const bool                   terrainHasWorkers = terrainBuildPipeline.HasWorkers();
        VX_LOG_INFO(
            "地表 tile 构建（W7-S3b / ADR 0022 形态）：%s（worker 线程 %u 个；主线程只做「收包 + 过滤 + 安装 + GPU 上传」）",
            terrainHasWorkers ? "**下沉 worker**（生成 + 网格化不再占用渲染帧）"
                              : "**不可用 ⇒ 回退同步构建**（见上方 WARN）",
            terrainBuildPipeline.WorkerThreadCount());

        std::set<vx::TileCoord>    terrainBuildInFlight;        ///< 已提交、尚未收包的**加载**任务坐标（主线程独占）
        std::set<vx::TileCoord>    terrainRelodInFlight;        ///< 已提交、尚未收包的 **relod** 任务坐标
        std::vector<vx::TileCoord> terrainPendingLoadScratch;   ///< `CollectPendingLoadTiles` 复用缓冲
        std::vector<vx::TileCoord> terrainRelodUploads;         ///< 已安装、待重传 GPU 的 relod tile（可能有上帧残留）
        int terrainPrefetchCenterX = std::numeric_limits<int>::max();
        int terrainPrefetchCenterZ = std::numeric_limits<int>::max();

        // 收包：加载结果 → 预取缓存（`Step` 命中即**廉价安装**）；relod 结果 → 直接换网格 + 记入待重传清单。
        // 每帧把完成队列**全部**取回（队列长度 ≤ 预取提前量 `kTerrainPrefetchLookahead`，**有上界**；
        // 每次只是一个 move）。**只暂存仍在当前常驻窗口内的结果**（窗口已移走的陈旧结果直接丢弃，
        // 否则会占用预取提前量、挤掉新窗口的预取）。
        const auto drainTerrainTileBuilds = [&]() {
            vx::TerrainTileBuildResult built;
            while (terrainBuildPipeline.TakeCompleted(built)) {
                if (built.remeshOnly) {
                    terrainRelodInFlight.erase(built.coord);
                    if (world.InstallRemeshedMesh(built.coord, built.lodLevel, std::move(built.mesh))) {
                        terrainRelodUploads.push_back(built.coord);
                    }
                } else {
                    terrainBuildInFlight.erase(built.coord);
                    if (tileScheduler.ResidencyWindow().Contains(built.coord)) {
                        world.StageTile(std::move(built.tile), std::move(built.mesh));
                    }
                }
            }
        };

        // 预取：为"待加载、未暂存、未在飞"的坐标**提前**提交 worker（早于 `Step`；ADR 0024 决策一）。
        const auto prefetchTerrainTiles = [&]() {
            terrainPendingLoadScratch.clear();
            if (!terrainHasWorkers) {
                return;  // 无 worker：`Step` 走同步回退（结果不变，只是尖峰回到从前）
            }
            tileScheduler.CollectPendingLoadTiles(terrainPendingLoadScratch);
            std::size_t submits = 0;
            for (const vx::TileCoord& coord : terrainPendingLoadScratch) {
                if (submits >= kTerrainPrefetchSubmitsPerFrame) {
                    break;
                }
                if (world.StagedTileCount() + terrainBuildInFlight.size() >= kTerrainPrefetchLookahead) {
                    break;  // 提前量封顶 ⇒ 预取缓存 / 在飞任务的内存有上界
                }
                const int lod = tileScheduler.LodLevelForTile(coord);
                if (world.HasStagedTile(coord.x, coord.z, lod) ||
                    terrainBuildInFlight.find(coord) != terrainBuildInFlight.end()) {
                    continue;
                }
                vx::TerrainTileBuildRequest request;
                request.coord    = coord;
                request.lodLevel = lod;
                terrainBuildPipeline.Submit(std::move(request));
                terrainBuildInFlight.insert(coord);
                ++submits;
            }
        };

        // 门控：`Step` 本帧会加载的**前 `batch` 个** tile 必须**全部已就绪**，否则本帧不推进 `Step`
        //（等 worker；**绝不**在渲染帧内同步生成）。`terrainPendingLoadScratch` 已由 `prefetchTerrainTiles` 填好。
        const auto terrainStepReady = [&](std::size_t batch) {
            if (!terrainHasWorkers) {
                return true;  // 无 worker：`Step` 走同步回退（预期路径）
            }
            const std::size_t count = std::min(batch, terrainPendingLoadScratch.size());
            for (std::size_t i = 0; i < count; ++i) {
                const vx::TileCoord& coord = terrainPendingLoadScratch[i];
                if (!world.HasStagedTile(coord.x, coord.z, tileScheduler.LodLevelForTile(coord))) {
                    return false;
                }
            }
            return true;
        };

        // W7-S3b：**碰撞体随窗口重扫**的游标（只在窗口中心变化时开启一轮；每帧只做 `kTerrainCollisionActionsPerFrame`
        // 个跃迁 ⇒ 单帧成本有上界）。`centerX/Z` 记录上一轮中心，用来判定"是否要开启新一轮"。
        bool  terrainCollisionSweepActive = false;
        int   terrainCollisionSweepCursor = 0;
        int   terrainCollisionCenterX     = 0;
        int   terrainCollisionCenterZ     = 0;
        {
            // ---- 阶段 2：地形 tile（生成 + 网格化**下沉 worker**；主线程按常驻窗口**分帧安装**，不冻结画面）----
            const auto terrainLoadProgress = [&]() {
                const std::size_t desired = tileScheduler.DesiredCount();
                return (desired == 0U) ? 1.0
                                       : static_cast<double>(tileCoords.size()) / static_cast<double>(desired);
            };
            (void)tileScheduler.Update(world, preset.spawnX, preset.spawnZ);
            terrainPrefetchCenterX = tileScheduler.Window().centerTileX;
            terrainPrefetchCenterZ = tileScheduler.Window().centerTileZ;
            if (!loading.Run(LoadStage::TerrainTiles, [&]() {
                    if (!tileScheduler.HasPendingWork()) {
                        return 1.0;
                    }
                    terrainResidencyChanged.clear();
                    drainTerrainTileBuilds();
                    prefetchTerrainTiles();
                    // 门控：本帧 `Step` 会加载的**整批** tile 都已就绪才推进（等 worker；**绝不**在渲染帧内同步生成）。
                    if (!terrainStepReady(kTerrainTilesPerLoadSlice)) {
                        return terrainLoadProgress();
                    }
                    (void)tileScheduler.Step(world, kTerrainTilesPerLoadSlice, terrainResidencyChanged);
                    for (const vx::TileCoord& coord : terrainResidencyChanged) {
                        // 世界数据已由 `Step`（从预取缓存）安装 ⇒ 这里只登记常驻集合（GPU / 碰撞在后续阶段）。
                        // ⚠️ 四个并行数组**必须同步增长**（同下标 = 同一 tile）；漏掉任一个都会让后续
                        // `SyncResidentTileCollision` 的按下标写入越界（曾经真的踩到）。
                        tileCoords.push_back(coord);
                        tileHandles.push_back(vx::MeshHandle {});
                        tileBounds.push_back(WorldAabb {});
                        tileCollisionActive.push_back(0U);
                    }
                    return terrainLoadProgress();
                })) {
                VX_LOG_INFO("加载期收到退出请求（地形 tile 阶段），退出");
                return EXIT_SUCCESS;
            }
        }
        VX_LOG_INFO("预设地图已加载：%s（文件 %s）—— 种子 %llu，tile 半径 [%d, %d]（世界内共 %zu 个 tile），"
                    "地形编辑 %zu 条，出生点 (%.1f, %.1f)",
                    preset.name.c_str(), presetSourceLabel.c_str(), static_cast<unsigned long long>(preset.seed),
                    preset.tileRadiusX, preset.tileRadiusZ, worldTilesX * worldTilesZ, preset.edits.size(),
                    preset.spawnX, preset.spawnZ);
        VX_LOG_INFO("地表 tile 常驻集合（W7-S3b / ADR 0024）：活动半径 %d tile（LOD 分环 %d/%d/%d tile，"
                    "最外环 ≈ %d m）+ 预取环 %d ⇒ 启动常驻 **%zu** 个（世界内 %zu 个；窗口目标 %zu 个）",
                    kGameTerrainLodRings.radii[2], kGameTerrainLodRings.radii[0], kGameTerrainLodRings.radii[1],
                    kGameTerrainLodRings.radii[2], kGameTerrainLodRings.radii[2] * vx::kTerrainTileSize,
                    vx::kTerrainResidencyPrefetchTiles, tileCoords.size(), worldTilesX * worldTilesZ,
                    tileScheduler.DesiredCount());

        // V2c（细则 plans/v0.5.md §1.10）：**世界指纹** —— 同种子切回同一世界必须逐位相同（红线 7）。
        // 冒烟日志只能证明"没崩"，指纹给"是不是同一个世界"一个**可判定**的数字：
        // 种子 + 常驻 tile（**升序** ⇒ 与流式到达顺序无关）的**全分辨率高度**（与 LOD 无关）⇒ 一个 64 位摘要。
        {
            const std::vector<vx::TileCoord>     fingerprintCoords = world.ResidentTiles();
            std::vector<vx::FingerprintTileView> fingerprintViews;
            fingerprintViews.reserve(fingerprintCoords.size());
            for (const vx::TileCoord& coord : fingerprintCoords) {
                const vx::TerrainTile* tile = world.FindTile(coord.x, coord.z);
                if (tile == nullptr) {
                    continue;  // 与 `ResidentTiles` 同源，正常不会发生；异常时下面单独告警（不静默）
                }
                fingerprintViews.push_back(
                    vx::FingerprintTileView { coord.x, coord.z, tile->heights.data(), tile->heights.size() });
            }
            const std::uint64_t worldFingerprint =
                vx::ComputeTerrainFingerprint(preset.seed, fingerprintViews.data(), fingerprintViews.size());
            VX_LOG_INFO("世界指纹（V2c）：%016llx —— 种子 %llu、常驻 tile %zu（升序 + 全分辨率高度；"
                        "同种子切回必须**完全相同**）",
                        static_cast<unsigned long long>(worldFingerprint),
                        static_cast<unsigned long long>(preset.seed), fingerprintViews.size());
            if (fingerprintViews.size() != fingerprintCoords.size()) {
                VX_LOG_WARN("世界指纹（V2c）：%zu 个常驻 tile 无高度数据（已跳过）⇒ 指纹只覆盖有数据的部分",
                            fingerprintCoords.size() - fingerprintViews.size());
            }
        }

        // 物理世界 + 碰撞体。T28 / ADR 0012：**地表高度场只在"可见地表不归体积画"的 tile 上建**，
        // 其余 tile 的碰撞改由可挖体积的三角网提供（否则隐形高度场会把角色挡在自己挖的洞口外）。
        vx::PhysicsWorld     physics;
        vx::TerrainCollision terrainCollision(physics);
        // W7-S3a：常驻集合的资源绑定（GPU 网格 / 碰撞 / AABB 与 `tileCoords` 同下标）。
        TileResidencyResources tileResidency { world, terrainCollision, renderer, tileCoords, tileHandles,
                                               tileBounds, tileCollisionActive };

        // T33：Jolt 的重力必须与玩法层**同一口径**（Jolt 默认 -9.81，而角色 / 弹道用 `kGravity`）——
        // 否则倒塌的塔与角色会各按一套重力下落，世界不自洽。
        physics.SetGravity(glm::vec3(0.0F, -kGravity, 0.0F));

        // T33：倒塌整体的运行时 —— 渲染网格池在**加载期**建好（SKILL 第四节硬规则 4：
        // 资源与管线创建不得发生在渲染热路径）。倒塌发生时只做"就地写顶点 + 每帧推位姿"。
        // T46：槽位数由 `kCollapseMeshSlots` 决定（岩石残骸保留几何体 ⇒ 槽位不随落定释放）。
        vx::RigidCollapseRuntime rigidCollapse;
        rigidCollapse.Init(renderer, kCollapseMeshSlots, kCollapseMeshCapacityVerts);

        // T81 / [ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md)：**块构建任务池**。
        // 声明顺序刻意放在 `digVolumes` **之前** ⇒ 它的生命周期覆盖后者（后者持裸指针，须"后建先毁"）。
        // 线程池不可用时构造会 WARN 一次，`CreateBlock` 自动走同步路径（结果不变、只是尖峰回到从前）。
        vx::VolumeBuildPipeline volumeBuildPipeline;
        VX_LOG_INFO("可挖体积块构建（T81 / ADR 0022）：%s（worker 线程 %u 个；主线程只做「采快照 + 收包 + 上传」）",
                    volumeBuildPipeline.HasWorkers()
                        ? "**下沉 worker**（FillBlockDensity + MeshBlock 不再占用渲染帧）"
                        : "**不可用 ⇒ 回退同步构建**（见上方 WARN）",
                    volumeBuildPipeline.WorkerThreadCount());

        // T8：**可挖体积世界**（ADR 0004 层 ②）——只在标记区域内存在。初始密度由地表高度场推导
        // （地下实心 / 空中空），块集合与 `digRegions.Blocks()` 一一对应。
        //
        // T36：块数可达数百（每块 33³ 采样 + 一次等值面网格化），一次跑完要停下等好几秒 ⇒ 分步推进：
        // 每帧在预算内做一步（填一块密度 / 网格化一块），两次之间照常出加载画面。
        vx::DigVolumeWorld digVolumes(world, digRegions);
        // T81：运行期"按需建块"（常驻调度）改走 worker；**加载期分步初始化仍走同步路径**（它本来就在
        // 加载画面之间按帧推进、且要求与批量初始化逐位一致，不动它风险最低）。
        digVolumes.SetBuildPipeline(&volumeBuildPipeline);
        // T61：**只初始化常驻集合**（玩家窗口 ∩ 区域表），而不是整张区域表（ADR 0020 决策二）。
        digVolumes.BeginInitFromHeightField(initialVolumeCoords);
        if (!loading.Run(LoadStage::DiggableVolumes, [&digVolumes]() {
                if (digVolumes.StepInitFromHeightField(kVolumeInitStepsPerSlice)) {
                    return 1.0;
                }
                return static_cast<double>(digVolumes.InitCompletedSteps()) /
                       static_cast<double>(digVolumes.InitTotalSteps());
            })) {
            VX_LOG_INFO("加载期收到退出请求（可挖体积阶段），退出");
            return EXIT_SUCCESS;
        }
        {
            std::size_t surfaceBlocks = 0;
            for (const vx::BlockCoord& coord : initialVolumeCoords) {
                const vx::MeshData* mesh = digVolumes.FindMesh(coord);
                if (mesh != nullptr && !mesh->vertices.empty()) {
                    ++surfaceBlocks;
                }
            }
            VX_LOG_INFO("可挖体积就绪：%zu 个块（密度 %.2f MB + 体素材质 %.2f MB，后者**懒分配**），其中 %zu 块存在等值面"
                        "（Surface Nets 网格化，法线由密度梯度给出）；**常驻集合内的地表已由体积接管**（ADR 0011 / 0020）",
                        initialVolumeCoords.size(), static_cast<double>(digVolumes.VoxelBytes()) / (1024.0 * 1024.0),
                        static_cast<double>(digVolumes.MaterialBytes()) / (1024.0 * 1024.0), surfaceBlocks);
        }

        // ---- W4：**地表壳**近场块清单（网格化放进下面**有预算**的阶段里，不在这里同步做）----
        // 另建一个噪声源，但用**同一份** `(种子, 参数)` ⇒ 与 `TerrainWorld` 的世界同源（纯函数 ⇒ 逐位一致）。
        const vx::TerrainNoiseGenerator shellNoise(preset.seed, terrainParams);

        // ---- W6：河流（ADR 0027）—— 河道自高处沿下坡生成，河床**刻蚀进地表壳**；水面按静态水位生成 ----
        // 与壳**同一份**下切场 ⇒ 河床碰撞随壳的三角网自动承担（"谁画谁挡"零分叉）。
        vx::RiverPath       riverPath;
        vx::RiverCarveField riverCarve;
        {
            riverPath = vx::GenerateRiverPath(shellNoise, terrainParams.river, shellRegion.minColumnX,
                                              shellRegion.minColumnZ, shellRegion.maxColumnX,
                                              shellRegion.maxColumnZ);
            riverCarve = vx::RiverCarveField(riverPath, terrainParams.river, shellRegion.minColumnX,
                                             shellRegion.minColumnZ, shellRegion.maxColumnX - shellRegion.minColumnX,
                                             shellRegion.maxColumnZ - shellRegion.minColumnZ);
            VX_LOG_INFO("河流（W6 / ADR 0027）：%s；河道 %zu 个节点（下切场 %d×%d 格；中心下切 %.1f 格、水深 %.1f 格）",
                        terrainParams.river.enabled ? "**启用**" : "关闭", riverPath.nodes.size(),
                        shellRegion.maxColumnX - shellRegion.minColumnX,
                        shellRegion.maxColumnZ - shellRegion.minColumnZ,
                        static_cast<double>(terrainParams.river.channelDepthBlocks),
                        static_cast<double>(terrainParams.river.waterDepthBlocks));
        }

        std::vector<vx::BlockCoord>     shellBlockCoords;
        std::vector<vx::MeshData>       shellBlockMeshes;
        std::vector<vx::MeshHandle>     shellHandles;
        std::vector<WorldAabb>          shellBounds;
        {
            const int blockMinX = shellRegion.minColumnX / vx::kVolumeBlockSize;
            const int blockMaxX = (shellRegion.maxColumnX - 1) / vx::kVolumeBlockSize;
            const int blockMinZ = shellRegion.minColumnZ / vx::kVolumeBlockSize;
            const int blockMaxZ = (shellRegion.maxColumnZ - 1) / vx::kVolumeBlockSize;
            for (int bz = blockMinZ; bz <= blockMaxZ; ++bz) {
                for (int bx = blockMinX; bx <= blockMaxX; ++bx) {
                    const vx::ShellBlockSpan span = vx::ComputeShellBlockSpanY(shellNoise, shellParams, bx, bz);
                    for (int by = span.minBlockY; by <= span.maxBlockY; ++by) {
                        shellBlockCoords.push_back(vx::BlockCoord { bx, by, bz });
                    }
                }
            }
        }
        shellBlockMeshes.resize(shellBlockCoords.size());
        shellHandles.resize(shellBlockCoords.size());
        shellBounds.resize(shellBlockCoords.size());
        VX_LOG_INFO("地表壳（W4 / ADR 0023）：区域列 [%d, %d)²，待建 %zu 个块（近场；全图铺开属 W7 流式）",
                    shellRegion.minColumnX, shellRegion.maxColumnX, shellBlockCoords.size());

        // ---- T28 碰撞接管（ADR 0012）----
        // 判据直接取"该 tile 的地表网格是否已经没有任何面"：它与 ADR 0011 的四边形跳过判据**同源**
        // （同一次 `BuildTerrainMesh`），因此不可能出现"渲染交给体积、碰撞却留在高度场"的漂移。
        //
        // T36：建碰撞体要逐个跑 Jolt 的网格形状构建（单块可达数十毫秒）⇒ 与 tile 判定合并成一个分片任务，
        // 每帧在预算内建若干个，中间照常出加载画面。
        vx::VolumeCollision volumeCollision(physics);
        std::size_t         collisionTiles = 0;
        std::size_t         takenOverTiles = 0;
        std::size_t         noCollisionTiles = 0;  ///< W7-S3b：超出碰撞半径、**不建**高度场碰撞体的 tile
        std::size_t         shellSurfaceBlocks = 0;
        {
            const std::size_t tileCount   = tileCoords.size();
            const std::size_t volumeCount = initialVolumeCoords.size();
            const std::size_t shellCount  = shellBlockCoords.size();
            const std::size_t totalUnits  = tileCount + volumeCount + shellCount;
            std::size_t       unit        = 0;
            if (!loading.Run(LoadStage::CollisionBodies, [&]() {
                    if (unit >= totalUnits) {
                        return 1.0;
                    }
                    if (unit < tileCount) {
                        // 可见面全归体积 / 地表壳 ⇒ 高度场碰撞体交出（判据与 ADR 0011 同源，见该助手）。
                        // W7-S3b：超出碰撞半径的 tile **不建**碰撞体（记入 `noCollisionTiles`）。
                        if (NeedsTerrainCollision(tileScheduler.Window(), tileResidency.coords[unit])) {
                            if (SyncResidentTileCollision(tileResidency, unit, /*wantCollision=*/true)) {
                                ++collisionTiles;
                            } else {
                                ++takenOverTiles;
                            }
                        } else {
                            (void)SyncResidentTileCollision(tileResidency, unit, /*wantCollision=*/false);
                            ++noCollisionTiles;
                        }
                    } else if (unit < tileCount + volumeCount) {
                        (void)volumeCollision.SyncBlock(digVolumes, initialVolumeCoords[unit - tileCount]);
                    } else {
                        // W4：地表壳块 —— **网格化一次**，同时用于碰撞（下面）与渲染（`MeshUpload` 阶段）。
                        const std::size_t    index = unit - tileCount - volumeCount;
                        const vx::BlockCoord& block = shellBlockCoords[index];
                        shellBlockMeshes[index] =
                            vx::BuildShellBlockMesh(shellNoise, terrainParams, shellParams, shellRegion, block,
                                                    &riverCarve);
                        if (!shellBlockMeshes[index].indices.empty()) {
                            AddShellBlockCollider(physics, shellBlockMeshes[index], block);
                            ++shellSurfaceBlocks;
                        }
                    }
                    ++unit;
                    return static_cast<double>(unit) / static_cast<double>(totalUnits);
                })) {
                VX_LOG_INFO("加载期收到退出请求（碰撞体阶段），退出");
                return EXIT_SUCCESS;
            }
        }
        const std::size_t volumeBodies = volumeCollision.BlockBodyCount();
        VX_LOG_INFO("碰撞接管（ADR 0012）：地表高度场碰撞体 %zu 个；%zu/%zu 个 tile 的可见面已全由体积绘制"
                    "（其高度场碰撞体已交出）；**%zu 个 tile 超出碰撞半径 %d（不建碰撞体）**；"
                    "可挖体积三角网碰撞体 %zu 个",
                    collisionTiles, takenOverTiles, tileCoords.size(), noCollisionTiles,
                    kTerrainCollisionRadiusTiles, volumeBodies);
        VX_LOG_INFO("地表壳（W4 / ADR 0023）：%zu/%zu 个块有等值面；网格已作为**三角网静态碰撞体**登记"
                    "（与渲染同源）；区域世界列中心 ≈ (%d, %d)",
                    shellSurfaceBlocks, shellBlockCoords.size(),
                    (shellRegion.minColumnX + shellRegion.maxColumnX) / 2,
                    (shellRegion.minColumnZ + shellRegion.maxColumnZ) / 2);

        // T18 / T84：世界边界由**地图范围自动推导**（tile_radius → 世界列范围），不硬编码：换地图或将来
        // 改由程序化决定大小时自动跟随。**六面封闭**（T84，所有者 2026-10-05 裁定）：四周建**不可见**静态墙
        // 挡住地面行走，**再加一块不可见顶盖** ⇒ 飞行也无法"升到墙顶之上再横向越过"；
        // 出界救援（见主循环）**降为纯兜底**（防边界漏洞与意外坠落），不再是正常的越界出口。
        const vx::WorldBounds                 bounds = vx::ComputeWorldBounds(preset.tileRadiusX, preset.tileRadiusZ);
        const std::array<vx::BoundaryWall, 4> boundaryWalls =
            vx::ComputeBoundaryWalls(bounds, vx::kBoundaryWallThickness);
        const vx::BoundaryWall boundaryCeiling = vx::ComputeBoundaryCeiling(bounds, vx::kBoundaryWallThickness);
        std::size_t boundaryWallBodies = 0;
        for (std::size_t i = 0; i < boundaryWalls.size(); ++i) {
            vx::PhysicsWorld::BoxDesc box;
            box.center      = boundaryWalls[i].center;
            box.halfExtents = boundaryWalls[i].halfExtents;
            if (physics.AddStaticBox(box) != 0) {
                ++boundaryWallBodies;
            }
            VX_LOG_INFO("边界墙 #%zu（%s）：中心 (%.1f, %.1f, %.1f)，半长 (%.1f, %.1f, %.1f)",
                        i, (i == 0) ? "-X" : ((i == 1) ? "+X" : ((i == 2) ? "-Z" : "+Z")), box.center.x, box.center.y,
                        box.center.z, box.halfExtents.x, box.halfExtents.y, box.halfExtents.z);
        }
        {
            vx::PhysicsWorld::BoxDesc box;
            box.center      = boundaryCeiling.center;
            box.halfExtents = boundaryCeiling.halfExtents;
            if (physics.AddStaticBox(box) != 0) {
                ++boundaryWallBodies;
            }
            VX_LOG_INFO("边界顶盖（T84 六面封闭）：中心 (%.1f, %.1f, %.1f)，半长 (%.1f, %.1f, %.1f)", box.center.x,
                        box.center.y, box.center.z, box.halfExtents.x, box.halfExtents.y, box.halfExtents.z);
        }
        VX_LOG_INFO("世界边界（由 tile 半径 [%d, %d] 自动推导）：范围 (%.1f, %.1f, %.1f) ~ (%.1f, %.1f, %.1f)；"
                    "不可见围墙 4 堵 + 顶盖 1 块 = %zu/5 个盒体（**六面封闭**），厚 %.1f 格；出界救援余量 %.1f 格（仅兜底）",
                    preset.tileRadiusX, preset.tileRadiusZ, bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x,
                    bounds.max.y, bounds.max.z, boundaryWallBodies, vx::kBoundaryWallThickness,
                    vx::kOutOfBoundsMargin);

        // T79③：**加载期静态体已全部加完**（地表高度场 + 可挖体积三角网 + 4 面围墙 + 1 块顶盖）⇒ 在**首个 `Update`
        // 之前**调用一次 `OptimizeBroadPhase()`（Jolt 官方文档："needed only if you've added many bodies
        // prior to calling `Update()` for the first time"）。不调用的话，同一份"建造包围体树"的工作会被
        // `PhysicsSystem::Update` **摊到随后若干帧**上（每帧多花一点 CPU，正是帧尖峰打点里的"未计时"）。
        // **不得每帧调用**（文档原文："Don't call this every frame"）—— 那是把已摊平的工作重新集中。
        {
            vx::Clock broadPhaseClock;
            physics.OptimizeBroadPhase();
            VX_LOG_INFO("物理宽相位已优化（T79③ / Jolt `OptimizeBroadPhase`）：静态体共 %zu 个"
                        "（地表高度场 %zu + 可挖体积三角网 %zu + 围墙 %zu），耗时 %.2f ms；"
                        "此后 `Update` 的建树工作不再摊到随后若干帧",
                        physics.BodyCount(), collisionTiles, volumeBodies, boundaryWallBodies,
                        broadPhaseClock.Tick() * 1000.0);
        }

        // T36：加载到了收尾段 —— 下面都是毫秒级的设置（出生点 / 角色 / 相机 / 面板数据 / 渲染原点），
        // 但仍照常出帧，保证"任何一步都不让画面停下等待"这条规则在这里也成立。
        if (!loading.Pump(LoadStage::Finalize, 0.5)) {
            VX_LOG_INFO("加载期收到退出请求（收尾阶段），退出");
            return EXIT_SUCCESS;
        }

        // T12：出生点来自预设地图（世界列坐标）；角色脚底抬离地表一点，随后自然落到地表。
        const float spawnX = static_cast<float>(preset.spawnX);
        const float spawnZ = static_cast<float>(preset.spawnZ);
        float       spawnSurface = static_cast<float>(kFallbackSpawnHeight);
        if (!world.QueryHeight(spawnX, spawnZ, spawnSurface)) {
            VX_LOG_WARN("出生列 (%.1f, %.1f) 无地表数据，改用回退高度 %d 格", static_cast<double>(spawnX),
                        static_cast<double>(spawnZ), kFallbackSpawnHeight);
        }
        VX_LOG_INFO("出生点：列 (%.1f, %.1f)，地表 %.2f 格，脚底 %.2f 格（离地 %.2f 格）",
                    static_cast<double>(spawnX), static_cast<double>(spawnZ), static_cast<double>(spawnSurface),
                    static_cast<double>(spawnSurface + kSpawnClearance), static_cast<double>(kSpawnClearance));

        // ---- V0b：物件层（ADR 0004 层③「物件 / 建造」）接线 ----
        // 读类型表 + 放置清单 → 逐条放置（底面 Y 按**地表高度**求解）→ 上传网格 + 建**静态三角网**碰撞体。
        // 硬约束（ADR 0004 / ADR 0028）：物件是**独立实体**，几何**绝不写进地形场**；
        // 渲染与碰撞**共用同一份 `MeshData`**（"谁画谁挡"同源，尺寸不可能漂移）。
        // 静态碰撞体**没有旋转接口** ⇒ 朝向烘进顶点（`RotateMeshAboutY`），与渲染的四元数同向。
        // V3：物件清单**由世界清单指定**（`objects_file`；ADR 0028 §一"差异全部落在清单"）——
        //     走 `--world=` 时取该世界清单的路径（绝对路径直接用，缺省为仓库相对字面量再拼 `SourceAssetPath`）；
        //     走 `--map=`（注册表为空）时回退到全局默认 `assets/config/objects.toml`。
        const std::filesystem::path objectsPath = [&]() -> std::filesystem::path {
            if (worldManager.Count() > 0U) {
                const std::filesystem::path& declared = worldManager.Active().objectsFile;
                return declared.is_absolute() ? declared : SourceAssetPath(declared);
            }
            return SourceAssetPath("assets/config/objects.toml");
        }();
        vx::ObjectTable objects = vx::ObjectTable::LoadFromFile(objectsPath);
        vx::ObjectLayer objectLayer;
        std::vector<ObjectSlot> objectSlots;

        // V8：模型文件**按路径缓存**（同一 `model_file` 只载入一次；多个类型 / 多次散布共用同一模型）。
        // 局部网格按形态分流：程序化形态走 `BuildObjectMesh`，`Model` 走 `BuildObjectMeshFromModel`
        //（等比装进 `2*half_extent` 的盒、底面贴地）—— 两者产物都是同一 `vx::MeshData` ⇒ 下游（渲染 + 碰撞 + 剔除）零分叉。
        std::unordered_map<std::string, vx::Model> modelCache;
        const auto buildLocalMesh = [&](const vx::ObjectType& type) -> vx::MeshData {
            if (type.kind != vx::ObjectAssetKind::Model) {
                return vx::BuildObjectMesh(type);
            }
            auto found = modelCache.find(type.modelFile);
            if (found == modelCache.end()) {
                found = modelCache.emplace(type.modelFile, vx::LoadModel(SourceAssetPath(type.modelFile))).first;
                VX_LOG_INFO("物件模型已载入（V8）：%s（网格 %zu 个）", type.modelFile.c_str(),
                            found->second.meshes.size());
            }
            // `material_slot` 未指定（-1）时取形态默认（对 `Model` 即草槽，见 `ObjectMaterialSlot` 的说明）。
            const float materialSlot = (type.materialSlot >= 0) ? static_cast<float>(type.materialSlot)
                                                               : vx::ObjectMaterialSlot(type.kind);
            return vx::BuildObjectMeshFromModel(found->second, type.halfExtentX, type.halfExtentY, type.halfExtentZ,
                                                materialSlot);
        };

        // V8：落点清单 = **显式 `[[placement]]`（按文件顺序）+ 程序化 `[[scatter]]` 展开**（按文件顺序）。
        // 顺序固定 ⇒ "遍历顺序 = 放置顺序"（确定性，红线 7）仍然成立。
        std::vector<vx::ObjectPlacement> objectPlan = objects.placements;
        std::size_t                      scatterPointCount = 0;
        for (const vx::ObjectScatter& scatter : objects.scatters) {
            const std::vector<vx::ScatterPoint> points = vx::PlanObjectScatter(scatter);
            if (points.size() < static_cast<std::size_t>(scatter.count)) {
                VX_LOG_WARN("散布 [%s] 只生成 %zu / %d 个点（半径太小？已抬高格点密度重试）",
                            scatter.typeId.c_str(), points.size(), scatter.count);
            }
            for (const vx::ScatterPoint& point : points) {
                vx::ObjectPlacement placed;
                placed.typeId     = scatter.typeId;
                placed.x          = point.x;
                placed.z          = point.z;
                placed.yawDegrees = point.yawDegrees;  // 散布给出朝向；显式放置仍用 `yaw_deg`
                objectPlan.push_back(std::move(placed));
            }
            scatterPointCount += points.size();
        }

        objectSlots.reserve(objectPlan.size());
        std::vector<vx::PortalEntry> portals;  // V3：供"最近门"查询（交互用）
        portals.reserve(2U);
        std::size_t objectSkipped = 0;
        for (const vx::ObjectPlacement& declared : objectPlan) {
            vx::ObjectPlacement placed = declared;
            float                surface = 0.0F;
            if (!world.QueryHeight(placed.x, placed.z, surface)) {
                // 不静默：落点没有地表数据时**跳过并告警**，绝不猜一个高度把物件放到错的地方。
                VX_LOG_WARN("物件 [%s] 的落点 (%.1f, %.1f) 无地表数据 ⇒ 跳过该条放置",
                            placed.typeId.c_str(), static_cast<double>(placed.x), static_cast<double>(placed.z));
                ++objectSkipped;
                continue;
            }
            placed.y = surface;  // 底面贴地表（`objects.toml` 的 y 分量当前不生效）

            const std::uint32_t id = objectLayer.Place(objects, placed);
            vx::ObjectInstance  instance;
            (void)objectLayer.Get(id, instance);

            const vx::MeshData localMesh    = buildLocalMesh(*instance.type);
            const vx::MeshData colliderMesh = vx::RotateMeshAboutY(localMesh, instance.yawDegrees);
            const glm::dvec3   origin(static_cast<double>(instance.x), static_cast<double>(instance.y),
                                      static_cast<double>(instance.z));
            // V3：传送门登记到交互表（`target_world` / `portal_name` 来自放置条目 —— `ObjectInstance` 不带这些）。
            if (instance.type->kind == vx::ObjectAssetKind::Portal) {
                portals.push_back(vx::PortalEntry { origin, placed.targetWorldId, placed.portalName });
            }
            const glm::quat rotation =
                glm::angleAxis(glm::radians(instance.yawDegrees), glm::vec3(0.0F, 1.0F, 0.0F));

            ObjectSlot slot;
            slot.id         = id;
            slot.type       = instance.type;
            slot.localMesh  = localMesh;
            slot.position   = origin;
            slot.yawDegrees = instance.yawDegrees;
            // 剔除包围盒取**旋转后**的几何（它才是世界里的真实形状；渲染侧施加同一旋转）。
            slot.bounds = BoundsOfVertices(colliderMesh.vertices, origin);
            slot.handle = renderer.UploadMesh(localMesh, origin);
            if (!slot.handle.IsValid()) {
                VX_LOG_WARN("物件 [%s] 的网格上传失败（网格为空）", instance.type->id.c_str());
            } else {
                renderer.SetMeshTransform(slot.handle, origin, rotation);
            }

            // 静态碰撞体：与渲染**共用同一份几何**（朝向烘进顶点 ⇒ "谁画谁挡"同源）。
            const std::vector<float>   positions = FlattenObjectPositions(colliderMesh);
            vx::PhysicsWorld::MeshDesc collider;
            collider.positions     = positions.data();
            collider.vertexCount   = colliderMesh.vertices.size();
            collider.indices       = colliderMesh.indices.data();
            collider.triangleCount = colliderMesh.indices.size() / 3U;
            collider.originX       = origin.x;
            collider.originY       = origin.y;
            collider.originZ       = origin.z;
            slot.body              = physics.AddMesh(collider);
            if (slot.body == 0) {
                VX_LOG_WARN("物件 [%s] 的静态碰撞体创建失败（渲染仍在 ⇒ 只会「看得见走得穿」）",
                            instance.type->id.c_str());
            }

            objectSlots.push_back(std::move(slot));
        }
        VX_LOG_INFO("物件层就绪（V0b/V0c/V3/V8）：清单 %s；放置 %zu / %zu 个物件（渲染 + 静态碰撞：其中传送门 %zu、"
                    "散布点 %zu、模型文件 %zu 个），类型表 %zu 项；可破坏总开关 = %s%s",
                    objectsPath.string().c_str(), objectSlots.size(), objectPlan.size(), portals.size(), scatterPointCount,
                    modelCache.size(), objects.types.size(), objects.destructibleEnabled ? "开" : "关",
                    (objectSkipped == 0U) ? "" : "（有落点被跳过，见上方 WARN）");

        vx::PhysicsWorld::CapsuleDesc capsule;
        capsule.radius             = kCharacterRadius;
        capsule.cylinderHalfHeight = kCharacterCylinderHalfHeight;
        capsule.maxSlopeAngleDeg   = kCharacterMaxSlopeDegrees;
        capsule.stepUpHeight       = kCharacterStepUpHeight;
        capsule.position = glm::dvec3(static_cast<double>(spawnX),
                                      static_cast<double>(spawnSurface) + static_cast<double>(kSpawnClearance),
                                      static_cast<double>(spawnZ));

        // T18：出界救援的目标即出生点脚底位置（与角色初始位置一致）。
        const glm::dvec3 spawnPosition = capsule.position;

        const vx::PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
        if (character == 0) {
            VX_LOG_ERROR("角色胶囊创建失败，无法继续");
            return EXIT_FAILURE;
        }

        const vx::DisplaySize clientSize = window.WindowSize();  // 已按设置恢复过尺寸
        vx::CameraSettings    settings;
        settings.aspectRatio    = (clientSize.height > 0)
                                      ? static_cast<float>(clientSize.width) / static_cast<float>(clientSize.height)
                                      : (16.0F / 9.0F);
        settings.followDistance = kCameraFollowDistance;
        // W6g：**肩位偏移**（over-the-shoulder）—— 注视点沿相机右方平移，主角偏出画面中心，
        // 半封闭空间里不必把悬臂塌到角色身上（业界 TPS 通行做法）。只平移注视点，**不改变朝向**。
        settings.shoulderOffset = 0.6F;
        // W6f：**球投射探针半径** —— 遮挡查询把相机近似为半径 0.25 格的球（对齐 Cinemachine `CameraRadius`），
        // 薄墙不再从相机旁边"擦过"而漏检 ⇒ 减少穿墙。
        settings.cameraProbeRadius = 0.25F;
        // W6e：**淡出主角** —— 相机与注视点近于 1.5 格起平滑淡出、到 0.4 格完全淡出（片元 Bayer 抖动 discard），
        // 取代"过近突然消失"（W6d 关闭的隐藏）与"近裁剪面切开模型看不到人物内部"。
        settings.targetFadeStartDistance = 1.5F;
        settings.targetFadeEndDistance   = 0.4F;
        // W6d：**取消**"相机过近时隐藏主角"（所有者 2026-10-06 裁定：镜头拉近时不再隐藏角色）。
        // 取 0 = 关闭该判据（`view.distance < 0` 恒 false）⇒ 主角恒提交；**能力代码保留**
        // （`CameraSettings::targetHideDistance` 与纯函数 `ShouldHideFollowTarget` 均未删除），改回 1.5 即可重新启用。
        // 当前的"贴脸穿模"由 W6e 的**淡出**负责（不再是隐藏）。
        settings.targetHideDistance = 0.0F;
        // W7-S3b：**远裁剪面**必须盖住 Ring 2 的外边界（2048 m），否则最外环会被裁掉 ⇒ 流式窗口边界露出"硬切"。
        // 取 2100（= Ring 2 外边界 2048 + 余量）。流式边界本身（常驻半径 33 tile ≈ 2112 m）由**雾**遮住：
        // 雾密度 0.003/格 ⇒ 2048 m 处遮挡 ≈ 99.8%（`assets/config/lighting.toml`；ADR 0024 的"视距 < 常驻半径"）。
        settings.farPlane = 2100.0F;

        vx::ThirdPersonCamera camera(settings);
        camera.SnapTo(glm::vec3(spawnX, static_cast<float>(capsule.position.y), spawnZ));
        camera.SetYaw(0.7F);
        camera.SetPitch(-0.42F);  // 略微俯视地表

        // 系统面板（T15，Esc）：面板就地编辑一份设置副本，主循环据此调用平台层与落盘。
        // V2b：**已上移到世界装载循环之外**（面板与用户设置是**跨世界共享**的状态，不随世界重建）。

        // 渲染原点：整数世界定位，上传的 float 顶点都以它为基准（红线 6）。
        glm::dvec3 renderOrigin(std::floor(static_cast<double>(spawnX)), std::floor(static_cast<double>(spawnSurface)),
                                std::floor(static_cast<double>(spawnZ)));

        if (!loading.Pump(LoadStage::Finalize, 1.0)) {
            VX_LOG_INFO("加载期收到退出请求（收尾阶段），退出");
            return EXIT_SUCCESS;
        }

        // T37：延后破坏队列与其执行器（爆炸只入队；重网格 / 上传 / 碰撞体重建按每帧预算推进）。
        vx::PendingDestruction pendingDestruction;
        DestructionProcessor   destructionProcessor;

        // ---- 阶段 6：网格上传（tile + 可挖体积）----
        // T36：每个网格的上传都是一次**阻塞到 GPU 完成**的拷贝；`UploadMesh` 会创建 GPU 资源并等待，
        // 而 SDL_gpu 的命令缓冲是单线程的 ⇒ 上传必须留在主线程，只能靠"每帧只传几个"来摊平。
        std::size_t volumeMeshCount = 0;
        std::size_t shellMeshCount  = 0;
        {
            const std::size_t tileCount   = tileCoords.size();
            const std::size_t volumeCount = initialVolumeCoords.size();
            const std::size_t shellCount  = shellBlockCoords.size();
            const std::size_t totalUnits  = tileCount + volumeCount + shellCount;
            std::size_t       unit        = 0;
            if (!loading.Run(LoadStage::MeshUpload, [&]() {
                    if (unit >= totalUnits) {
                        return 1.0;
                    }
                    const std::size_t batchEnd = std::min(unit + kMeshUploadsPerSlice, totalUnits);
                    for (; unit < batchEnd; ++unit) {
                        if (unit < tileCount) {
                            UploadResidentTile(tileResidency, unit);
                        } else if (unit < tileCount + volumeCount) {
                            const std::size_t index = unit - tileCount;
                            UploadVolumeMeshAt(volumeSlots, renderer, digVolumes, initialVolumeCoords[index]);
                            const auto uploaded = volumeSlots.Find(initialVolumeCoords[index]);
                            if (uploaded != volumeSlots.end() && uploaded->second.handle.IsValid()) {
                                ++volumeMeshCount;
                            }
                        } else {
                            // W4：地表壳块 —— 上传**碰撞用的同一份**网格（谁画谁挡同源）；无表面则跳过。
                            const std::size_t     index = unit - tileCount - volumeCount;
                            const vx::MeshData&   mesh  = shellBlockMeshes[index];
                            if (!mesh.indices.empty()) {
                                const vx::BlockCoord& block = shellBlockCoords[index];
                                const glm::dvec3 origin(static_cast<double>(vx::BlockOriginBlocks(block.x)),
                                                        static_cast<double>(vx::BlockOriginBlocks(block.y)),
                                                        static_cast<double>(vx::BlockOriginBlocks(block.z)));
                                shellHandles[index] =
                                    renderer.UploadMesh(mesh, origin, /*emissive=*/false,
                                                        /*reserveVertexCount=*/0, /*reserveIndexCount=*/0,
                                                        /*depthBiased=*/false);
                                shellBounds[index] = BoundsOfVertices(mesh.vertices, origin);
                                ++shellMeshCount;
                            }
                        }
                    }
                    return static_cast<double>(unit) / static_cast<double>(totalUnits);
                })) {
                VX_LOG_INFO("加载期收到退出请求（网格上传阶段），退出");
                return EXIT_SUCCESS;
            }
        }

        // ---- W6：水面网格（近场；与河道同源）—— 独立管线（flow 着色 + 半透明），在主通道最后绘制 ----
        vx::MeshHandle waterHandle;
        std::size_t    waterTriangles = 0;
        if (!riverPath.Empty()) {
            const vx::MeshData waterMesh =
                vx::BuildRiverWaterMesh(riverPath, shellRegion.minColumnX, shellRegion.minColumnZ);
            if (!waterMesh.indices.empty()) {
                const glm::dvec3 waterOrigin(static_cast<double>(shellRegion.minColumnX), 0.0,
                                             static_cast<double>(shellRegion.minColumnZ));
                waterHandle = renderer.UploadMesh(waterMesh, waterOrigin, /*emissive=*/false,
                                                  /*reserveVertexCount=*/0, /*reserveIndexCount=*/0,
                                                  /*depthBiased=*/false, /*water=*/true);
                waterTriangles = waterMesh.indices.size() / 3U;
            }
        }
        VX_LOG_INFO("水面网格（W6 / ADR 0027）：%zu 个三角形（flow 滚动波 + 半透明；河床碰撞随地表壳承担）",
                    waterTriangles);

        VX_LOG_INFO("可挖体积网格已上传：%zu/%zu 个块有可见表面（其余块全实心或全空，无等值面）", volumeMeshCount,
                    volumeSlots.Size());
        VX_LOG_INFO("地表壳网格已上传：%zu/%zu 个块（Surface Nets；与碰撞体同一份数据）", shellMeshCount,
                    shellBlockCoords.size());

        // T13 / T69：主角**可视**体（装饰用，不参与任何物理，尺寸与碰撞胶囊一致）。
        // T69：优先用 CC0 **占位模型**（Quaternius《Casual Female》，蒙皮 + 骨骼动画，见 plans/v0.3.md §1.3）；
        // **资源不入库** ⇒ 干净克隆 / 缺文件时**回落到程序化胶囊**（P6 的硬要求：缺资源也必须照常启动）。
        vx::CapsuleMeshSpec capsuleSpec;
        capsuleSpec.radius             = kCharacterRadius;
        capsuleSpec.cylinderHalfHeight = kCharacterCylinderHalfHeight;
        const vx::MeshData          capsuleLocalMesh  = vx::BuildCapsuleMesh(capsuleSpec);
        std::vector<vx::MeshVertex> characterVertices = capsuleLocalMesh.vertices;

        vx::MeshHandle           characterMesh;
        bool                     characterUsesModel = false;
        vx::Model                characterModel;
        vx::CharacterSkinnedMesh characterSkinned;
        glm::vec3                characterLocalPivot { 0.0F };
        // 四个动画状态各对应的 clip（名字由 `ClipNameForCharacterState` 决定；查不到时为 nullptr）。
        const vx::AnimationClip* characterClips[4] = { nullptr, nullptr, nullptr, nullptr };
        std::vector<float>       characterBoneMatrices;  // 每帧待上传的骨骼矩阵（一次分配、之后只 memcpy）
        float                    characterAnimTime  = 0.0F;
        vx::CharacterAnimState   characterAnimState = vx::CharacterAnimState::Idle;
        // T86：角色朝向（绕 +Y 的 yaw，与相机同口径）。只在**检测到移动**时按角速度上限靠拢；
        // 静止时保持不变。在固定步内推进（红线 11），渲染时作为蒙皮网格的旋转下发。
        float                    characterYaw      = 0.0F;
        {
            const std::filesystem::path modelPath = SourceAssetPath("assets/models/character/Casual_Female.glb");
            try {
                characterModel   = vx::LoadModel(modelPath);
                characterSkinned = vx::BuildSkinnedMeshFromModel(characterModel);
                if (characterSkinned.mesh.vertices.empty() || characterSkinned.mesh.indices.empty() ||
                    characterModel.joints.empty() || characterModel.joints.size() > vx::kMaxSkinJoints) {
                    throw std::runtime_error("模型无可用网格或骨架（或关节数超出 kMaxSkinJoints）");
                }
                characterMesh = renderer.UploadSkinnedMesh(characterSkinned.mesh, renderOrigin,
                                                           static_cast<std::uint32_t>(characterModel.joints.size()));
                characterUsesModel  = characterMesh.IsValid();
                characterLocalPivot = characterSkinned.localPivot;
            } catch (const std::exception& error) {
                characterUsesModel = false;
                VX_LOG_WARN("占位主角模型不可用 ⇒ **回落程序化胶囊**（资源不入库，请先执行 tools/fetch_assets.ps1）：%s",
                            error.what());
            }
        }
        if (characterUsesModel) {
            for (int state = 0; state < 4; ++state) {
                const char* clipName  = vx::ClipNameForCharacterState(static_cast<vx::CharacterAnimState>(state));
                characterClips[state] = vx::FindAnimationClip(characterModel, clipName);
                if (characterClips[state] == nullptr) {
                    VX_LOG_WARN("占位主角模型缺少动画 `%s` ⇒ 该状态保持绑定姿态（占位口径，见 plans/v0.3.md §1.4）",
                                clipName);
                }
            }
            VX_LOG_INFO("占位主角模型已启用：顶点 %zu / 索引 %zu / 关节 %zu（**占位、外形待定**；来源见 NOTICE.md 台账）",
                        characterSkinned.mesh.vertices.size(), characterSkinned.mesh.indices.size(),
                        characterModel.joints.size());
        } else if (!characterMesh.IsValid()) {
            // 回落路径：程序化胶囊（先上传局部网格；之后每帧就地刷渲染相对顶点）。
            characterMesh = renderer.UploadMesh(capsuleLocalMesh, renderOrigin);
        }
        if (!characterMesh.IsValid()) {
            VX_LOG_WARN("主角可视网格上传失败（网格为空），本帧起将看不到角色");
        }

        // T27：光球（弹丸）——每种槽位一份网格（自发光），每帧只刷新位置；池容量来自配置。
        renderer.SetEmissiveColor(orbSpec.emissiveRgb[0], orbSpec.emissiveRgb[1], orbSpec.emissiveRgb[2]);
        vx::OrbPool orbPool(static_cast<std::size_t>(projectiles.MaxActive()));
        const vx::MeshData                orbLocalMesh = vx::BuildOrbMesh(orbSpec.radius);
        std::vector<vx::MeshHandle>       orbHandles;
        std::vector<std::vector<vx::MeshVertex>> orbVertices;  // 每槽一份可写顶点副本（就地刷新）
        orbHandles.reserve(orbPool.Capacity());
        orbVertices.reserve(orbPool.Capacity());
        for (std::size_t i = 0; i < orbPool.Capacity(); ++i) {
            // T41：与主角同约定 —— 每帧按当前渲染原点烘焙渲染相对顶点（原点传当前渲染原点）。
            orbHandles.push_back(renderer.UploadMesh(orbLocalMesh, renderOrigin, /*emissive=*/true));
            orbVertices.push_back(orbLocalMesh.vertices);
        }
        if (orbHandles.empty() || !orbHandles.front().IsValid()) {
            VX_LOG_WARN("光球网格上传失败（网格为空），本帧起将看不到弹丸（爆炸仍会生效）");
        }
        VX_LOG_INFO("光球就绪：[%s]，池容量 %zu，自发光色 (%.2f, %.2f, %.2f)（线性光）", orbSpec.id.c_str(),
                    orbPool.Capacity(), static_cast<double>(orbSpec.emissiveRgb[0]),
                    static_cast<double>(orbSpec.emissiveRgb[1]), static_cast<double>(orbSpec.emissiveRgb[2]));

        // T27：弹道世界查询 + 爆炸写回上下文（两者都只在固定步内使用）。
        // T41 起上传不再依赖渲染原点（顶点是网格局部坐标），故上下文不含 `renderOrigin`。
        const GameOrbWorldQuery orbQuery(world, digVolumes, physics);
        // 缺陷修复（人工实测第 7 轮）：相机的地形查询必须**包含可挖体积**（否则站在洞里的角色会把相机顶出洞外）。
        const GameCameraQuery   cameraQuery(world, digVolumes);
        WorldEditContext        editContext { world,
                                              digVolumes,
                                              terrainCollision,
                                              volumeCollision,
                                              collapseSpec,
                                              destructionSpec,
                                              renderer,
                                              tileCoords,
                                              tileHandles,
                                              volumeSlots,
                                              tileBounds,
                                              pendingDestruction,
                                              physics,
                                              rigidCollapse };

        // 每帧的绘制列表（tile + 体积 + 主角 + 活动光球 + **倒塌整体**）：容量固定，稳态零分配。
        std::vector<vx::MeshHandle> frameHandles;
        frameHandles.reserve(tileHandles.size() + volumeSlots.Size() + 1 + orbHandles.size() +
                             objectSlots.size() + static_cast<std::size_t>(collapseSpec.maxActiveUnits));

        /// T33：本帧**落定**（倒了、停住了）的倒塌整体 —— 帧末统一体素化回写（缓冲复用，稳态零分配）。
        std::vector<vx::ActiveCollapseUnit> settledCollapseUnits;
        settledCollapseUnits.reserve(static_cast<std::size_t>(collapseSpec.maxActiveUnits));

        vx::Clock                clock;
        vx::FixedStepAccumulator accumulator(vx::kFixedDt);
        // T24：CPU 帧时间分解的相位计时器（逻辑步 / UI 构建 / 渲染提交）。
        PhaseTimer   logicTimer;
        PhaseTimer   uiTimer;
        PhaseTimer   renderTimer;
        // T45：把原先落在"未计时区"的三段显式量出来 —— **动态顶点上传 / uniform 构建 / 限帧与呈现**。
        // 起因：一次 61.4 ms 尖峰里三相 CPU 只占 22.2 ms（≈39 ms 归属不明）⇒ 现有日志给出的
        // "主要受限在谁"可能是误判（SKILL 第四节「观测先于结论」）。
        PhaseTimer   dynamicUploadTimer;
        PhaseTimer   uniformTimer;
        PhaseTimer   throttleTimer;
        // T79②（T74 打点拆分）：把剩下三段**原先没被计量的每帧工作**也量出来 ——
        // **输入与事件**（`pump_events` + `input.BeginFrame`）、**剔除与绘制列表构建**、**渲染原点重定基**。
        // 这三段就是尖峰日志里"未计时"的主要来源（实测出现过 100+ ms 的未计时，见 `docs/devlog.md` T72 条目）。
        PhaseTimer   inputTimer;
        PhaseTimer   cullTimer;
        PhaseTimer   rebaseTimer;
        /// T79②：**本帧**计时器（帧首 → 帧末，含限帧）—— 尖峰日志的 `frameMs` 用它，保证与各段**同一区间**。
        PhaseTimer   frameTimer;
        /// 本帧各段耗时（毫秒）。尖峰日志移到**帧末**打印 ⇒ 读到的都是本帧的值（见帧首的采样点说明）。
        double       inputMs  = 0.0;
        double       cullMs   = 0.0;
        double       rebaseMs = 0.0;
        /// 本帧的**限帧**耗时（毫秒；日志在帧末打印 ⇒ 是本帧的值）。
        double       throttleMs = 0.0;
        CpuFrameCost cpuCost;  // 本帧的值（帧末的尖峰日志与下一帧的面板共用）

        // ---- 加载结束（T36）：恢复玩家设置的帧率上限 / 呈现模式，并撤下加载画面。----
        // 自此进入稳态主循环：加载期"先给画面、再给结果"的阶段到此结束。
        ApplyFrameRateCap(window, frameLimiter, systemSettings.frameRateCap, displayRefreshRate);
        debugOverlay.ClearLoadingStatus();

        // T14：鼠标捕获（相对模式）状态。game 只持有**意图**，SDL 的真实状态由平台层维护
        // （窗口失焦时平台层会自动释放，见 `Window::pump_events`）。启动即捕获，玩家一进游戏就能转视角。
        bool mouseCaptured = window.SetRelativeMouseMode(true);
        VX_LOG_INFO("鼠标捕获：%s（相对模式：光标隐藏、鼠标位移不再受屏幕边界限制；Esc = 开关系统面板，"
                    "打开面板时释放捕获、关闭时恢复；点击窗口 = 重新捕获）",
                    mouseCaptured ? "开" : "关（SDL 未接受，稍后点击窗口重试）");

        // T15：系统面板**打开前**的捕获状态记忆（关闭面板时据此恢复）。与 `mouseCaptured` 同属一套捕获状态机。
        bool captureBeforePanel = false;

        // 飞行模式开关状态（T12）；切换时清零速度，避免残留速度把角色弹飞。
        bool flying = false;

        // W7-S4：`--autofly=<秒>` 的剩余秒数与"已启动"标记（缺省 0 ⇒ 全程不生效）。
        double autoFlyRemainingSeconds = autoFlySeconds;
        bool   autoFlyStarted          = false;

        // T27：重新捕获鼠标的那一次点击**不落到发射上**（直到松开按键）。左键改为按住连发后，
        // 若只在按下帧抑制，按住不放会在下一帧立刻发射，等于把"捕获点击"变成了开火；
        // 故用锁存：捕获点击被消费时置位，左键松开时清零。
        bool fireSuppressUntilRelease = false;

        // T27：光球连发冷却（秒）。只在**固定步**内递减（红线 11：不用可变帧间隔驱动玩法节奏）。
        float fireCooldown = 0.0F;

        // T54：土狼时间 / 跳跃缓冲（跨固定步持有；只在**固定步**内推进，红线 11）。
        vx::JumpAssist jumpAssist;

        // T28 自检（一次性）：把地面碰撞换成**体积三角网**之后，"角色真的站在体积面上"必须可观测 ——
        // 启动后 2 秒打一行日志（脚底高度 / 是否着地），否则"掉进地下"这类失败只会表现为画面异常。
        bool  groundCheckLogged = false;
        float simulatedSeconds  = 0.0F;

        // 跳跃请求**帧级锁存**（缺陷 B3）：主循环在 Mailbox 下可达上千 FPS，而逻辑 / 物理是 60 Hz 固定步，
        // 多数帧的 `StepPlan::steps` 为 0。若在帧边界直接消费"本帧按下"边沿，该边沿会在没有逻辑步的帧上
        // 被静默丢弃（实测 ~1500 FPS 时只有 ~4% 的帧有逻辑步 → 96% 的空格按下丢失）。故先锁存，
        // 交给**第一个真正执行的固定步**，再由该步消费掉。
        bool jumpRequested = false;

        VX_LOG_INFO("地表世界就绪：种子 %llu，tile %zu 个，材质表 schema_version=%d",
                    static_cast<unsigned long long>(preset.seed), tileCoords.size(), materials.SchemaVersion());
        VX_LOG_INFO("角色物理就绪：地表碰撞体 %zu 个 tile + 体积碰撞体 %zu 个（ADR 0012 碰撞接管）；"
                    "胶囊 半径 %.2f / 总高 %.2f 格；"
                    "重力 %.1f、跳跃初速 %.2f（由身高推导，最高点 %.2f 格 = 身高 %.0f%%）、"
                    "最大坡度 %.0f°、自动上台阶 %.1f 格（dt=1/60）",
                    collisionTiles, volumeCollision.BlockBodyCount(), static_cast<double>(kCharacterRadius),
                    static_cast<double>(kCharacterHeight),
                    static_cast<double>(kGravity), static_cast<double>(kJumpSpeed),
                    static_cast<double>(vx::kJumpApexHeightRatio * kCharacterHeight),
                    static_cast<double>(vx::kJumpApexHeightRatio * 100.0F),
                    static_cast<double>(kCharacterMaxSlopeDegrees), static_cast<double>(kCharacterStepUpHeight));
        VX_LOG_INFO("ImGui 已启用（事件转发已接）：F1 调试面板（当前%s）；Esc 系统面板（当前%s）；两个面板均可交互",
                    debugOverlay.Visible() ? "显示" : "隐藏", debugOverlay.SystemPanelOpen() ? "打开" : "关闭");
        VX_LOG_INFO("控制说明：W/A/S/D = 移动；Shift = 冲刺；Space = 跳（飞行中 = 上升）；"
                    "F = 切换飞行模式；飞行中 左Ctrl = 下降（Shift 加速）；鼠标移动 = 环视（已捕获，可转满 ±89°）；"
                    "**鼠标左键 = 发射光球[%s]**（按住连发，冷却 %.2f 秒；弹道受重力影响，重力系数 %.2f）；"
                    "**光球命中物体表面即爆炸**：射入可挖区域（测试地图西南的山体）⇒ **在山体上挖出洞**；"
                    "射在区域外的地面 ⇒ 炸出坑（半径 %.1f 格 / 深 %.1f / 外环 %.1f）；"
                    "（原鼠标挖 / 堆 / 爆破笔刷已解绑：地形破坏只由光球触发）；"
                    "**E = 走近传送门时传送**（HUD 出提示后按 E 切换到门的目标世界；**不自动切换**）；"
                    "Esc = 开关系统面板（打开时释放鼠标、关闭时恢复）；"
                    "点击窗口 = 重新捕获（**该次点击不会发射**）；F1 = 调试面板；关闭窗口 = 退出",
                    orbSpec.id.c_str(), static_cast<double>(orbSpec.fireIntervalSeconds),
                    static_cast<double>(orbSpec.gravityScale), static_cast<double>(orbSpec.explosionRadiusBlocks),
                    static_cast<double>(orbSpec.explosionDepthBlocks), static_cast<double>(orbSpec.explosionRimBlocks));

        // T38：帧尖峰打点所需的状态（跨帧保持）。
        vx::Clock           hitchLogClock;           ///< 尖峰日志的节流时钟（最多每 `kHitchLogMinIntervalMs` 一条）
        bool                cullingLogged = false;   ///< T39：剔除结果只打一条日志（启动后第一次提交时）

        // T39：剔除所需的**太阳方向**（来自配置、不随帧变化，只算一次）。影子往太阳的反方向落。
        const glm::vec3 sunDirection = [&lighting]() {
            const glm::vec3 direction(lighting.Sun().direction[0], lighting.Sun().direction[1],
                                      lighting.Sun().direction[2]);
            const float     length = glm::length(direction);
            return (length > 0.0F) ? (direction / length) : glm::vec3(0.0F, 1.0F, 0.0F);
        }();

        // W6：水面流动时间（秒）—— 累加**固定步**时间（与物理同步 ⇒ 确定性，不受帧率影响）。
        double waterTimeSeconds = 0.0;

        while (true) {
            // T79②（T74 打点拆分，**先量后改**）：帧周期的采样点必须在**帧首**。
            // 原实现在**逻辑相位内**采样（`clock.Tick()` 在 `logicTimer` 里），于是 frameMs 恒覆盖
            // "上一帧尾 + 本帧头"、却**跳过本帧的逻辑相位**（实测表现为"逻辑 116 ms 与未计时 108 ms 交替出现"）
            // ⇒ 打印出来的"未计时"是两帧错位后的残差，不能用来定位。
            // 现在：帧首采样（供固定步推进）+ **另起一个本帧计时器**（帧首 → 帧末）⇒ 尖峰日志里的
            // 各段之和与 `frameMs` **同一区间**，残差才是真的没被计量的部分。
            const double frameDeltaSeconds = clock.Tick();
            frameTimer.Begin();

            inputTimer.Begin();
            if (!window.pump_events(input)) {
                quitRequested = true;  // V2b：本轮结束后退出外层"世界装载循环"
                break;  // 窗口关闭 / 收到退出事件（与旧 `while (window.pump_events(...))` 等价）
            }
            input.BeginFrame();  // 每帧采样一次，且只在固定步循环之外
            inputMs = inputTimer.EndMs();

            // T14：平台层在窗口失焦时会自动释放相对模式（见 `Window::pump_events`）；这里把 game 的意图同步过来并记日志。
            // **不**在此自动重新捕获：焦点恢复必须靠用户的显式点击，否则光标会自己消失，令人困惑。
            if (mouseCaptured && !window.IsRelativeMouseMode()) {
                mouseCaptured = false;
                VX_LOG_INFO("鼠标捕获：关（窗口失焦，平台层已自动释放；点击窗口可重新捕获）");
            }

            // T15：Esc 的语义已统一为"开关系统面板"（不再单独承担"释放鼠标"）。
            // 打开面板 → 释放捕获并记住打开前状态；关闭面板 → 恢复到打开前状态。
            // 与 T14 的捕获状态机共存于 `mouse_capture.hpp`，不是第二套机制。
            if (input.ConsumePressed(vx::ActionId::ToggleSystemPanel)) {
                if (debugOverlay.PortalMenuOpen()) {
                    // V9：Esc 先关**传送门菜单**（与系统面板同一套捕获语义），**不**顺带打开系统面板。
                    debugOverlay.ClosePortalMenu();
                    const vx::PanelCaptureTransition transition =
                        vx::DecidePanelCaptureTransition(/*opening=*/false, captureBeforePanel);
                    if (transition.captureRequested) {
                        mouseCaptured = window.SetRelativeMouseMode(true);
                    }
                    jumpRequested = false;
                    VX_LOG_INFO("传送门菜单（V9）：关闭（Esc）；鼠标捕获：%s",
                                mouseCaptured ? "开（已恢复打开前状态）" : "关");
                } else {
                    const bool opening = !debugOverlay.SystemPanelOpen();
                    debugOverlay.ToggleSystemPanel();
                    const vx::PanelCaptureTransition transition =
                        vx::DecidePanelCaptureTransition(opening, opening ? mouseCaptured : captureBeforePanel);
                    if (transition.rememberCaptureState) {
                        captureBeforePanel = mouseCaptured;
                    }
                    if (transition.releaseRequested) {
                        (void)window.SetRelativeMouseMode(false);
                        mouseCaptured = false;
                    }
                    if (transition.captureRequested) {
                        mouseCaptured = window.SetRelativeMouseMode(true);
                    }
                    jumpRequested = false;  // 面板开关不应遗留锁存的跳跃请求
                    VX_LOG_INFO("系统面板：%s；鼠标捕获：%s", opening ? "打开（置于屏幕中央）" : "关闭",
                                mouseCaptured ? "开（已恢复打开前状态）" : "关（光标可见，可点击窗口重新捕获）");
                }
            }

            // 起 ImGui 帧（任一面板可见时）：必须先于读取捕获标志，且早于玩法输入处理。
            // 捕获期间让 ImGui 忽略鼠标（相对模式坐标无意义），避免误判悬停而抑制视角。
            // T24：ImGui 帧开销计入 UI 构建耗时（NewFrame 与面板构建是两段，累加）。
            uiTimer.Begin();
            debugOverlay.SetGameplayMouseCaptured(mouseCaptured);
            debugOverlay.BeginFrame();
            double uiMs = uiTimer.EndMs();

            // T15：玩法输入抑制——面板打开或 ImGui 想接管鼠标 / 键盘时，吞掉对应类别，
            // 使"点按钮"不会挖地、"拖音量"不会转相机。决策为纯函数（见 `gameplay_input.hpp`）。
            // V9：**任一面板打开**（系统面板 或 传送门菜单）都全量抑制玩法输入。
            const vx::InputSuppression suppression =
                vx::DecideInputSuppression(debugOverlay.AnyBlockingPanelOpen(), debugOverlay.WantsCaptureMouse(),
                                           debugOverlay.WantsCaptureKeyboard());

            // T14 捕获状态机（仅在**任一面板关闭**时）：未捕获时的点击用于重新捕获，状态机把它标记为
            // "已被捕获消费"，随后消费掉鼠标左键边沿，使这次点击绝不会落到发射上。
            // 面板（系统面板 / V9 传送门菜单）打开时整体跳过：此时点击属于面板控件，绝不能触发重捕获。
            // 该顺序由单测钉死。
            if (!debugOverlay.AnyBlockingPanelOpen()) {
                const bool anyClickEdge = input.Pressed(vx::ActionId::Attack);
                // `escapePressed` 恒为 false：Esc 已改由上面的系统面板消费（T15 统一语义）。
                const vx::MouseCaptureDecision captureDecision =
                    vx::DecideMouseCapture(mouseCaptured, /*escapePressed=*/false, anyClickEdge);
                if (captureDecision.captureRequested) {
                    mouseCaptured = window.SetRelativeMouseMode(true);
                    VX_LOG_INFO("鼠标捕获：%s（点击重新捕获；本次点击已被捕获消费，不发射光球）",
                                mouseCaptured ? "开" : "关（SDL 未接受，请再点一次）");
                }
                if (captureDecision.clickConsumedByCapture) {
                    // 消费本帧的鼠标点击边沿：重新捕获的这一次点击到此为止，绝不落到发射上。
                    (void)input.ConsumePressed(vx::ActionId::Attack);
                    fireSuppressUntilRelease = true;  // 直到松开按键才解除（见其声明处说明）
                }
            }

            // 相机环绕：模拟量每帧消费一次（无论是否捕获都要消费，避免位移残留到下一帧）。
            // T14/T15：未捕获、或 ImGui 正在接管鼠标（拖滑块 / 悬停面板）时丢弃位移、不转视角。
            const float lookX = input.ConsumeValue(vx::ActionId::LookX);
            const float lookY = input.ConsumeValue(vx::ActionId::LookY);
            if (mouseCaptured && !suppression.cameraLook) {
                camera.AddYaw(-lookX * kLookSensitivity);
                camera.AddPitch(-lookY * kLookSensitivity);
            }

            // 调试面板开关：本帧按下边沿消费一次。F1 属调试设施，不受面板输入抑制影响。
            if (input.ConsumePressed(vx::ActionId::ToggleDebugPanel)) {
                debugOverlay.Toggle();
                VX_LOG_INFO("调试面板：%s", debugOverlay.Visible() ? "显示" : "隐藏");
            }

            // 飞行模式开关（T12）：本帧按下边沿消费一次；切换瞬间清零速度，
            // 使"飞行 → 普通"不残留速度（不会把角色弹飞），"普通 → 飞行"无残余下落。
            // T15：ImGui 接管键盘时（如在控件上打字）不得切换飞行。
            const bool flyToggleEdge = input.ConsumePressed(vx::ActionId::ToggleFly);
            if (flyToggleEdge && !suppression.keyboardGameplay) {
                flying = !flying;
                physics.SetCharacterVelocity(character, glm::vec3(0.0F));
                VX_LOG_INFO("飞行模式：%s",
                            flying ? "开（无重力，Space 上升 / 左Ctrl 下降）" : "关（恢复重力与碰撞）");
            }

            // T27：发射意图在**帧边界**采样一次（按住 = 连发，由固定步内的冷却控制射速）。
            // 这里仍消费"本帧按下"边沿，避免边沿残留到下一帧被重复消费。
            (void)input.ConsumePressed(vx::ActionId::Attack);
            if (!input.Held(vx::ActionId::Attack)) {
                fireSuppressUntilRelease = false;  // 松开左键即解除"捕获点击"抑制
            }
            const bool fireHeld = mouseCaptured && !suppression.mouseAction && !fireSuppressUntilRelease &&
                                  input.Held(vx::ActionId::Attack);
            // 瞄准方向每帧算一次（相机视线；见 `AimDirection` 的说明），供本帧全部固定步复用。
            const glm::vec3 aimDirection = fireHeld ? AimDirection(camera, cameraQuery) : glm::vec3(0.0F);

            // 空格跳跃：本帧按下边沿先**锁存**，不在此帧边界丢弃（缺陷 B3，见 jumpRequested 的说明）。
            // T14/T15：未捕获、或 ImGui 接管键盘时不接受移动 / 跳跃输入；
            // 同时清掉可能残留的锁存请求，避免重新捕获的那一帧凭空起跳。
            if (mouseCaptured && !suppression.keyboardGameplay) {
                jumpRequested = jumpRequested || input.ConsumePressed(vx::ActionId::Jump);
            } else {
                jumpRequested = false;
            }

            // 移动输入本帧只读一次，供本帧全部固定逻辑步复用；未捕获或被抑制时保持全零指令（不移动）。
            MoveCommand command;
            if (mouseCaptured && !suppression.keyboardGameplay) {
                command.forward = (input.Held(vx::ActionId::MoveForward) ? 1.0F : 0.0F) -
                                  (input.Held(vx::ActionId::MoveBackward) ? 1.0F : 0.0F);
                command.strafe = (input.Held(vx::ActionId::MoveRight) ? 1.0F : 0.0F) -
                                 (input.Held(vx::ActionId::MoveLeft) ? 1.0F : 0.0F);
                command.speed = input.Held(vx::ActionId::Sprint) ? kSprintSpeed : kWalkSpeed;
                command.jump  = jumpRequested;  // 锁存的跳跃请求（见 jumpRequested 的说明）
                command.vertical = (input.Held(vx::ActionId::Jump) ? 1.0F : 0.0F) -
                                   (input.Held(vx::ActionId::FlyDown) ? 1.0F : 0.0F);
                command.flySpeed = input.Held(vx::ActionId::Sprint) ? (kFlySpeed * 2.0F) : kFlySpeed;
            }

            // W7-S4：自动化飞行（`--autofly=<秒>`；缺省 0 ⇒ 本段整体不生效，玩法与从前逐值一致）。
            // 语义：强制飞行 + **持续前进 + 持续上升**（沿世界顶盖下方横穿全图，不受地形起伏阻挡），
            // 到时自动交还控制。用于给"10km 飞越 / 常驻量只随窗口变化 / P99"提供可脚本化证据。
            if (autoFlyRemainingSeconds > 0.0) {
                if (!autoFlyStarted) {
                    autoFlyStarted = true;
                    flying         = true;
                    physics.SetCharacterVelocity(character, glm::vec3(0.0F));
                    VX_LOG_INFO("自动飞行测试（--autofly）：强制飞行 + 前进 + 上升，持续 %.1f 秒（W7-S4 实测用）",
                                autoFlySeconds);
                }
                command.forward  = 1.0F;
                command.vertical = 1.0F;
                command.speed    = kFlySpeed;
                command.flySpeed = kFlySpeed;
                autoFlyRemainingSeconds -= frameDeltaSeconds;
                if (autoFlyRemainingSeconds <= 0.0) {
                    VX_LOG_INFO("自动飞行测试（--autofly）：**结束**（已飞行 %.1f 秒；此后交还玩家控制）",
                                autoFlySeconds);
                }
            }

            // T24：逻辑步相位（固定步循环：物理 + 相机 + 光球 + 出界检查）。
            logicTimer.Begin();
            // T79②：帧间隔在**帧首**采到（`frameDeltaSeconds`）⇒ 固定步推进与帧时间同源，
            // 不再依赖"逻辑相位内再采一次"（那会让帧时间与相位分解错位一帧）。
            const vx::StepPlan plan = accumulator.Advance(frameDeltaSeconds);
            bool                 terrainExplosionSeen  = false;
            settledCollapseUnits.clear();
            // V0c：物件支撑检查的固定步节拍（成本与物件数成正比 ⇒ 不每步做）。
            int objectSupportStepCounter = 0;
            for (int step = 0; step < plan.steps; ++step) {
                StepCharacter(physics, character, camera, command, flying, jumpAssist);

                // T33：推进**动态刚体**（倒塌整体）—— 重力已在启动时设为 `-kGravity`（与角色同一口径）。
                // 随后做落定检测：连续 `settle_steps` 个固定步低于阈值 ⇒ 转为"待回写"（本帧末体素化回写）。
                physics.Update(static_cast<float>(vx::kFixedDt));
                rigidCollapse.Step(physics, collapseSpec, settledCollapseUnits);

                // T69：主角动画状态与局部时间按**固定步**推进（红线 11：动画不随帧率漂移）。
                // 换状态 ⇒ 局部时间归零（占位口径：不做动画混合 / 过渡）。
                if (characterUsesModel) {
                    const vx::PhysicsWorld::CharacterState animationSource = physics.GetCharacterState(character);
                    const float horizontalSpeed = std::sqrt(animationSource.velocity.x * animationSource.velocity.x +
                                                            animationSource.velocity.z * animationSource.velocity.z);
                    // T86：朝向跟随移动方向 —— 由**水平速度方向**求目标 yaw，按角速度上限平滑靠拢；
                    // 静止（低于阈值）时**保持当前朝向**。固定步推进 ⇒ 与帧率无关（红线 11）。
                    float targetYaw = 0.0F;
                    if (vx::TryComputeTargetYaw(animationSource.velocity.x, animationSource.velocity.z, targetYaw)) {
                        characterYaw = vx::AdvanceYawTowards(
                            characterYaw, targetYaw, vx::kCharacterTurnRateRadPerSec * static_cast<float>(vx::kFixedDt));
                    }
                    const vx::CharacterAnimState nextState = vx::SelectCharacterAnimState(
                        animationSource.onGround, horizontalSpeed, animationSource.velocity.y);
                    if (nextState != characterAnimState) {
                        characterAnimState = nextState;
                        characterAnimTime  = 0.0F;
                    } else {
                        characterAnimTime += static_cast<float>(vx::kFixedDt);
                        // 循环动作（Idle / Run）在 `[0, duration)` 上回绕；否则采样会把时间钳位到末尾，
                        // 角色停在最后一帧 ⇒ 走动时看着像"原地滑步"（一次性动作不循环，见 `LoopsCharacterAnimation`）。
                        const vx::AnimationClip* currentClip = characterClips[static_cast<int>(characterAnimState)];
                        if (vx::LoopsCharacterAnimation(characterAnimState) && currentClip != nullptr &&
                            currentClip->duration > 0.0F) {
                            characterAnimTime = std::fmod(characterAnimTime, currentClip->duration);
                        }
                    }
                }

                // T28 自检：2 秒后打一次角色落地状态（见 `groundCheckLogged` 的说明）。
                simulatedSeconds += static_cast<float>(vx::kFixedDt);
                if (!groundCheckLogged && simulatedSeconds >= 2.0F) {
                    groundCheckLogged = true;
                    const vx::PhysicsWorld::CharacterState landed = physics.GetCharacterState(character);
                    VX_LOG_INFO("角色落地自检（T28 碰撞接管后）：脚底 (%.2f, %.2f, %.2f)，着地=%s",
                                landed.position.x, landed.position.y, landed.position.z,
                                landed.onGround ? "是" : "否");
                }

                // T18 出界救援：墙挡不住"飞越墙顶后坠落"，故在**每个固定步后**检查角色是否已掉出世界。
                // 命中则重用 `SetCharacterPosition`（内部会把位置瞬移并清零速度）送回出生点，并
                // `SnapTo` 相机以消除插值拖影。送回后位置落在边界盒内，下一帧不会再触发
                // （该性质由 `OutOfBounds.SingleRescueDoesNotRetriggerOnNextFrame` 钉死）——
                // 因此日志天然"每次出界只记一次"，不会逐帧刷屏。
                if (vx::IsCharacterOutOfBounds(physics.GetCharacterState(character).position, bounds,
                                               vx::kOutOfBoundsMargin)) {
                    physics.SetCharacterPosition(character, spawnPosition);
                    jumpAssist.Reset();  // T54：瞬移不该把"土狼时间 / 跳跃缓冲"带过去
                    camera.SnapTo(glm::vec3(static_cast<float>(spawnPosition.x),
                                            static_cast<float>(spawnPosition.y),
                                            static_cast<float>(spawnPosition.z)));
                    VX_LOG_WARN("角色出界（超出边界 %.0f 格余量）→ 已送回出生点 (%.1f, %.1f, %.1f)",
                                vx::kOutOfBoundsMargin, spawnPosition.x, spawnPosition.y, spawnPosition.z);
                }

                // T27：光球——发射（受射速冷却限制）、按**固定步长**推进弹道、命中即爆炸。
                // 全部发生在固定步内：弹道与重力不随帧率漂移（红线 11）。
                fireCooldown = std::max(0.0F, fireCooldown - static_cast<float>(vx::kFixedDt));
                if (fireHeld && fireCooldown <= 0.0F) {
                    const vx::PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
                    const glm::dvec3 muzzle(state.position.x + static_cast<double>(aimDirection.x) * kMuzzleForwardOffset,
                                            state.position.y + static_cast<double>(kCharacterHeight * kMuzzleHeightRatio),
                                            state.position.z + static_cast<double>(aimDirection.z) * kMuzzleForwardOffset);
                    if (orbPool.Fire(muzzle, aimDirection, orbSpec)) {
                        fireCooldown = orbSpec.fireIntervalSeconds;
                    }
                }
                for (vx::Orb& orb : orbPool.Orbs()) {
                    vx::OrbHit hit;
                    if (vx::StepOrb(orb, orbQuery, kGravity, orbSpec.gravityScale, static_cast<float>(vx::kFixedDt), hit)) {
                        // T50（ADR 0018 决策二）：命中的若是**动态刚体**（掉落中的整体 **或** 保留中的岩石残骸），
                        // 就**在它自己的补丁上雕刻** —— 球体挖除 + 同一份 Surface Nets 重网格 + 按剩余体素
                        // **原地重建刚体**，全程**不切换表示**。
                        // 为什么不再体素化回写（T46/T48 的旧路径）：体素化是"中心 → 最近整数格"的硬量化 + 找空位，
                        // 会把整块轮廓改掉 —— 而"岩石不允许任何形状变化"是硬约束（项目所有者实测的 BUG2）。
                        // 等值面是补丁采样的**纯函数** ⇒ 未被挖到的区域顶点逐位不变。
                        vx::CollapseCarveResult carved;
                        if (rigidCollapse.CarveBody(hit.body, hit.point, orbSpec.explosionRadiusBlocks, collapseSpec,
                                                    materials, physics, renderer, carved)) {
                            VX_LOG_INFO("光球命中整体 ⇒ **就地雕刻**（T50）：打掉 %zu 个、剩余 %zu 个体素%s"
                                        "（累计雕刻 %zu 次 / %zu 体素）",
                                        carved.removedVoxels, carved.leftVoxels,
                                        carved.deleted ? "；**剩余过少 ⇒ 整体删除**（不留下凸包残影）" : "",
                                        rigidCollapse.TotalCarved(), rigidCollapse.TotalCarvedVoxels());
                        }
                        const DetonationOutcome outcome = Detonate(editContext, hit.point, orbSpec);
                        terrainExplosionSeen = terrainExplosionSeen || outcome.terrainChanged;
                        // V0c：同一次爆炸也作用于**物件层**（ADR 0004 层③）—— 按类型分流：
                        // 可破坏者被摧毁消失，不可破坏者只被炸飞（见 `BlastObjects`）。
                        BlastObjects(objectSlots, objects, objectLayer, physics, renderer, hit.point,
                                     orbSpec.explosionRadiusBlocks);
                    }
                }

                // V0c：**支撑检查**（节拍推进）—— 脚下地表 / 体积被移除的物件必须**落下**（不得悬空）。
                // 放在本步**末尾**：这样本步的爆炸 / 挖除结果能在同一步被看见。
                if (++objectSupportStepCounter >= kObjectSupportCheckIntervalSteps) {
                    objectSupportStepCounter = 0;
                    CheckObjectSupports(objectSlots, orbQuery, physics);
                }
            }
            // 至少跑过一个逻辑步后，锁存的跳跃请求已被判定过（含"不满足着地条件而放弃"），消费掉。
            if (plan.steps > 0) {
                jumpRequested = false;
            }

            // V2b（测试设施）：`--switch-test` 到点请求**一次**世界切换（按会话时钟；可连续给出多个）。
            // 请求只是记账（V2a 的 `WorldManager`）；真正的卸载 / 重载在主循环退出后由外层循环尾部执行。
            if (switchTestNext < switchTests.size() && !worldManager.HasPendingSwitch() &&
                sessionClock.ElapsedSeconds() >= switchTests[switchTestNext].first) {
                const std::string& target = switchTests[switchTestNext].second;
                std::string        switchReason;
                if (worldManager.RequestSwitch(target, switchReason)) {
                    VX_LOG_INFO("世界切换（--switch-test）：已于会话 %.2f s 请求切到 [%s]（本帧末执行卸载 / 重载）",
                                sessionClock.ElapsedSeconds(), target.c_str());
                } else {
                    VX_LOG_WARN("世界切换（--switch-test）被拒绝：%s", switchReason.c_str());
                }
                ++switchTestNext;
            }

            // V3：传送门交互 —— **走近（≤ 提示半径）出提示；按 E 打开交互菜单**（V9 起**不再直接切换**）；
            //     **不自动切换**（正式玩家路径只有"门 + E ⇒ 菜单 ⇒ 进入"；`--switch-test` 保留为测试设施）。
            // 位置取本帧固定步之后的角色状态；`ConsumePressed` 只在门附近消费（pressed 边沿本就不跨帧残留）。
            // `nearbyPortalPromptName` 在帧末填入 `DebugStats` 交给 HUD（空串 = 不显示提示）。
            std::string nearbyPortalPromptName;
            if (!portals.empty()) {
                const vx::PortalEntry* portal =
                    vx::FindNearestPortal(portals, physics.GetCharacterState(character).position,
                                          vx::kPortalPromptRadius);
                if (portal != nullptr) {
                    // 提示显示名：有 CJK 字体 ⇒ 门名（配置未给 ⇒ 缺省「神秘传送门」）；否则 ⇒ **纯 ASCII** 的目标世界 id。
                    const bool cjkLabels = debugOverlay.UsesCjkLabels();
                    nearbyPortalPromptName = cjkLabels && !portal->name.empty()
                                                 ? portal->name
                                                 : (cjkLabels ? vx::UiText(vx::UiLabel::PortalDefaultName, true)
                                                              : portal->targetWorldId);
                    // V9：按 E **打开菜单**（数据由 game 层组装；UI 只画）—— 与 ESC 面板同一套捕获语义（打开即释放捕获）。
                    if (mouseCaptured && !suppression.keyboardGameplay && !debugOverlay.PortalMenuOpen() &&
                        input.ConsumePressed(vx::ActionId::Interact)) {
                        debugOverlay.OpenPortalMenu(vx::BuildPortalMenuModel(
                            *portal, worldManager.Find(portal->targetWorldId),
                            worldManager.FindInstance(portal->targetWorldId), cjkLabels));
                        captureBeforePanel = mouseCaptured;
                        (void)window.SetRelativeMouseMode(false);
                        mouseCaptured = false;
                        jumpRequested = false;
                        VX_LOG_INFO("传送门菜单（V9）：打开（门「%s」⇒ 目标世界 [%s]）；鼠标捕获：关（已释放，供点击控件）",
                                    nearbyPortalPromptName.c_str(), portal->targetWorldId.c_str());
                    }
                }
            }

            // V9：消费**传送门菜单**的一次选择 —— 请求由**上一帧末**的 ImGui 构建（`BuildUI`）产生，
            // `Take` 后清零 ⇒ **只生效一次**；`Esc` 关闭路径不产生动作（见上面 ToggleSystemPanel 分支）。
            {
                const vx::PortalMenuRequest portalRequest = debugOverlay.TakePortalMenuRequest();
                if (portalRequest.action != vx::PortalAction::None) {
                    // 菜单已关（UI 在按钮点击时即关闭）⇒ 恢复**打开前**的鼠标捕获状态（复用同一状态机）。
                    const vx::PanelCaptureTransition transition =
                        vx::DecidePanelCaptureTransition(/*opening=*/false, captureBeforePanel);
                    if (transition.captureRequested) {
                        mouseCaptured = window.SetRelativeMouseMode(true);
                    }
                    jumpRequested = false;

                    if (portalRequest.action == vx::PortalAction::Enter) {
                        std::string switchReason;
                        if (worldManager.RequestSwitch(portalRequest.targetWorldId, switchReason)) {
                            VX_LOG_INFO("世界切换（传送门菜单 · 进入）：请求切到 [%s]（本帧末执行卸载 / 重载；"
                                        "**复用已有秘境实例** ⇒ 同一会话内反复进入 ⇒ 同一世界）",
                                        portalRequest.targetWorldId.c_str());
                        } else {
                            VX_LOG_WARN("世界切换（传送门菜单 · 进入）被拒绝：%s", switchReason.c_str());
                        }
                    } else if (portalRequest.action == vx::PortalAction::Reset) {
                        // V9b：**销毁并重生** —— roll 新种子、`generation + 1`；**不自动进入**（与"不自动切换"一致）。
                        // 尚无实例 ⇒ 首次生成（`EnsureInstance`）；已有 ⇒ 销毁旧实例（`ResetInstance`）。
                        const std::uint64_t newSeed   = RollInstanceSeed();
                        std::string         resetReason;
                        const bool          resetOk =
                            (worldManager.FindInstance(portalRequest.targetWorldId) != nullptr)
                                ? worldManager.ResetInstance(portalRequest.targetWorldId, newSeed, resetReason)
                                : worldManager.EnsureInstance(portalRequest.targetWorldId, newSeed, resetReason);
                        if (resetOk) {
                            char message[256] = {};
                            std::snprintf(message, sizeof(message),
                                          vx::UiText(vx::UiLabel::PortalResetDoneFormat, debugOverlay.UsesCjkLabels()),
                                          portalRequest.targetWorldId.c_str(),
                                          static_cast<unsigned long long>(newSeed));
                            VX_LOG_INFO("传送门菜单（V9b · 重置）：%s；**不自动进入** ⇒ 再按 E 进入的是**新秘境**",
                                        message);
                            // V10：重置后**立刻持久化新种子**（否则重启会回到旧绑定 ⇒ "重置无效"）。
                            persistWorldInstances("重置秘境（V9b）");
                        } else {
                            VX_LOG_WARN("传送门菜单（V9b · 重置）被拒绝：%s", resetReason.c_str());
                        }
                    } else {
                        VX_LOG_INFO("传送门菜单（V9）：取消（保持「不自动切换」）");
                    }
                }
            }
            // T33：把本帧**落定**的倒塌整体体素化回写为地形（残骸可站、可继续挖），
            // 其重网格 / 碰撞体重建 / GPU 上传交给 T37 的延后队列按每帧预算推进。
            // 回写按最终姿态把体素落位到最近格 ⇒ 重叠 / 越界时可能少量丢弃（ADR 0015 后果 3 已登记）。
            for (const vx::ActiveCollapseUnit& settled : settledCollapseUnits) {
                const vx::CollapseWriteback writeback = rigidCollapse.Writeback(physics, digVolumes, renderer, settled);
                pendingDestruction.MergeVolumeBlocks(writeback.dirty);
                editContext.totalCollapseVoxels += writeback.writtenVoxels;
                const vx::CollapsePose restPose { settled.posePosition, settled.poseRotation };
                VX_LOG_INFO("倒塌落定（T33）：回写体素 %zu 个（丢弃 %zu 个、**接地沉降 %zu 个**、"
                            "**悬空已清除 %zu 个**）；倾角 %.0f°、"
                            "落点 (%.1f, %.1f, %.1f)；入队 %zu 个块重建；**体素材质**共 %.2f MB（T42，懒分配）",
                            writeback.writtenVoxels, writeback.droppedVoxels, writeback.settledVoxels,
                            writeback.removedFloatingVoxels,
                            static_cast<double>(restPose.TiltDegrees()), settled.posePosition.x,
                            settled.posePosition.y, settled.posePosition.z, writeback.dirty.size(),
                            static_cast<double>(digVolumes.MaterialBytes()) / (1024.0 * 1024.0));
                if (writeback.removedFloatingVoxels > 0) {
                    // T51（2026-09-28）：降不到支撑的残留体素**已被清除**（所有者指定"悬空的小土块可以直接删除"）
                    // ⇒ 场景里不会再出现悬空的泥土。仍打 WARN：这说明落点与真实支撑差了 32 格以上，值得留意。
                    VX_LOG_WARN("散体回写后仍有悬空体素 ⇒ **已清除** %zu 个（T51：落点下方 32 格内没有支撑）",
                                writeback.removedFloatingVoxels);
                }
            }
            settledCollapseUnits.clear();

            // T33：把活跃倒塌整体的位姿推给渲染器（每帧一次 64 B 推送 / 个，**不重烘焙顶点**）。
            rigidCollapse.SyncRender(renderer);

            // ---- T61（ADR 0020 决策二 / 四）：**常驻集合随玩家移动** ----
            // 每帧按预算建 / 卸少量块（一个动作 ≈ 1~5 ms，见 `kVolumeResidencyActionsPerFrame`）；
            // 建 / 卸引起的**重网格、GPU 上传、碰撞体增删**不在这里同步做，一律入既有的延后队列，
            // 由下面的 `destructionProcessor` 按 `kDestructionBudgetMs` 摊平（重活不得留在渲染帧里）。
            // 为什么跟着窗口走：静态全图在 1 km 下要 69~549 MB，而窗口内只需 ≈ 7~10 MB（ADR 0020）。
            {
                const vx::PhysicsWorld::CharacterState playerState = physics.GetCharacterState(character);
                // T81：`在飞` 也算"忙"（异步建块期间 `Step` 不会报告动作，但窗口调整并未真正结束）。
                const bool wasBusy =
                    volumeScheduler.HasPendingWork() || digVolumes.PendingBuildCount() > 0U;
                volumeResidencyChanged.clear();
                // T81 / ADR 0022：**先收包** —— 把 worker 算好的块安装进世界；安装之后的后续流程
                // （入表 / 上传 / 碰撞体 / tile 重网格）与"同步创建"**完全同源**，故复用同一段代码。
                if (digVolumes.PollBlockBuildsAndInstall(volumeResidencyChanged, kVolumeBuildsInstalledPerFrame) > 0U) {
                    VX_LOG_DEBUG("可挖体积块构建收包（T81）：本帧安装 %zu 块（在飞 %zu 块、worker 已完成 %zu 块、"
                                 "单块 worker 计算峰值 %.2f ms）",
                                 volumeResidencyChanged.size(), digVolumes.PendingBuildCount(),
                                 volumeBuildPipeline.SnapshotStats().completed,
                                 volumeBuildPipeline.SnapshotStats().computeMsMax);
                }
                if (volumeScheduler.Update(digVolumes, playerState.position.x, playerState.position.z)) {
                    (void)volumeScheduler.Step(digVolumes, kVolumeResidencyActionsPerFrame,
                                               volumeResidencyChanged);
                }
                if (!volumeResidencyChanged.empty()) {
                    volumeCreated.clear();
                    volumeTouchedTiles.clear();
                    for (const vx::BlockCoord& coord : volumeResidencyChanged) {
                        const auto slot = volumeSlots.Find(coord);  // T79⑤：O(1)
                        if (digVolumes.Blocks().find(coord) != digVolumes.Blocks().end()) {
                            if (slot == volumeSlots.end()) {
                                // 新建：**先入表**（层间交接过滤器读的就是这张表，ADR 0011 / 0020 决策三），
                                // 再立刻上传它的网格 —— 本帧稍后处理 tile 重网格时，体积面**已经在画**，
                                // 因此不会出现"地表已被跳过、体积还没画"的破洞。
                                volumeSlots.Insert(coord);
                                UploadVolumeMeshAt(volumeSlots, renderer, digVolumes, coord);
                                volumeCreated.push_back(coord);
                            }
                        } else if (slot != volumeSlots.end()) {
                            // 走远卸载（或超上限淘汰）：释放 GPU 网格 + 撤销三角网碰撞体。
                            if (slot->second.handle.IsValid()) {
                                renderer.ReleaseMesh(slot->second.handle);
                            }
                            (void)volumeSlots.Erase(coord);
                            volumeCollision.RemoveBlock(coord);
                        } else {
                            continue;  // 世界与表里都没有 ⇒ 无变化（防御性）
                        }
                        volumeTouchedTiles.push_back(vx::TileCoord { vx::TileOfBlockIndex(coord.x),
                                                                     vx::TileOfBlockIndex(coord.z) });
                    }
                    if (!volumeCreated.empty()) {
                        // 只补"碰撞体"这一半：网格已在 `CreateBlock` 内生成、上面已上传（T61：不重复重网格）。
                        pendingDestruction.MergeVolumeBlockCollisions(volumeCreated);
                    }
                    if (!volumeTouchedTiles.empty()) {
                        // 接管状态翻转的 tile 必须重网格：常驻集合一变，`ResidentQuadFilter` 的判据就变了
                        // （否则窗口边缘会出现空洞或重影面）。延后阶段的 `TileCollision` 分支据此增删高度场。
                        pendingDestruction.MergeTiles(volumeTouchedTiles);
                    }
                }
                if (wasBusy && !volumeScheduler.HasPendingWork() && digVolumes.PendingBuildCount() == 0U) {
                    // 一次"窗口调整"**收尾后**记一条（每次跨越 tile 边界一条，不逐帧刷屏）——走动验收的可观测证据。
                    // 必须放在上面的同步之后：否则本帧那一个动作还没落到 `volumeSlots` 上，打印出来的计数会差一个。
                    // T81：同时给出块构建的观测数据（worker 完成数 / 单块计算峰值）——"尖峰去哪了"的可复现证据。
                    const vx::VolumeBuildPipeline::Stats buildStats = volumeBuildPipeline.SnapshotStats();
                    VX_LOG_INFO("可挖体积常驻集合已随窗口调整完毕（ADR 0020 窗口 + T81 worker 构建）：玩家 tile (%d, %d) ⇒ 常驻 %zu 块"
                                "（世界内 %zu 块、窗口目标 %zu 块、**留驻脏块 %zu 块**；"
                                "**worker 已构建 %zu 块、单块计算峰值 %.2f ms**、worker %u 个）",
                                volumeScheduler.Window().centerTileX, volumeScheduler.Window().centerTileZ,
                                volumeSlots.Size(), digVolumes.Blocks().size(), volumeScheduler.DesiredCount(),
                                volumeScheduler.KeptDirtyCount(), buildStats.completed, buildStats.computeMsMax,
                                volumeBuildPipeline.WorkerThreadCount());
                }

                // ---- W7-S3b：地表 tile 常驻集合随窗口调整（ADR 0024 决策一）----
                // 生成 + 网格化**下沉 worker**（ADR 0022 形态）：主线程只做"收包 + 过滤 + 安装 + GPU 上传"，
                // 每帧成本**有上界**（见 `kTerrain*PerFrame`）；`Update` 只重算计划（幂等）。
                terrainResidencyChanged.clear();
                terrainRelodChanged.clear();
                const bool tileWasBusy = tileScheduler.HasPendingWork();
                (void)tileScheduler.Update(world, playerState.position.x, playerState.position.z);
                // 窗口中心变化 ⇒ 预取缓存（按旧中心的集合 / LOD 口径）作废，重新预取。
                if (tileScheduler.Window().centerTileX != terrainPrefetchCenterX ||
                    tileScheduler.Window().centerTileZ != terrainPrefetchCenterZ) {
                    terrainPrefetchCenterX = tileScheduler.Window().centerTileX;
                    terrainPrefetchCenterZ = tileScheduler.Window().centerTileZ;
                    world.ClearStagedTiles();
                    terrainBuildInFlight.clear();
                }
                // 先收包（安装已算好的结果），再**提前**预取，最后**门控**推进 `Step` 做廉价安装。
                drainTerrainTileBuilds();
                prefetchTerrainTiles();
                // 门控：本帧 `Step` 会加载的**整批** tile 都已就绪才推进（等 worker；**绝不**在渲染帧内同步生成）。
                const bool canStep = terrainStepReady(kTerrainResidencyActionsPerFrame);
                if (canStep && tileScheduler.HasPendingWork()) {
                    (void)tileScheduler.Step(world, kTerrainResidencyActionsPerFrame, terrainResidencyChanged);
                }
                for (const vx::TileCoord& coord : terrainResidencyChanged) {
                    // `Step` 已经改过世界：**世界里有 ⇒ 这是新建**（登记 + 上传 + 碰撞）；否则是卸载。
                    if (world.HasTile(coord.x, coord.z)) {
                        const std::size_t index = AppendResidentTile(tileResidency, coord);
                        UploadResidentTile(tileResidency, index);
                        // 新 tile 只可能出现在窗口边缘（Chebyshev 33）⇒ 通常不在碰撞半径内；这里按判据如实处理。
                        (void)SyncResidentTileCollision(tileResidency, index,
                                                        NeedsTerrainCollision(tileScheduler.Window(), coord));
                    } else {
                        (void)RemoveResidentTile(tileResidency, coord);
                    }
                }

                // **LOD 切换（relod）**：只改**网格**，不改世界数据。
                // 有 worker ⇒ 提交**重网格任务**（高度快照进 worker，网格化离开渲染帧）；结果在 `drain` 里安装、
                // 随后按预算重传。无 worker ⇒ 回退主线程重建（有界由 `kTerrainRelodPerFrame` 保证）。
                (void)tileScheduler.StepRelod(kTerrainRelodPerFrame, terrainRelodChanged);
                for (const vx::TileCoord& coord : terrainRelodChanged) {
                    std::size_t index = 0;
                    if (!world.HasTile(coord.x, coord.z) ||
                        !FindResidentTileIndex(tileResidency, coord, index)) {
                        continue;  // 同一帧里已被卸掉（relod 清单可能含"随后卸载"的 tile）⇒ 跳过
                    }
                    const int lod = tileScheduler.LodLevelForTile(coord);
                    if (terrainHasWorkers) {
                        if (terrainRelodInFlight.find(coord) != terrainRelodInFlight.end()) {
                            continue;  // 已在飞 ⇒ 不重复提交
                        }
                        const vx::TerrainTile* tile = world.FindTile(coord.x, coord.z);
                        if (tile == nullptr) {
                            continue;
                        }
                        vx::TerrainTileBuildRequest request;
                        request.coord      = coord;
                        request.lodLevel   = lod;
                        request.remeshOnly = true;
                        request.tile       = *tile;  // 高度快照（8 KB）⇒ worker 只读、不碰世界
                        terrainBuildPipeline.Submit(std::move(request));
                        terrainRelodInFlight.insert(coord);
                        continue;
                    }
                    world.MeshTile(coord.x, coord.z, lod);
                    if (tileResidency.handles[index].IsValid()) {
                        tileResidency.renderer.ReleaseMesh(tileResidency.handles[index]);
                        tileResidency.handles[index] = vx::MeshHandle {};
                    }
                    UploadResidentTile(tileResidency, index);
                    (void)SyncResidentTileCollision(tileResidency, index,
                                                    NeedsTerrainCollision(tileScheduler.Window(), coord));
                }
                // 重传本帧（含上帧残留）已安装的 relod 网格（有上界）。
                {
                    std::size_t relodUploads = 0;
                    while (relodUploads < kTerrainRelodUploadsPerFrame && !terrainRelodUploads.empty()) {
                        const vx::TileCoord coord = terrainRelodUploads.front();
                        terrainRelodUploads.erase(terrainRelodUploads.begin());
                        ++relodUploads;
                        std::size_t index = 0;
                        if (!world.HasTile(coord.x, coord.z) ||
                            !FindResidentTileIndex(tileResidency, coord, index)) {
                            continue;  // 已被卸掉 ⇒ 丢弃
                        }
                        if (tileResidency.handles[index].IsValid()) {
                            tileResidency.renderer.ReleaseMesh(tileResidency.handles[index]);
                            tileResidency.handles[index] = vx::MeshHandle {};
                        }
                        UploadResidentTile(tileResidency, index);
                        (void)SyncResidentTileCollision(tileResidency, index,
                                                        NeedsTerrainCollision(tileScheduler.Window(), coord));
                    }
                }

                // **碰撞体随窗口重扫**（W7-S3b）：只在窗口中心变化时**开启一轮**，每帧只做固定个**状态跃迁**。
                // 为什么必须收敛半径：碰撞只在近场有意义（角色 / 弹道 / 爆炸都在玩家附近），而常驻集合要到 33 tile；
                // 若给每个常驻 tile 都建高度场，Jolt 的静态体会达数千个（宽相位 / 内存 / 建体耗时都不可接受）。
                if (terrainCollisionCenterX != tileScheduler.Window().centerTileX ||
                    terrainCollisionCenterZ != tileScheduler.Window().centerTileZ) {
                    terrainCollisionCenterX     = tileScheduler.Window().centerTileX;
                    terrainCollisionCenterZ     = tileScheduler.Window().centerTileZ;
                    terrainCollisionSweepActive = true;
                    terrainCollisionSweepCursor = 0;
                }
                if (terrainCollisionSweepActive) {
                    const int   half = kTerrainCollisionRadiusTiles + 1;  // 多扫一圈：让"刚离开的"也走到撤销
                    const int   side = 2 * half + 1;
                    std::size_t used = 0;
                    while (terrainCollisionSweepCursor < side * side &&
                           used < kTerrainCollisionActionsPerFrame) {
                        const int dx = (terrainCollisionSweepCursor % side) - half;
                        const int dz = (terrainCollisionSweepCursor / side) - half;
                        ++terrainCollisionSweepCursor;
                        const vx::TileCoord coord { terrainCollisionCenterX + dx, terrainCollisionCenterZ + dz };
                        std::size_t         index = 0;
                        if (!FindResidentTileIndex(tileResidency, coord, index)) {
                            continue;  // 不在常驻集合里（世界边界外 / 尚未加载）⇒ 无碰撞体可谈
                        }
                        const bool want = NeedsTerrainCollision(tileScheduler.Window(), coord);
                        if (want == (tileResidency.collisionActive[index] != 0U)) {
                            continue;  // 状态一致 ⇒ 不付重活（游标继续扫，不占预算）
                        }
                        (void)SyncResidentTileCollision(tileResidency, index, want);
                        ++used;
                    }
                    if (terrainCollisionSweepCursor >= side * side) {
                        terrainCollisionSweepActive = false;
                    }
                }
                // "窗口调整完毕" = 调度器无待办 **且** worker 在飞 / 待重传都已排空（**真正追上飞行**的可观测判据）。
                const bool terrainCaughtUp = !tileScheduler.HasPendingWork() && terrainBuildInFlight.empty() &&
                                             terrainRelodInFlight.empty() && terrainRelodUploads.empty();
                if (tileWasBusy && terrainCaughtUp) {
                    // 一次"窗口调整"收尾后记一条（跨越 tile 边界一条，不逐帧刷屏）——走动验收的可观测证据。
                    const vx::TerrainTileBuildPipeline::Stats buildStats = terrainBuildPipeline.SnapshotStats();
                    VX_LOG_INFO("地表 tile 常驻集合已随窗口调整完毕（W7-S3b / ADR 0024）：玩家 tile (%d, %d) ⇒ "
                                "常驻 **%zu** 个（世界内 %zu 个、窗口目标 %zu 个；LOD 分环 %d/%d/%d tile、"
                                "碰撞半径 %d tile）；**worker 已构建 %zu 个 tile、单 tile 计算峰值 %.2f ms**、"
                                "**同步回退 %zu 次**",
                                tileScheduler.Window().centerTileX, tileScheduler.Window().centerTileZ,
                                tileCoords.size(), worldTilesX * worldTilesZ, tileScheduler.DesiredCount(),
                                kGameTerrainLodRings.radii[0], kGameTerrainLodRings.radii[1],
                                kGameTerrainLodRings.radii[2], kTerrainCollisionRadiusTiles, buildStats.completed,
                                buildStats.computeMsMax, world.SyncFallbackCount());
                }
            }

            const std::size_t destructionUnits = destructionProcessor.Process(editContext, kDestructionBudgetMs);
            if (terrainExplosionSeen) {
                // 地表爆破的外环会抬高地形 ⇒ 复用缺陷 B2 的救场：把被埋住的角色顶回地面。
                LiftCharacterIfBuried(physics, character, camera, world);
            }
            const double logicMs = logicTimer.EndMs();

            // 浮点原点重定基（T41）：渲染原点漂移过远时把它搬到相机附近。
            // T41 起这**只是一次常量更新** —— 顶点承载的是**网格局部**坐标，"这块在哪"由
            // `renderer.SetRenderOrigin` 每帧下发的偏移（网格原点 − 渲染原点）在绘制时补上，
            // 因此重定基**不再重传任何网格**（旧做法在这里逐个 `ReleaseMesh` + `UploadMesh` 137 个网格，
            // 每次阻塞到 GPU 完成，debug 下当帧停顿 ≈70~140 ms —— 那正是"走动时周期性卡顿"的根因）。
            // T79②：本段独立计时（原先落在"未计时"里，是那颗 1339 ms 首帧尖峰的候选之一）。
            rebaseTimer.Begin();
            const glm::vec3 focus = camera.TargetCurrent();
            const glm::dvec3 focusDouble(static_cast<double>(focus.x), static_cast<double>(focus.y),
                                         static_cast<double>(focus.z));
            if (glm::distance(renderOrigin, focusDouble) > kRebaseDistance) {
                renderOrigin = glm::dvec3(std::floor(focusDouble.x), std::floor(focusDouble.y),
                                          std::floor(focusDouble.z));
                VX_LOG_INFO("渲染原点重定基到 (%.0f, %.0f, %.0f)（T41：只更新 uniform，零重传）", renderOrigin.x,
                            renderOrigin.y, renderOrigin.z);
            }
            rebaseMs = rebaseTimer.EndMs();

            // T13：每帧把主角胶囊改写为**渲染相对**顶点并就地刷新。位置取相机目标的插值位置，
            // 与渲染插值一致（alpha 只用于渲染，绝不回写模拟状态，红线 11）；装饰用，不影响碰撞。
            // T41：顶点按当前渲染原点烘焙 ⇒ 原点也传当前渲染原点（偏移恒 0），重定基后二者一起跟着变。
            // T45：**动态顶点上传**独立计时（原先落在未计时区）。
            dynamicUploadTimer.Begin();
            if (characterMesh.IsValid()) {
                const glm::vec3 feetRender =
                    glm::mix(camera.TargetPrevious(), camera.TargetCurrent(), static_cast<float>(plan.alpha));
                if (characterUsesModel) {
                    // T69：蒙皮网格每帧只做两件事 —— ①更新网格变换（原点 = 世界脚底 − 脚底中心偏移；
                    // 旋转 = **T86 的朝向 yaw**，绕 +Y、作用于脚底中心）；
                    // ②上传骨骼矩阵（**每个蒙皮网格一次整块**上传）。**不重传任何顶点**（对照下面的胶囊路径）。
                    // `feetRender` 取的是 `camera.TargetCurrent()`（**世界坐标**）⇒ `SetMeshTransform` 的
                    // 世界原点直接用它，`renderer` 内部再减渲染原点（见 mesh_renderer.cpp 的 modelToRender）。
                    const glm::dvec3 feetWorld(static_cast<double>(feetRender.x), static_cast<double>(feetRender.y),
                                               static_cast<double>(feetRender.z));
                    const glm::dvec3 meshOrigin = feetWorld - glm::dvec3(characterLocalPivot);
                    // T86：朝向四元数 = 绕 +Y 转 `characterYaw`（+ 模型前向修正）。旋转作用于网格原点（脚底中心），
                    // 故角色绕自身脚底中心转向、不产生额外位移。
                    const glm::quat characterRotation =
                        glm::angleAxis(characterYaw + vx::kCharacterModelForwardOffsetRad, glm::vec3(0.0F, 1.0F, 0.0F));
                    renderer.SetMeshTransform(characterMesh, meshOrigin, characterRotation);

                    const vx::AnimationClip* clip = characterClips[static_cast<int>(characterAnimState)];
                    if (clip != nullptr) {
                        const std::vector<glm::mat4> skinning =
                            vx::ComputeSkinningMatrices(characterModel, *clip, characterAnimTime);
                        if (skinning.size() == characterModel.joints.size() &&
                            skinning.size() <= static_cast<std::size_t>(vx::kMaxSkinJoints)) {
                            characterBoneMatrices.resize(skinning.size() * 16U);
                            for (std::size_t joint = 0; joint < skinning.size(); ++joint) {
                                for (int element = 0; element < 16; ++element) {
                                    // glm::mat4 是列主序（element = col * 4 + row）⇒ 与 std430 的 mat4[] 一致。
                                    characterBoneMatrices[joint * 16U + static_cast<std::size_t>(element)] =
                                        skinning[joint][element / 4][element % 4];
                                }
                            }
                            renderer.SetSkinningMatrices(characterMesh, characterBoneMatrices.data(),
                                                         static_cast<std::uint32_t>(skinning.size()));
                        }
                    }
                } else {
                    const glm::dvec3 feetRenderDouble(static_cast<double>(feetRender.x),
                                                      static_cast<double>(feetRender.y),
                                                      static_cast<double>(feetRender.z));
                    UpdateCharacterRenderVertices(characterVertices, capsuleLocalMesh, feetRenderDouble, renderOrigin);
                    // 唯一可能的失败是句柄失效或顶点数变化，这里两者都不会发生（已在上面校验句柄）。
                    (void)renderer.UpdateMeshVertices(characterMesh, characterVertices, renderOrigin);
                }
            }

            // T27：活动光球每帧就地刷新顶点（球心 = 弹道位置，取渲染插值；与主角同一套约定）。
            // 位置用 `double` 累加后再落回 `float`（红线 6）；未激活的槽位不进绘制列表，无需刷新。
            for (std::size_t i = 0; i < orbPool.Orbs().size(); ++i) {
                const vx::Orb& orb = orbPool.Orbs()[i];
                if (!orb.active || i >= orbHandles.size() || !orbHandles[i].IsValid()) {
                    continue;
                }
                UpdateOrbRenderVertices(orbVertices[i], orbLocalMesh, orb.previousPosition, orb.position, plan.alpha,
                                        renderOrigin);
                (void)renderer.UpdateMeshVertices(orbHandles[i], orbVertices[i], renderOrigin);
            }
            const double dynamicUploadMs = dynamicUploadTimer.EndMs();

            // W6h：推进**避障平滑**（每渲染帧一次）—— 阻尼 + 迟滞，消除"临界点（遮挡刚出现 / 消失）反复横跳
            // ⇒ 跟随距离瞬间跳变 ⇒ 画面闪烁"。`Evaluate` 随后只**读**该结果（保持 const / 幂等，红线 11）。
            camera.UpdateAvoidance(static_cast<float>(frameDeltaSeconds), plan.alpha, &cameraQuery);
            // 渲染：alpha 只用于在上一 / 当前逻辑状态之间插值，绝不回写模拟状态（红线 11）。
            const vx::CameraView view = camera.Evaluate(plan.alpha, &cameraQuery);
            // 渲染原点相对视图：顶点上传时已减去渲染原点，故**相机与剔除必须用同一坐标系**（红线 6）。
            // 提前到这里是因为下面的绘制列表要用它的 `viewProjection` 做视锥剔除（T39）。
            vx::CameraView relativeView = RelativeCameraView(view, renderOrigin);
            // W7-S3b：**LOD 原点** = 玩家所在 tile 的**中心**（tile 对齐）在**渲染相对**坐标下的位置。
            // 顶点着色器按"顶点到它的 Chebyshev 距离"推进 CDLOD morph —— 与 CPU 侧
            // `TerrainTileWindow::TileDistanceFromCenter` 同源 ⇒ morph 因子恰在环边界取 1，
            // 相邻环在边界处几何**逐位相同**（无接缝）。
            relativeView.lodOrigin = glm::vec3(
                static_cast<float>(static_cast<double>(tileScheduler.Window().centerTileX * vx::kTerrainTileSize +
                                                       vx::kTerrainTileSize / 2) -
                                   renderOrigin.x),
                0.0F,
                static_cast<float>(static_cast<double>(tileScheduler.Window().centerTileZ * vx::kTerrainTileSize +
                                                       vx::kTerrainTileSize / 2) -
                                   renderOrigin.z));
            // V0c：把**动态物件**（掉落中 / 被炸飞）的位姿推给渲染器与剔除包围盒。
            // 与倒塌整体同口径：只推 64 B 的逐网格变换（`SetMeshTransform`），**不重烘焙顶点**。
            // 动态物件用**包围球**做剔除包围盒（任意姿态下都保守，不会漏画）。
            for (ObjectSlot& slot : objectSlots) {
                if (slot.removed || !slot.dynamic || slot.body == 0 || slot.type == nullptr) {
                    continue;
                }
                const vx::PhysicsWorld::RigidBodyState state = physics.GetRigidBodyState(slot.body);
                slot.position = state.position;
                if (slot.handle.IsValid()) {
                    renderer.SetMeshTransform(slot.handle, state.position, state.rotation);
                }
                const float radius = std::sqrt(slot.type->halfExtentX * slot.type->halfExtentX +
                                               slot.type->halfExtentY * slot.type->halfExtentY +
                                               slot.type->halfExtentZ * slot.type->halfExtentZ);
                const glm::vec3 center(static_cast<float>(state.position.x), static_cast<float>(state.position.y),
                                       static_cast<float>(state.position.z));
                slot.bounds.min   = center - glm::vec3(radius, radius, radius);
                slot.bounds.max   = center + glm::vec3(radius, radius, radius);
                slot.bounds.valid = true;
            }

            // T79②：**剔除与绘制列表构建**独立计时（原先落在"未计时"里）。
            cullTimer.Begin();

            // T39：**先剔除再提交**（`references/performance-and-hitches.md` §1.3 硬规则 3）。
            // 判据 = 相机视锥 ∩（物体 ∪ 其影子落点）：前者去掉"背后 / 侧向"的网格，后者保证不丢阴影。
            const vx::Frustum frustum = vx::FrustumFromViewProjection(relativeView.viewProjection);

            // T27：本帧绘制列表 = 地表 tile + 可挖体积块 + 主角 + **活动**光球（失效槽位不进列表，
            // 因此不会为它们付出 draw call 与统计）。容量在启动时已预留，稳态零分配。
            frameHandles.clear();
            std::size_t visibleTiles   = 0;
            std::size_t visibleVolumes = 0;
            std::size_t visibleShells  = 0;
            std::size_t visibleObjects = 0;
            for (std::size_t i = 0; i < tileHandles.size(); ++i) {
                if (tileHandles[i].IsValid() && VisibleToCamera(frustum, tileBounds[i], renderOrigin, sunDirection)) {
                    frameHandles.push_back(tileHandles[i]);
                    ++visibleTiles;
                }
            }
            for (const auto& entry : volumeSlots) {
                if (entry.second.handle.IsValid() &&
                    VisibleToCamera(frustum, entry.second.bounds, renderOrigin, sunDirection)) {
                    frameHandles.push_back(entry.second.handle);
                    ++visibleVolumes;
                }
            }
            // W4：地表壳的近场块（静网格，与 tile / 体积同走 T39 视锥剔除）。
            for (std::size_t i = 0; i < shellHandles.size(); ++i) {
                if (shellHandles[i].IsValid() &&
                    VisibleToCamera(frustum, shellBounds[i], renderOrigin, sunDirection)) {
                    frameHandles.push_back(shellHandles[i]);
                    ++visibleShells;
                }
            }
            // V0b：物件层（ADR 0004 层③）—— **静网格**，与 tile / 体积 / 地表壳同走 T39 视锥剔除。
            // 提交量由剔除结果决定（SKILL 第四节硬规则 3）；物件不移动 ⇒ 位姿在上传时一次登记。
            for (const ObjectSlot& slot : objectSlots) {
                if (slot.handle.IsValid() && VisibleToCamera(frustum, slot.bounds, renderOrigin, sunDirection)) {
                    frameHandles.push_back(slot.handle);
                    ++visibleObjects;
                }
            }
            // W6：水面（半透明；在主通道**最后**绘制 ⇒ 追加在列表末尾）。
            if (waterHandle.IsValid()) {
                frameHandles.push_back(waterHandle);
            }
            // W6e：主角**淡出** —— 相机贴太近时不再被近裁剪面切开（⇒ 不再看到"人物内部"；业界对"贴脸穿模"的标准解）。
            // 不透明度由纯函数给出（开阔处 = 1 = 不透明）；实际淡出在片元侧按 **Bayer 抖动 discard** 实现
            // （保持不透明管线 ⇒ 无深度排序问题）。
            // W6d 的"过近隐藏"能力仍保留、仍关闭（`targetHideDistance = 0` ⇒ 判据恒 false ⇒ 主角恒提交）。
            if (characterMesh.IsValid() && !vx::ShouldHideFollowTarget(view, camera.Settings())) {
                renderer.SetMeshOpacity(characterMesh, vx::FollowTargetFadeOpacity(view, camera.Settings()));
                frameHandles.push_back(characterMesh);
            }
            for (std::size_t i = 0; i < orbHandles.size(); ++i) {
                if (orbPool.Orbs()[i].active && orbHandles[i].IsValid()) {
                    frameHandles.push_back(orbHandles[i]);
                }
            }
            // T33：活跃倒塌整体的网格（落定后即移出列表 ⇒ 不再为它付 draw call）。
            // 它们的位置每帧在变，故**不参与** T39 的静态视锥剔除（数量 ≤ `max_active_units`，代价可忽略）。
            for (const vx::ActiveCollapseUnit& unit : rigidCollapse.Active()) {
                if (unit.mesh.IsValid()) {
                    frameHandles.push_back(unit.mesh);
                }
            }
            // 提交量（T38）：在 `RenderFrame` 之后记录，**下一帧**的尖峰日志才能与同批实测值（draw call /
            // 三相耗时 / 帧时长）对齐 —— 三者都取"最近一次"的实测值，混帧会让定位结论失真。
            const std::size_t submittedThisFrame = frameHandles.size();
            cullMs = cullTimer.EndMs();  // T79②：剔除相位到此结束（下面的一次性日志不计入）

            // T39：剔除结果**首次可观测**（`references/performance-and-hitches.md` §3"提交量必须由剔除结果
            // 决定"）：只打一条，用于确认剔除真的在起作用（而不是把整个世界都提交了）。
            if (!cullingLogged) {
                cullingLogged = true;
                VX_LOG_INFO("首帧视锥剔除（T39）：地表 tile %zu/%zu、可挖体积块 %zu/%zu、地表壳块 %zu/%zu、"
                            "物件 %zu/%zu 通过（含阴影扫掠余量）；本帧提交网格 %zu 个",
                            visibleTiles, tileHandles.size(), visibleVolumes, volumeSlots.Size(), visibleShells,
                            shellHandles.size(), visibleObjects, objectSlots.size(), submittedThisFrame);
            }

            // 调试面板：统计经独立接口采集，只在渲染线程构建，不进世界层热路径。
            vx::DebugStats stats;
            const vx::PhysicsWorld::CharacterState characterState = physics.GetCharacterState(character);
            stats.frameSeconds      = frameDeltaSeconds;  // T79②：帧首采样（= 上一帧的完整周期）
            stats.stepsThisFrame     = plan.steps;
            stats.frameRateCap       = panelContext.settings.frameRateCap;  // T17：F1 面板显示当前目标
            stats.characterPosition = characterState.position;
            stats.characterOnGround = characterState.onGround;
            stats.characterFlying   = flying;
            stats.cameraYaw         = camera.Yaw();
            stats.cameraPitch       = camera.Pitch();
            stats.cameraDistance    = view.distance;
            stats.explosionRadius   = orbSpec.explosionRadiusBlocks;
            stats.orbActiveCount    = orbPool.ActiveCount();
            stats.orbCapacity       = orbPool.Capacity();
            stats.volumeBlockCount  = volumeSlots.Size();
            stats.carvedBlockCount  = digVolumes.CarvedBlockCount();
            // T61：常驻调度的可观测量 —— "待办"回落说明窗口已跟上玩家，"留驻"增长说明玩家改造的洞被保住。
            stats.volumePendingActions = volumeScheduler.PendingActionCount();
            stats.volumeKeptDirtyCount = volumeScheduler.KeptDirtyCount();
            stats.mouseCaptured     = mouseCaptured;
            stats.loadedTileCount   = tileCoords.size();
            stats.lastDirtyTileCount = destructionUnits;
            stats.tileBodyCount     = terrainCollision.TileBodyCount();
            stats.nearbyPortalPromptName = nearbyPortalPromptName;  // V3/V9：走近传送门的提示（空串 = 不显示）
            stats.physicsReady      = true;

            // T24：渲染开销取自引擎的通用统计；绘制数为**最近一次** RenderFrame（面板早于本帧渲染）。
            const vx::RenderStats& renderStats = renderer.Stats();
            stats.drawCalls     = renderStats.drawCalls;
            stats.triangleCount = renderStats.triangleCount;
            stats.vertexCount   = renderStats.vertexCount;
            stats.textureBytes  = renderStats.textureBytes;
            // T28 / T29：体积碰撞体数与累计塌落体素数（纯展示，用于验证"洞能走进去、支撑缺失会塌"）。
            stats.volumeBodyCount    = volumeCollision.BlockBodyCount();
            stats.collapseMovedVoxels = editContext.totalCollapseVoxels;
            // T24：CPU 分解用**上一帧**的实测值（本帧渲染尚未提交，与 frameSeconds 同源）。
            stats.cpuLogicMs  = cpuCost.logicMs;
            stats.cpuUiMs     = cpuCost.uiMs;
            stats.cpuRenderMs = cpuCost.renderMs;
            stats.swapchainWaitMs = renderStats.swapchainWaitMs;  // T38：与 draw call 同源的"在等"量

            // T24：面板构建（含 ImGui::Render）计入 UI 构建耗时。
            uiTimer.Begin();
            debugOverlay.BuildUI(stats, panelContext);
            debugOverlay.EndFrame();
            uiMs += uiTimer.EndMs();

            // T15：把系统面板的改动落到平台层（"控件标签即行为契约"：全屏真的切、分辨率真的改、音量真的接增益路径）。
            bool windowGeometryChanged = false;
            if (panelContext.displayModeChanged) {
                if (panelContext.settings.displayMode == vx::DisplayMode::Fullscreen) {
                    (void)window.SetFullscreen(true);  // 桌面无边框全屏
                    VX_LOG_INFO("系统面板：显示模式 → 全屏（桌面无边框）");
                } else {
                    (void)window.SetFullscreen(false);
                    (void)window.SetWindowSize(panelContext.settings.windowWidth, panelContext.settings.windowHeight);
                    VX_LOG_INFO("系统面板：显示模式 → 窗口（%d x %d，已恢复客户区尺寸）",
                                panelContext.settings.windowWidth, panelContext.settings.windowHeight);
                }
                windowGeometryChanged = true;
            }
            if (panelContext.resolutionChanged && vx::IsResolutionEditable(panelContext.settings.displayMode)) {
                (void)window.SetWindowSize(panelContext.settings.windowWidth, panelContext.settings.windowHeight);
                VX_LOG_INFO("系统面板：分辨率 → %d x %d", panelContext.settings.windowWidth,
                            panelContext.settings.windowHeight);
                windowGeometryChanged = true;
            }
            if (panelContext.volumeCommitted) {
                // 唯一的音频增益入口（当前无音源，只记录设置与折算增益）。
                vx::ApplyMasterVolumeGain(panelContext.settings.masterVolume);
            }
            if (panelContext.frameRateCapChanged) {
                // T17：滑块改动**立即生效**——目标 == 刷新率 → 垂直同步；低于 → 睡眠限帧。
                ApplyFrameRateCap(window, frameLimiter, panelContext.settings.frameRateCap, displayRefreshRate);
            }
            if (windowGeometryChanged) {
                // 分辨率 / 全屏变化会改变视口宽高比，重算投影（否则画面被拉伸）。
                const vx::DisplaySize size = window.WindowSize();
                if (size.height > 0) {
                    camera.SetAspectRatio(static_cast<float>(size.width) / static_cast<float>(size.height));
                }
            }
            if (panelContext.requestQuit) {
                VX_LOG_INFO("系统面板：点击退出游戏 → 离开主循环");
                quitRequested = true;  // V2b：本轮结束后退出外层"世界装载循环"
                break;
            }

            // T45：**uniform 构建**独立计时（材质 / 光照 / 相机 / 渲染原点 / 阴影级联，原先落在未计时区）。
            uniformTimer.Begin();
            // 材质参数（高度带 / 坡度带 / UV 尺度 / 层色）来自与 TerrainWorld **同一份**材质表；
            // 渲染原点每次重定基后都要刷新（原点进 uniform，片元据此把渲染相对坐标还原为世界坐标）。
            const vx::MaterialUniform materialUniform =
                vx::BuildMaterialUniform(world.Materials(), renderOrigin.x, renderOrigin.y, renderOrigin.z,
                                         realTextureMode);
            renderer.SetMaterialUniform(&materialUniform, sizeof(materialUniform));

            // 光照与雾参数（T21a / T21c）：来自启动期加载的同一份光照表，经 BuildLightingUniform 单入口投影。
            // **相机世界位置每帧变化**（第三人对焦跟随 + 避障），而雾按视距插值，故 uniform 必须每帧重建。
            // `view.eye` 是绝对世界坐标，与片元还原出的 worldPosition 同空间。
            // T67：`environmentIblReady` 决定环境项走 IBL 还是半球天空光回落；两者**共用同一个 uniform 结构**，
            // 只是 `fogParams.zw` 不同（着色器按位分支），因此切换不需要换管线、也不需要重编 Shader。
            const vx::LightingUniform lightingUniform = vx::BuildLightingUniform(
                lighting, static_cast<double>(view.eye.x), static_cast<double>(view.eye.y),
                static_cast<double>(view.eye.z),
                environmentIblReady ? vx::kEnvironmentPrefilterMipCount : 0U);
            renderer.SetLightingUniform(&lightingUniform, sizeof(lightingUniform));

            // 相机常量（T39 起 `relativeView` 在**构建绘制列表之前**就已算好，见那里的视锥剔除）。
            renderer.SetCamera(relativeView);
            // T41：每帧下发渲染原点 —— 顶点只承载网格局部坐标，逐网格变换 = 平移(网格原点 − 渲染原点) × 旋转。
            // 这是"渲染原点重定基"的全部代价（一次常量写入，零重传）。`renderOrigin` 是整数（取整），
            // 传入 `double` 与上传时登记的网格原点相减不会引入误差。
            renderer.SetRenderOrigin(renderOrigin);
            // W6：推进水面流动时间并下发（`water.frag` 的滚动波据此流动）。
            waterTimeSeconds += static_cast<double>(plan.steps) * vx::kFixedDt;
            renderer.SetWaterTime(static_cast<float>(waterTimeSeconds));
            // T21b：级联分割与各级光空间矩阵由 game 每帧按相机参数算出（engine 不认识相机设置），
            // 经 BuildShadowUniform 单入口投影成片元 uniform 槽 2 的参数块；级数 / 分辨率来自配置。
            // 缺陷 1：投射体扩展需要"最高投射体相对渲染原点的高度"——由已加载地形推导：
            //   casterTopRelative = 最高地表高度（世界 Y，格）− 渲染原点 Y
            // 与级联中心同坐标系（都是渲染原点相对），故引擎侧 `casterTopRelative − center.y` 即
            // "最高地形高度 − 该级切片中心高度"。地形可被笔刷挖/堆，故每帧重算（仅遍历已加载 tile）。
            const vx::CameraSettings& cameraSettings = camera.Settings();
            const float               maxSurfaceBlocks = world.MaxSurfaceHeightBlocks();
            const float               casterTopRelative =
                std::max(0.0F, maxSurfaceBlocks - static_cast<float>(renderOrigin.y));
            const vx::ShadowUniform   shadowUniform  = vx::BuildShadowUniform(
                lighting, relativeView.view, cameraSettings.fieldOfViewDegrees, cameraSettings.aspectRatio,
                cameraSettings.nearPlane, cameraSettings.farPlane, casterTopRelative);
            renderer.SetShadowCascades(shadowUniform, static_cast<std::uint32_t>(lighting.Shadow().cascadeCount),
                                       static_cast<std::uint32_t>(lighting.Shadow().resolution));
            const double uniformMs = uniformTimer.EndMs();
            // T24：渲染提交相位（RenderFrame 内含相机常量与动态顶点等内部上传）。
            renderTimer.Begin();
            if (!renderer.RenderFrame(frameHandles.data(), frameHandles.size(), clearColor, &debugOverlay)) {
                VX_LOG_DEBUG("本帧未取得交换链纹理（窗口最小化？），跳过渲染");
            }
            const double renderMs = renderTimer.EndMs();
            cpuCost = CpuFrameCost { logicMs, uiMs, renderMs };  // 本帧值：面板在下一帧读、尖峰日志在帧末读

            // T17：帧末补睡到目标间隔，限制帧率。垂直同步档 `TargetFps() == 0`，本调用立即返回。
            // 只用睡眠、绝不忙等（见 FrameLimiter 注释）。
            // T45：**限帧与呈现**独立计时（原先落在未计时区，也是"帧率掉但三相都不高"的常见解释）。
            throttleTimer.Begin();
            (void)frameLimiter.Throttle();
            throttleMs = throttleTimer.EndMs();

            // ---- T38 / T45 / T79② 帧尖峰打点（`references/performance-and-hitches.md` §2 的第一步）----
            // **帧末打印**（T79② 起）：本帧的全部相位都已量完（输入 / 逻辑 / UI / 剔除 / 动态上传 / uniform /
            // 提交 / 限帧），且 `frameMs` 由**帧首启动的 `frameTimer`** 量出 ⇒ 与各段**同源同区间**，
            // 各段之和与它之差才是真正的"未计时"（旧实现在逻辑相位内采样 + 帧中打印，读数错位一帧，
            // 会打出 100+ ms 的假未计时）。
            // 一条日志里同时给出各段 CPU + draw call + 提交网格数 + 等交换链耗时，据此可立刻区分
            // 「CPU 忙 / GPU 忙 / 在空等」。
            const double frameMs = frameTimer.EndMs();
            if (frameMs > kHitchThresholdMs && hitchLogClock.Tick() * 1000.0 >= kHitchLogMinIntervalMs) {
                (void)hitchLogClock.Tick();  // 重置节流窗口（节流口径不变，观测本身不制造新卡顿）
                const double measuredMs = inputMs + cpuCost.logicMs + cpuCost.uiMs + cullMs + rebaseMs +
                                          dynamicUploadMs + uniformMs + cpuCost.renderMs + throttleMs;
                const double untimedMs = std::max(0.0, frameMs - measuredMs);
                VX_LOG_WARN("帧尖峰 %.1f ms（阈值 %.0f ms）：输入 %.2f + 逻辑 %.2f + UI %.2f + 剔除 %.2f + "
                            "重定基 %.2f + 动态上传 %.2f + uniform %.2f + 渲染提交 %.2f + 限帧 %.2f = %.2f，"
                            "**未计时 %.2f** ms；draw call %u、提交网格 %zu、固定步 %d、等交换链 %.2f ms ⇒ 主要受限在 %s",
                            frameMs, kHitchThresholdMs, inputMs, cpuCost.logicMs, cpuCost.uiMs, cullMs, rebaseMs,
                            dynamicUploadMs, uniformMs, cpuCost.renderMs, throttleMs, measuredMs, untimedMs,
                            renderStats.drawCalls, submittedThisFrame, plan.steps, renderStats.swapchainWaitMs,
                            (renderStats.swapchainWaitMs > cpuCost.renderMs * 0.5)
                                ? "等交换链（GPU / 呈现）"
                                : "CPU 侧（逻辑 / UI / 提交）");
            }

            // V2b：会话墙钟推进 —— `Clock::ElapsedSeconds()` **只在 `Tick()` 时累计**，
            // 而 `--switch-test` 的秒数以它为准 ⇒ 必须每帧 `Tick` 一次（否则它恒为 0，到点判断永不成立）。
            (void)sessionClock.Tick();

            // V2b：本帧末尾若已有**待处理的世界切换请求**（`--switch-test` 或 V3 的交互点）⇒ 结束本轮世界：
            // 跳出主循环，由外层"世界装载循环"的尾部执行**卸载**并在下一轮**装配新世界**。
            // 放在帧末（而非请求处立即跳）：保证退出前的那一帧是**完整的一帧**（不半途而废）。
            if (worldManager.HasPendingSwitch()) {
                break;
            }
        }

        // ================================================================================
        // V2b：本轮世界结束 —— **卸载**（交还该世界创建的 GPU 网格）并决定**退出**或**切到下一个世界**。
        // 为什么必须显式交还：这些句柄是**本轮的局部变量**，析构只销毁句柄值、**不会**把槽位还给渲染器；
        // 不交还的话每切一次世界，`MeshRenderer` 的网格槽位就永久多一批（V2b 验收判据 = "卸载不留残"）。
        // Jolt 物体不需逐个移除：`PhysicsWorld` 等世界级对象都是**本轮局部变量**，随作用域析构。
        // ================================================================================
        for (vx::MeshHandle& handle : tileHandles) {
            if (handle.IsValid()) { renderer.ReleaseMesh(handle); }
        }
        for (auto& entry : volumeSlots) {
            if (entry.second.handle.IsValid()) { renderer.ReleaseMesh(entry.second.handle); }
        }
        for (vx::MeshHandle& handle : shellHandles) {
            if (handle.IsValid()) { renderer.ReleaseMesh(handle); }
        }
        for (ObjectSlot& slot : objectSlots) {
            if (slot.handle.IsValid()) { renderer.ReleaseMesh(slot.handle); }
        }
        for (vx::MeshHandle& handle : orbHandles) {
            if (handle.IsValid()) { renderer.ReleaseMesh(handle); }
        }
        if (waterHandle.IsValid()) { renderer.ReleaseMesh(waterHandle); }
        if (characterMesh.IsValid()) { renderer.ReleaseMesh(characterMesh); }
        rigidCollapse.ReleasePool(renderer);
        VX_LOG_INFO("世界卸载完成（V2b）：交还 GPU 网格 —— tile %zu / 体积块 %zu / 地表壳 %zu / 物件 %zu / 光球 %zu"
                    "（另含水面 / 主角 / 倒塌网格池）；渲染器网格槽位 %zu（反复切换应**趋于稳定** = 无泄漏）",
                    tileHandles.size(), volumeSlots.Size(), shellHandles.size(), objectSlots.size(),
                    orbHandles.size(), renderer.MeshSlotCount());

        if (quitRequested) {
            break;  // 退出世界装载循环（随后走设置落盘与退出日志）
        }
        if (!worldManager.HasPendingSwitch()) {
            break;  // 既未退出也无切换请求（`while (true)` 只在退出时结束）⇒ 收尾
        }
        // 切换：取出请求并**确认新世界**。
        // 口径：装载失败会抛异常 ⇒ 进程直接退出（**不会**留下"半个新世界"），故这里先确认再于下一轮装配；
        // 若将来要"装载失败回退到原世界"，把 `CommitActive` 移到本轮世界阶段全部完成之后即可（V2c 可细化）。
        {
            const std::optional<std::string> target = worldManager.TakePendingSwitch();
            if (!target.has_value()) {
                break;
            }
            worldManager.CommitActive(*target);
            VX_LOG_INFO("世界切换（V2b）：开始装载 [%s]（地形预设 %s）—— 下一轮装配新世界",
                        target->c_str(), worldManager.Active().terrainPresetPath.c_str());
        }
        }  // for (;;)：进入下一轮，装配（新）世界

        // 设置落盘：正常退出、窗口关闭、面板退出游戏都走这里（落盘失败只告警，不阻断退出）。
        try {
            vx::SaveSystemSettings(settingsPath, panelContext.settings);
            VX_LOG_INFO("设置已保存：%s", settingsPath.string().c_str());
        } catch (const std::exception& saveError) {
            VX_LOG_WARN("设置保存失败：%s", saveError.what());
        }

        VX_LOG_INFO("收到退出请求，主循环结束（累计 %.1f s）", sessionClock.ElapsedSeconds());
    } catch (const std::exception& error) {
        VX_LOG_ERROR("启动或主循环失败：%s", error.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
