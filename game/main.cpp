// V0.1 地表世界运行闭环：窗口 + 高度场 tile 生成 / 网格化 + Jolt 角色胶囊物理 +
// 第三人称相机 + 固定步长主循环 + 笔刷挖堆 + ImGui 调试面板。
//
// 分层与依赖方向（SKILL §2）：platform → engine core → world → game。
// 本文件位于最上层 game/，只消费下层接口：
//   - 事件 / 输入只在平台层（`Window::pump_events`）发生，这里只读 `InputMap` 的动作；
//   - 世界坐标用整数与 `double`（tile 坐标 + 1/16 格定点高度 + Jolt 的 `double` 位置），
//     上传 GPU 前做**相机相对偏移**转 float（红线 6）；
//   - 逻辑与物理按固定步长 1/60 s 推进，渲染只用插值系数 alpha，绝不把它写回模拟状态（红线 11）。

#include "character_mesh.hpp"
#include "character_movement.hpp"
#include "core/clock.hpp"
#include "core/fixed_step.hpp"
#include "core/frame_limiter.hpp"
#include "core/log.hpp"
#include "debug_overlay.hpp"
#include "gameplay_input.hpp"
#include "generation/map_preset.hpp"
#include "input/input_map.hpp"
#include "mouse_capture.hpp"
#include "out_of_bounds.hpp"
#include "physics/physics_world.hpp"
#include "platform/settings.hpp"
#include "platform/window.hpp"
#include "render/camera.hpp"
#include "render/lighting_table.hpp"
#include "render/mesh_renderer.hpp"
#include "render/shadow_cascade.hpp"
#include "terrain/material_table.hpp"
#include "terrain/material_textures.hpp"
#include "terrain/terrain_collision.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"
#include "terrain/world_bounds.hpp"
#include "dig/terrain_brush.hpp"

#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

namespace {

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

// ---------------------------------------------------------------
// 角色物理参数（T7 验收口径：20 格/秒冲刺不穿地形；1 格台阶可自动上步）
// ---------------------------------------------------------------

constexpr float kGravity                    = 24.0F;  ///< 重力加速度（格/秒²）
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

/// 把 tile 网格改写成**相机相对**的上传数据。
///
/// 世界定位在这里完成最后一次转换：tile 内定点坐标（float）先升到 `double` 加上 tile 的世界整数原点，
/// 再减去 `double` 渲染原点，最后才落回 `float`（红线 6）。因此进入 GPU 的世界定位永远是小数值，
/// 顶点 float 的有效位不会被千里之外的大坐标吃掉。
[[nodiscard]] vx::MeshData BuildRenderMesh(const vx::TerrainTileMesh& tileMesh, const glm::dvec3& renderOrigin) {
    vx::MeshData mesh = tileMesh.mesh;  // 索引沿用，只改写顶点位置

    const double originX = static_cast<double>(vx::TileOriginColumn(tileMesh.coord.x));
    const double originZ = static_cast<double>(vx::TileOriginColumn(tileMesh.coord.z));

    for (vx::MeshVertex& vertex : mesh.vertices) {
        vertex.position[0] = static_cast<float>(originX + static_cast<double>(vertex.position[0]) - renderOrigin.x);
        vertex.position[1] = static_cast<float>(static_cast<double>(vertex.position[1]) - renderOrigin.y);
        vertex.position[2] = static_cast<float>(originZ + static_cast<double>(vertex.position[2]) - renderOrigin.z);
    }
    return mesh;
}

/// 上传（或重传）一个已网格化 tile 的 GPU 网格。
void UploadTileMesh(vx::MeshRenderer& renderer, vx::MeshHandle& handle, const vx::TerrainWorld& world,
                    const vx::TileCoord& coord, const glm::dvec3& renderOrigin) {
    const vx::TerrainTileMesh* tileMesh = world.FindMesh(coord.x, coord.z);
    if (tileMesh == nullptr) {
        return;
    }
    if (handle.IsValid()) {
        renderer.ReleaseMesh(handle);
        handle = vx::MeshHandle {};
    }
    handle = renderer.UploadMesh(BuildRenderMesh(*tileMesh, renderOrigin));
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

/// 把主角胶囊的**局部**顶点（脚底为原点）搬到**相机相对**空间：`world = 脚底 + 局部 - 渲染原点`。
///
/// 与地表网格同一约定（红线 6）：世界定位用 `double` 累加后再落回 `float`，且减去当前渲染原点，
/// 因此渲染原点重定基时不会抖动。就地改写 `vertices`（长度与 `local` 一致），避免每帧堆分配。
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

/// 推进一个固定逻辑步：把移动指令按相机 yaw 转到世界方向，交给 Jolt `CharacterVirtual` 求解
/// （重力 / 上坡 / **自动上台阶**都在物理层内完成），再让第三人称相机跟随角色。
///
/// T12 飞行模式：关闭重力（`gravity = 0`）并直接给竖直速度，使角色可自由升 / 降、悬停；
/// 切回普通模式时由调用方清零速度，故不会残留速度、也不会穿过地形（碰撞仍在生效）。
void StepCharacter(vx::PhysicsWorld& physics, vx::PhysicsWorld::CharacterHandle character,
                   vx::ThirdPersonCamera& camera, const MoveCommand& command, bool flying) {
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
    } else {
        if (lengthSq > 0.0F) {
            velocity.x = direction.x * command.speed;
            velocity.z = direction.z * command.speed;
        }
        if (command.jump && state.onGround) {
            velocity.y = kJumpSpeed;  // 竖直分量由玩法层给冲量，重力由物理层在步内累加
        }
        physics.SetCharacterVelocity(character, velocity);
        physics.MoveCharacter(character, static_cast<float>(vx::kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));
    }

    const vx::PhysicsWorld::CharacterState after = physics.GetCharacterState(character);
    camera.Advance(glm::vec3(static_cast<float>(after.position.x), static_cast<float>(after.position.y),
                             static_cast<float>(after.position.z)));
}

/// 笔刷动作（T26）：右键 = 填平、左键 = 削平、Shift + 左键 = 爆破演示。
enum class BrushAction {
    Fill,    ///< 平整填充：半径内向施力点高度收敛（只抬升低处）
    Shave,   ///< 削平：半径内向施力点高度收敛（只削低高处）
    Crater,  ///< 爆破演示：下挖 + 外环隆起（为战斗破坏地形系统做的能力入口）
};

/// 对当前施力点执行一次笔刷操作，**只重网格、只重传、只重建**受影响的 tile
/// （红线：禁止整世界重网格）。参数全部来自 `brush.toml`（唯一事实来源）。
/// 返回本次被弄脏的 tile 数（供调试面板显示）。
std::size_t ApplyBrush(vx::TerrainWorld& world, vx::TerrainCollision& collision, vx::MeshRenderer& renderer,
                       const std::vector<vx::TileCoord>& tileCoords, std::vector<vx::MeshHandle>& tileHandles,
                       const glm::vec3& focus, const vx::BrushSettings& settings, BrushAction action, float dt,
                       const glm::dvec3& renderOrigin) {
    vx::BrushPose brush;
    brush.centerX = focus.x;
    brush.centerZ = focus.z;
    brush.radius  = settings.radius;

    // 平整的目标高度 = **施力点**（脚下 / 视线落点）处的地表高度：把范围内的低处填到它、高处削到它。
    float targetHeight = 0.0F;
    if (!world.QueryHeight(brush.centerX, brush.centerZ, targetHeight)) {
        return 0;
    }

    const char*     actionName = "平整填平";
    vx::BrushResult result;
    switch (action) {
        case BrushAction::Fill:
            result = vx::ApplyTerrainLevel(world, brush, targetHeight, settings.strength * dt, settings.falloff,
                                           vx::LevelMode::Fill);
            break;
        case BrushAction::Shave:
            actionName = "削平";
            result = vx::ApplyTerrainLevel(world, brush, targetHeight, settings.strength * dt, settings.falloff,
                                           vx::LevelMode::Shave);
            break;
        case BrushAction::Crater:
            actionName = "爆破演示";
            result = vx::ApplyTerrainCrater(world, brush, settings.craterDepth, settings.craterRim,
                                            settings.craterRadius, settings.falloff);
            break;
    }
    if (result.changedColumns == 0) {
        return 0;
    }

    const std::size_t remeshed = world.RemeshDirtyTiles(result.dirtyTiles);
    for (const vx::TileCoord& coord : result.dirtyTiles) {
        for (std::size_t i = 0; i < tileCoords.size(); ++i) {
            if (tileCoords[i] == coord) {
                UploadTileMesh(renderer, tileHandles[i], world, coord, renderOrigin);
                break;
            }
        }
    }
    // 高度变了 → 重建这些 tile 的物理碰撞体（T7：高度变化后重建 HeightFieldShape）。
    const std::size_t rebuiltBodies = collision.SyncTiles(world, result.dirtyTiles);

    // 每帧持续施力 ⇒ 用 DEBUG 级别（避免刷屏），只在确实改动时记录。
    VX_LOG_DEBUG("笔刷[%s]（半径 %.1f 格）作用于 (%.1f, %.1f)：改动 %zu 列，重网格 %zu 个 tile，重建碰撞体 %zu 个",
                 actionName, static_cast<double>(settings.radius), static_cast<double>(brush.centerX),
                 static_cast<double>(brush.centerZ), result.changedColumns, remeshed, rebuiltBodies);
    return result.dirtyTiles.size();
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

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path shaderDir = ResolveShaderDir(argv[0]);
    if (argc > 1) {
        shaderDir = argv[1];
    }

    try {
        const vx::TerrainMaterialTable materials =
            vx::TerrainMaterialTable::LoadFromFile(SourceAssetPath("assets/config/materials.toml"));

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

        // T26：笔刷配置（平整 / 削平 / 爆破的参数）。与材质表 / 光照表**同源解析**（同一个 SourceAssetPath，
        // 同一个 toml++）；加载失败（缺失 / 语法错 / 校验不过 / schema_version 不符）抛异常 → 启动失败，
        // 与其它配置表口径一致：**禁止静默回退**。
        const vx::BrushTable      brushTable    = vx::BrushTable::LoadFromFile(SourceAssetPath("assets/config/brush.toml"));
        const vx::BrushSettings&  brushSettings = brushTable.Settings();
        VX_LOG_INFO("笔刷配置已加载（schema_version=%d）：半径 %.1f 格，平整速率 %.1f 格/秒，衰减带 %.2f；"
                    "爆破 深 %.1f / 坑半径 %.1f / 外环 %.1f 格",
                    brushTable.SchemaVersion(), static_cast<double>(brushSettings.radius),
                    static_cast<double>(brushSettings.strength), static_cast<double>(brushSettings.falloff),
                    static_cast<double>(brushSettings.craterDepth), static_cast<double>(brushSettings.craterRadius),
                    static_cast<double>(brushSettings.craterRim));

        // T11：从预设地图构建世界（种子 / 范围 / 地形编辑全部来自文件，不再硬编码）。
        const std::filesystem::path mapPath = SourceAssetPath(kDefaultMapFile);
        const vx::MapPreset         preset  = vx::MapPreset::LoadFromFile(mapPath);

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

        // 应用设置：窗口模式恢复客户区尺寸；全屏走桌面无边框全屏。
        if (vx::IsResolutionEditable(systemSettings.displayMode)) {
            (void)window.SetWindowSize(systemSettings.windowWidth, systemSettings.windowHeight);
        } else {
            (void)window.SetFullscreen(true);
        }
        vx::ApplyMasterVolumeGain(systemSettings.masterVolume);

        // T17：帧率限速。目标 == 刷新率 → 垂直同步；低于刷新率 → 睡眠限帧（禁止忙等）。
        vx::FrameLimiter frameLimiter;
        ApplyFrameRateCap(window, frameLimiter, systemSettings.frameRateCap, displayRefreshRate);

        vx::TerrainWorld world(preset.seed, materials);
        world.SetMapPreset(preset);  // 噪声先行、编辑覆盖其上（必须在 LoadTile 之前）

        // 地图范围由预设的 tile 半径决定：tile ∈ [-r, r] → 世界列 ∈ [-r*64, r*64]。
        std::vector<vx::TileCoord>  tileCoords;
        std::vector<vx::MeshHandle> tileHandles;
        const std::size_t           tilesX = static_cast<std::size_t>(2 * preset.tileRadiusX + 1);
        const std::size_t           tilesZ = static_cast<std::size_t>(2 * preset.tileRadiusZ + 1);
        tileCoords.reserve(tilesX * tilesZ);
        tileHandles.reserve(tilesX * tilesZ);
        for (int tileZ = -preset.tileRadiusZ; tileZ <= preset.tileRadiusZ; ++tileZ) {
            for (int tileX = -preset.tileRadiusX; tileX <= preset.tileRadiusX; ++tileX) {
                world.LoadTile(tileX, tileZ);  // 生成 + 网格化
                tileCoords.push_back(vx::TileCoord { tileX, tileZ });
                tileHandles.push_back(vx::MeshHandle {});
            }
        }
        VX_LOG_INFO("预设地图已加载：%s（文件 %s）—— 种子 %llu，tile 半径 [%d, %d]（%zu 个 tile），"
                    "地形编辑 %zu 条，出生点 (%.1f, %.1f)",
                    preset.name.c_str(), mapPath.string().c_str(), static_cast<unsigned long long>(preset.seed),
                    preset.tileRadiusX, preset.tileRadiusZ, tileCoords.size(), preset.edits.size(), preset.spawnX,
                    preset.spawnZ);

        // 物理世界 + 地表碰撞体（每个地表 tile 一个 HeightFieldShape）。
        vx::PhysicsWorld    physics;
        vx::TerrainCollision terrainCollision(physics);
        const std::size_t    collisionTiles = terrainCollision.SyncTiles(world, tileCoords);

        // T18：世界边界由**地图范围自动推导**（tile_radius → 世界列范围），不硬编码：换地图或将来
        // 改由程序化决定大小时自动跟随。四周建**不可见**的静态墙挡住地面行走；出界救援（见主循环）
        // 另兜住"飞越墙后坠落"。
        const vx::WorldBounds                 bounds = vx::ComputeWorldBounds(preset.tileRadiusX, preset.tileRadiusZ);
        const std::array<vx::BoundaryWall, 4> boundaryWalls =
            vx::ComputeBoundaryWalls(bounds, vx::kBoundaryWallThickness);
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
        VX_LOG_INFO("世界边界（由 tile 半径 [%d, %d] 自动推导）：范围 (%.1f, %.1f, %.1f) ~ (%.1f, %.1f, %.1f)；"
                    "不可见围墙 %zu/4 个，厚 %.1f 格；出界救援余量 %.1f 格",
                    preset.tileRadiusX, preset.tileRadiusZ, bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x,
                    bounds.max.y, bounds.max.z, boundaryWallBodies, vx::kBoundaryWallThickness,
                    vx::kOutOfBoundsMargin);

        vx::MeshRenderer renderer(window.device(), window.handle(), shaderDir);

        // T20 / ADR 0010：把配置里的曝光交给色调映射通道（参数进配置，改值不需重编 Shader）。
        renderer.SetExposure(systemSettings.exposure);

        // T23 / ADR 0010 P3：把配置里的 MSAA 档位交给渲染器（引擎层不读配置文件；档位 = 1 时零额外开销）。
        renderer.SetMsaaSampleCount(static_cast<std::uint32_t>(systemSettings.msaaSamples));

        // T22 / ADR 0010 P2：程序生成的占位材质贴图（多尺度 albedo / 法线 + roughness + AO + 宏观变化），
        // 上传为五个 2D 纹理数组（albedo/normal/roughness/AO 各 4 层，macro 1 层），供片元着色器逐像素混合
        // 并做 PBR。无二进制资产、种子确定性。显存由 CreateTextureArray 自动计入 RenderStats::textureBytes。
        const vx::MaterialTextureSet materialTextures = vx::GenerateMaterialTextures(preset.seed);
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
        {
            // 材质数组总量 = 每层第 0 级字节 × 4/3（mip）× 总层数；层数 = 4×4 + 1（macro）。
            const double mipFactor = 4.0 / 3.0;
            const double baseBytes = static_cast<double>(materialTextures.size) * materialTextures.size * 4.0;
            const double totalLayerCount =
                static_cast<double>(materialTextures.layerCount) * 4.0 + static_cast<double>(vx::kMaterialMacroLayerCount);
            VX_LOG_INFO("材质贴图已生成并上传：%u×%u，R8G8B8A8_UNORM；albedo/normal/roughness/AO 各 %u 层 + macro %u 层，"
                        "含 mip 约 %.2f MB 显存（第 0 级 %.2f MB/层）",
                        materialTextures.size, materialTextures.size, materialTextures.layerCount,
                        vx::kMaterialMacroLayerCount, baseBytes * mipFactor * totalLayerCount / (1024.0 * 1024.0),
                        baseBytes / (1024.0 * 1024.0));
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

        vx::ThirdPersonCamera camera(settings);
        camera.SnapTo(glm::vec3(spawnX, static_cast<float>(capsule.position.y), spawnZ));
        camera.SetYaw(0.7F);
        camera.SetPitch(-0.42F);  // 略微俯视地表

        // 调试面板（T9）：基于 imgui 的 SDL3 + SDL3_gpu 后端；F1 开关。
        // T15：系统面板与它共用同一个 ImGui 上下文（由本对象托管），并通过事件转发变为**可交互**。
        vx::DebugOverlay debugOverlay(window.device(), window.handle());

        // T15：把 SDL 事件转发给 ImGui 后端（全生命周期只安装一次）。平台层仍是唯一读事件队列的地方，
        // game/ 只是注册回调；未接这一步之前面板只读，正是因为事件从未到达 ImGui。
        window.SetEventCallback(&vx::DebugOverlay::OnSdlEvent, &debugOverlay);

        // 系统面板（T15，Esc）：面板就地编辑一份设置副本，主循环据此调用平台层与落盘。
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

        // 渲染原点：整数世界定位，上传的 float 顶点都以它为基准（红线 6）。
        glm::dvec3 renderOrigin(std::floor(static_cast<double>(spawnX)), std::floor(static_cast<double>(spawnSurface)),
                                std::floor(static_cast<double>(spawnZ)));
        for (std::size_t i = 0; i < tileCoords.size(); ++i) {
            UploadTileMesh(renderer, tileHandles[i], world, tileCoords[i], renderOrigin);
        }

        // T13：主角**可视**胶囊体（装饰用，尺寸与碰撞胶囊一致；不参与任何物理）。
        // 一次性上传局部网格（脚底为原点），并把句柄追加到绘制列表末尾；此后每帧只就地刷新顶点位置。
        vx::CapsuleMeshSpec capsuleSpec;
        capsuleSpec.radius             = kCharacterRadius;
        capsuleSpec.cylinderHalfHeight = kCharacterCylinderHalfHeight;
        const vx::MeshData  capsuleLocalMesh = vx::BuildCapsuleMesh(capsuleSpec);
        const vx::MeshHandle characterMesh   = renderer.UploadMesh(capsuleLocalMesh);
        std::vector<vx::MeshVertex> characterVertices = capsuleLocalMesh.vertices;
        if (characterMesh.IsValid()) {
            tileHandles.push_back(characterMesh);  // 末尾槽位：地表 tile 之后绘制主角
        } else {
            VX_LOG_WARN("主角可视网格上传失败（网格为空），本帧起将看不到角色");
        }

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
        input.BindMouseButton(vx::ActionId::Attack, SDL_BUTTON_LEFT);   // 主笔刷：挖
        input.BindMouseButton(vx::ActionId::Use, SDL_BUTTON_RIGHT);     // 副笔刷：堆
        input.BindMouseAxis(vx::ActionId::LookX, vx::MouseAxis::X);
        input.BindMouseAxis(vx::ActionId::LookY, vx::MouseAxis::Y);

        vx::Clock                clock;
        vx::FixedStepAccumulator accumulator(vx::kFixedDt);

        // T20 / T21a：主通道写 HDR 目标，清屏色须按**线性光**给出（色调映射通道最后编码到 sRGB）。
        // T21a 起不再写死：取配置里的**天空地平色**（sRGB 作者色 → 线性）。为什么是地平色而不是天顶色：
        // 清屏色就是"没有几何处的天空背景"，而远景地形会被雾混向**地平色**
        // （fog.color 缺失时默认 = sky.horizon_color），两者同源才能让远景与天空无缝衔接、无硬边。
        const vx::ColorRgb clearLinear = vx::SrgbToLinear(lighting.Sky().horizonColor);
        const SDL_FColor   clearColor { clearLinear[0], clearLinear[1], clearLinear[2], 1.0F };

        // T24：CPU 帧时间分解的相位计时器（逻辑步 / UI 构建 / 渲染提交）。
        PhaseTimer   logicTimer;
        PhaseTimer   uiTimer;
        PhaseTimer   renderTimer;
        CpuFrameCost cpuCost;  // 上一帧的值（面板早于本帧渲染构建，与 frameSeconds 同源）

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

        // T26：重新捕获鼠标的那一次点击**不落到笔刷上**（直到松开按键）。笔刷改为持续输入后，
        // 若只在按下帧抑制，按住不放会在下一帧立刻开始施力，等于把"捕获点击"变成了笔刷操作；
        // 故用锁存：捕获点击被消费时置位，两个笔刷键都松开时清零。
        bool brushSuppressUntilRelease = false;

        // 跳跃请求**帧级锁存**（缺陷 B3）：主循环在 Mailbox 下可达上千 FPS，而逻辑 / 物理是 60 Hz 固定步，
        // 多数帧的 `StepPlan::steps` 为 0。若在帧边界直接消费"本帧按下"边沿，该边沿会在没有逻辑步的帧上
        // 被静默丢弃（实测 ~1500 FPS 时只有 ~4% 的帧有逻辑步 → 96% 的空格按下丢失）。故先锁存，
        // 交给**第一个真正执行的固定步**，再由该步消费掉。
        bool jumpRequested = false;

        VX_LOG_INFO("地表世界就绪：种子 %llu，tile %zu 个，材质表 schema_version=%d，笔刷半径 %.1f 格",
                    static_cast<unsigned long long>(preset.seed), tileCoords.size(), materials.SchemaVersion(),
                    static_cast<double>(brushSettings.radius));
        VX_LOG_INFO("角色物理就绪：地表碰撞体 %zu 个 tile；胶囊 半径 %.2f / 总高 %.2f 格；"
                    "重力 %.1f、跳跃初速 %.2f（由身高推导，最高点 %.2f 格 = 身高 %.0f%%）、"
                    "最大坡度 %.0f°、自动上台阶 %.1f 格（dt=1/60）",
                    collisionTiles, static_cast<double>(kCharacterRadius), static_cast<double>(kCharacterHeight),
                    static_cast<double>(kGravity), static_cast<double>(kJumpSpeed),
                    static_cast<double>(vx::kJumpApexHeightRatio * kCharacterHeight),
                    static_cast<double>(vx::kJumpApexHeightRatio * 100.0F),
                    static_cast<double>(kCharacterMaxSlopeDegrees), static_cast<double>(kCharacterStepUpHeight));
        VX_LOG_INFO("ImGui 已启用（事件转发已接）：F1 调试面板（当前%s）；Esc 系统面板（当前%s）；两个面板均可交互",
                    debugOverlay.Visible() ? "显示" : "隐藏", debugOverlay.SystemPanelOpen() ? "打开" : "关闭");
        VX_LOG_INFO("控制说明：W/A/S/D = 移动；Shift = 冲刺；Space = 跳（飞行中 = 上升）；"
                    "F = 切换飞行模式；飞行中 左Ctrl = 下降（Shift 加速）；鼠标移动 = 环视（已捕获，可转满 ±89°）；"
                    "**鼠标右键 = 平整填平**（把半径 %.1f 格内的低处填到脚下高度）；"
                    "**鼠标左键 = 削平**（把高于脚下高度的部分削掉）；"
                    "**Shift + 鼠标左键 = 爆破演示**（下挖 %.1f 格 / 坑半径 %.1f 格 / 外环 %.1f 格）；"
                    "笔刷为按住持续施力（速率 %.1f 格/秒，参数见 brush.toml）；"
                    "Esc = 开关系统面板（打开时释放鼠标、关闭时恢复）；"
                    "点击窗口 = 重新捕获（该次点击不施力）；F1 = 调试面板；关闭窗口 = 退出",
                    static_cast<double>(brushSettings.radius), static_cast<double>(brushSettings.craterDepth),
                    static_cast<double>(brushSettings.craterRadius), static_cast<double>(brushSettings.craterRim),
                    static_cast<double>(brushSettings.strength));

        std::size_t lastDirtyTiles = 0;

        while (window.pump_events(input)) {
            input.BeginFrame();  // 每帧采样一次，且只在固定步循环之外

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

            // 起 ImGui 帧（任一面板可见时）：必须先于读取捕获标志，且早于玩法输入处理。
            // 捕获期间让 ImGui 忽略鼠标（相对模式坐标无意义），避免误判悬停而抑制视角。
            // T24：ImGui 帧开销计入 UI 构建耗时（NewFrame 与面板构建是两段，累加）。
            uiTimer.Begin();
            debugOverlay.SetGameplayMouseCaptured(mouseCaptured);
            debugOverlay.BeginFrame();
            double uiMs = uiTimer.EndMs();

            // T15：玩法输入抑制——面板打开或 ImGui 想接管鼠标 / 键盘时，吞掉对应类别，
            // 使"点按钮"不会挖地、"拖音量"不会转相机。决策为纯函数（见 `gameplay_input.hpp`）。
            const vx::InputSuppression suppression =
                vx::DecideInputSuppression(debugOverlay.SystemPanelOpen(), debugOverlay.WantsCaptureMouse(),
                                           debugOverlay.WantsCaptureKeyboard());

            // T14 捕获状态机（仅在系统面板关闭时）：未捕获时的点击用于重新捕获，状态机把它标记为
            // "已被捕获消费"，随后消费掉 Attack / Use 边沿，使这次点击绝不会落到笔刷上。
            // 面板打开时整体跳过：此时点击属于面板控件，绝不能触发重捕获。该顺序由单测钉死。
            if (!debugOverlay.SystemPanelOpen()) {
                const bool anyClickEdge = input.Pressed(vx::ActionId::Attack) || input.Pressed(vx::ActionId::Use);
                // `escapePressed` 恒为 false：Esc 已改由上面的系统面板消费（T15 统一语义）。
                const vx::MouseCaptureDecision captureDecision =
                    vx::DecideMouseCapture(mouseCaptured, /*escapePressed=*/false, anyClickEdge);
                if (captureDecision.captureRequested) {
                    mouseCaptured = window.SetRelativeMouseMode(true);
                    VX_LOG_INFO("鼠标捕获：%s（点击重新捕获；本次点击已被捕获消费，不触发挥 / 堆）",
                                mouseCaptured ? "开" : "关（SDL 未接受，请再点一次）");
                }
                if (captureDecision.clickConsumedByCapture) {
                    // 消费本帧的鼠标点击边沿：重新捕获的这一次点击到此为止，绝不落到笔刷上。
                    (void)input.ConsumePressed(vx::ActionId::Attack);
                    (void)input.ConsumePressed(vx::ActionId::Use);
                    brushSuppressUntilRelease = true;  // 直到松开按键才解除（见其声明处说明）
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

            // 笔刷：T26 起为**持续输入**（按住即连续施力），故这里只消费点击边沿（避免残留），
            // 实际施力放在固定步循环之后，按固定步长折算速率（红线 11）。
            (void)input.ConsumePressed(vx::ActionId::Attack);
            (void)input.ConsumePressed(vx::ActionId::Use);

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

            // T24：逻辑步相位（固定步循环：物理 + 相机 + 出界检查）。
            logicTimer.Begin();
            const vx::StepPlan plan = accumulator.Advance(clock.Tick());
            for (int step = 0; step < plan.steps; ++step) {
                StepCharacter(physics, character, camera, command, flying);

                // T18 出界救援：墙挡不住"飞越墙顶后坠落"，故在**每个固定步后**检查角色是否已掉出世界。
                // 命中则重用 `SetCharacterPosition`（内部会把位置瞬移并清零速度）送回出生点，并
                // `SnapTo` 相机以消除插值拖影。送回后位置落在边界盒内，下一帧不会再触发
                // （该性质由 `OutOfBounds.SingleRescueDoesNotRetriggerOnNextFrame` 钉死）——
                // 因此日志天然"每次出界只记一次"，不会逐帧刷屏。
                if (vx::IsCharacterOutOfBounds(physics.GetCharacterState(character).position, bounds,
                                               vx::kOutOfBoundsMargin)) {
                    physics.SetCharacterPosition(character, spawnPosition);
                    camera.SnapTo(glm::vec3(static_cast<float>(spawnPosition.x),
                                            static_cast<float>(spawnPosition.y),
                                            static_cast<float>(spawnPosition.z)));
                    VX_LOG_WARN("角色出界（超出边界 %.0f 格余量）→ 已送回出生点 (%.1f, %.1f, %.1f)",
                                vx::kOutOfBoundsMargin, spawnPosition.x, spawnPosition.y, spawnPosition.z);
                }
            }
            // 至少跑过一个逻辑步后，锁存的跳跃请求已被判定过（含"不满足着地条件而放弃"），消费掉。
            if (plan.steps > 0) {
                jumpRequested = false;
            }
            const double logicMs = logicTimer.EndMs();

            // T26 笔刷：按住左键 = 削平；按住右键 = 平整填平；Shift + 左键 = 爆破演示。
            // 参数全部来自 brush.toml（唯一事实来源）；速率按**固定步长**折算（红线 11：不用可变帧间隔）。
            // 松开任一键即解除"捕获点击"抑制。
            if (!input.Held(vx::ActionId::Attack) && !input.Held(vx::ActionId::Use)) {
                brushSuppressUntilRelease = false;
            }
            const bool brushAllowed = mouseCaptured && !suppression.mouseBrush && !brushSuppressUntilRelease;
            const bool shaveHeld    = brushAllowed && input.Held(vx::ActionId::Attack);
            const bool fillHeld     = brushAllowed && input.Held(vx::ActionId::Use);
            if (plan.steps > 0 && (shaveHeld || fillHeld)) {
                const bool        shiftHeld = input.Held(vx::ActionId::Sprint);
                const BrushAction brushAction =
                    shaveHeld ? (shiftHeld ? BrushAction::Crater : BrushAction::Shave) : BrushAction::Fill;
                const float brushDt = static_cast<float>(vx::kFixedDt) * static_cast<float>(plan.steps);
                lastDirtyTiles =
                    ApplyBrush(world, terrainCollision, renderer, tileCoords, tileHandles, camera.TargetCurrent(),
                               brushSettings, brushAction, brushDt, renderOrigin);

                // 抬升地形后把被埋住的角色**顶回新地表**（缺陷 B2 的物理侧）。
                // Jolt 的静态高度场在 `SetShape` 之后不会把 `CharacterVirtual` 推出去：角色一旦被抬高的
                // 地表埋住，支撑判定失效，它会在重力下穿过高度场、带着相机钻到地下（画面只剩清屏色）。
                // 因此这里把低于地表的角色放回地表，并把相机吸附到同一位置，避免一帧的插值拖影。
                // 填平 / 爆破会抬高地表；削平只降低地表，此检查幂等、无副作用。
                const vx::PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
                float                                    surface = 0.0F;
                if (world.QueryHeight(static_cast<float>(state.position.x), static_cast<float>(state.position.z),
                                      surface) &&
                    state.position.y < static_cast<double>(surface)) {
                    const glm::dvec3 lifted(state.position.x, static_cast<double>(surface), state.position.z);
                    physics.SetCharacterPosition(character, lifted);
                    camera.SnapTo(glm::vec3(static_cast<float>(lifted.x), static_cast<float>(lifted.y),
                                            static_cast<float>(lifted.z)));
                }
            }

            // 浮点原点重定基：相机漂移过远时把渲染原点搬到相机附近并整体重传（低频，不在热路径上）。
            const glm::vec3 focus = camera.TargetCurrent();
            const glm::dvec3 focusDouble(static_cast<double>(focus.x), static_cast<double>(focus.y),
                                         static_cast<double>(focus.z));
            if (glm::distance(renderOrigin, focusDouble) > kRebaseDistance) {
                renderOrigin = glm::dvec3(std::floor(focusDouble.x), std::floor(focusDouble.y),
                                          std::floor(focusDouble.z));
                for (std::size_t i = 0; i < tileCoords.size(); ++i) {
                    UploadTileMesh(renderer, tileHandles[i], world, tileCoords[i], renderOrigin);
                }
                VX_LOG_INFO("渲染原点重定基到 (%.0f, %.0f, %.0f)", renderOrigin.x, renderOrigin.y, renderOrigin.z);
            }

            // T13：每帧把主角胶囊改写为相机相对顶点并就地刷新。位置取相机目标的插值位置，
            // 与渲染插值一致（alpha 只用于渲染，绝不回写模拟状态，红线 11）；装饰用，不影响碰撞。
            if (characterMesh.IsValid()) {
                const glm::vec3 feetRender =
                    glm::mix(camera.TargetPrevious(), camera.TargetCurrent(), static_cast<float>(plan.alpha));
                const glm::dvec3 feetRenderDouble(static_cast<double>(feetRender.x), static_cast<double>(feetRender.y),
                                                  static_cast<double>(feetRender.z));
                UpdateCharacterRenderVertices(characterVertices, capsuleLocalMesh, feetRenderDouble, renderOrigin);
                // 唯一可能的失败是句柄失效或顶点数变化，这里两者都不会发生（已在上面校验句柄）。
                (void)renderer.UpdateMeshVertices(characterMesh, characterVertices);
            }

            // 渲染：alpha 只用于在上一 / 当前逻辑状态之间插值，绝不回写模拟状态（红线 11）。
            const vx::CameraView view = camera.Evaluate(plan.alpha, &world);

            // 调试面板：统计经独立接口采集，只在渲染线程构建，不进世界层热路径。
            vx::DebugStats stats;
            const vx::PhysicsWorld::CharacterState characterState = physics.GetCharacterState(character);
            stats.frameSeconds      = clock.DeltaSeconds();
            stats.stepsThisFrame     = plan.steps;
            stats.frameRateCap       = panelContext.settings.frameRateCap;  // T17：F1 面板显示当前目标
            stats.characterPosition = characterState.position;
            stats.characterOnGround = characterState.onGround;
            stats.characterFlying   = flying;
            stats.cameraYaw         = camera.Yaw();
            stats.cameraPitch       = camera.Pitch();
            stats.cameraDistance    = view.distance;
            stats.brushRadius       = brushSettings.radius;
            stats.mouseCaptured     = mouseCaptured;
            stats.loadedTileCount   = tileCoords.size();
            stats.lastDirtyTileCount = lastDirtyTiles;
            stats.tileBodyCount     = terrainCollision.TileBodyCount();
            stats.physicsReady      = true;

            // T24：渲染开销取自引擎的通用统计；绘制数为**最近一次** RenderFrame（面板早于本帧渲染）。
            const vx::RenderStats& renderStats = renderer.Stats();
            stats.drawCalls     = renderStats.drawCalls;
            stats.triangleCount = renderStats.triangleCount;
            stats.vertexCount   = renderStats.vertexCount;
            stats.textureBytes  = renderStats.textureBytes;
            // T24：CPU 分解用**上一帧**的实测值（本帧渲染尚未提交，与 frameSeconds 同源）。
            stats.cpuLogicMs  = cpuCost.logicMs;
            stats.cpuUiMs     = cpuCost.uiMs;
            stats.cpuRenderMs = cpuCost.renderMs;

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
                break;
            }

            // 材质参数（高度带 / 坡度带 / UV 尺度 / 层色）来自与 TerrainWorld **同一份**材质表；
            // 渲染原点每次重定基后都要刷新（原点进 uniform，片元据此把相机相对坐标还原为世界坐标）。
            const vx::MaterialUniform materialUniform =
                vx::BuildMaterialUniform(world.Materials(), renderOrigin.x, renderOrigin.y, renderOrigin.z);
            renderer.SetMaterialUniform(&materialUniform, sizeof(materialUniform));

            // 光照与雾参数（T21a / T21c）：来自启动期加载的同一份光照表，经 BuildLightingUniform 单入口投影。
            // **相机世界位置每帧变化**（第三人对焦跟随 + 避障），而雾按视距插值，故 uniform 必须每帧重建。
            // `view.eye` 是绝对世界坐标，与片元还原出的 worldPosition 同空间。
            const vx::LightingUniform lightingUniform =
                vx::BuildLightingUniform(lighting, static_cast<double>(view.eye.x),
                                         static_cast<double>(view.eye.y), static_cast<double>(view.eye.z));
            renderer.SetLightingUniform(&lightingUniform, sizeof(lightingUniform));

            const vx::CameraView relativeView = RelativeCameraView(view, renderOrigin);
            renderer.SetCamera(relativeView);

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
            // T24：渲染提交相位（RenderFrame 内含相机常量与动态顶点等内部上传）。
            renderTimer.Begin();
            if (!renderer.RenderFrame(tileHandles.data(), tileHandles.size(), clearColor, &debugOverlay)) {
                VX_LOG_DEBUG("本帧未取得交换链纹理（窗口最小化？），跳过渲染");
            }
            const double renderMs = renderTimer.EndMs();
            cpuCost = CpuFrameCost { logicMs, uiMs, renderMs };  // 供下一帧面板显示

            // T17：帧末补睡到目标间隔，限制帧率。垂直同步档 `TargetFps() == 0`，本调用立即返回。
            // 只用睡眠、绝不忙等（见 FrameLimiter 注释）。
            (void)frameLimiter.Throttle();
        }

        // 设置落盘：正常退出、窗口关闭、面板退出游戏都走这里（落盘失败只告警，不阻断退出）。
        try {
            vx::SaveSystemSettings(settingsPath, panelContext.settings);
            VX_LOG_INFO("设置已保存：%s", settingsPath.string().c_str());
        } catch (const std::exception& saveError) {
            VX_LOG_WARN("设置保存失败：%s", saveError.what());
        }

        VX_LOG_INFO("收到退出请求，主循环结束（累计 %.1f s）", clock.ElapsedSeconds());
    } catch (const std::exception& error) {
        VX_LOG_ERROR("启动或主循环失败：%s", error.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
