// V0.1 地表世界运行闭环：窗口 + 高度场 tile 生成 / 网格化 + Jolt 角色胶囊物理 +
// 第三人称相机 + 固定步长主循环 + 笔刷挖堆 + ImGui 调试面板。
//
// 分层与依赖方向（SKILL §2）：platform → engine core → world → game。
// 本文件位于最上层 game/，只消费下层接口：
//   - 事件 / 输入只在平台层（`Window::pump_events`）发生，这里只读 `InputMap` 的动作；
//   - 世界坐标用整数与 `double`（tile 坐标 + 1/16 格定点高度 + Jolt 的 `double` 位置），
//     上传 GPU 前做**相机相对偏移**转 float（红线 6）；
//   - 逻辑与物理按固定步长 1/60 s 推进，渲染只用插值系数 alpha，绝不把它写回模拟状态（红线 11）。

#include "core/clock.hpp"
#include "core/fixed_step.hpp"
#include "core/log.hpp"
#include "debug_overlay.hpp"
#include "input/input_map.hpp"
#include "physics/physics_world.hpp"
#include "platform/window.hpp"
#include "render/camera.hpp"
#include "render/mesh_renderer.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_collision.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"
#include "dig/terrain_brush.hpp"

#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

namespace {

// ---------------------------------------------------------------
// 场景参数（V0.1 小场景：3×3 tile）
// ---------------------------------------------------------------

constexpr std::uint64_t kWorldSeed            = 0x5EED1234ULL;
constexpr int           kTileRadius           = 1;  ///< tile 坐标 -1 ~ +1，即 3×3 个 tile
constexpr int           kSceneCenterColumn    = 0;  ///< 出生列（世界列坐标）
constexpr int           kFallbackSpawnHeight  = 64; ///< 出生列无地表数据时的回退高度（格）

constexpr float kLookSensitivity    = 0.0022F;                    ///< 鼠标环绕灵敏度（弧度/像素）
constexpr float kWalkSpeed          = 9.0F;                       ///< 行走速度（格/秒）
constexpr float kSprintSpeed        = 20.0F;                      ///< 冲刺速度（格/秒）
constexpr float kBrushRadius        = 6.0F;                       ///< 笔刷半径（格）
constexpr int   kBrushDeltaUnits    = vx::kHeightUnitsPerBlock;   ///< 每次点击抬升 / 下沉 1 格
constexpr double kRebaseDistance    = 24.0;                       ///< 渲染原点重定基阈值（格）
constexpr float kCameraFollowDistance = 14.0F;                    ///< 第三人称相机跟随距离（格）

// ---------------------------------------------------------------
// 角色物理参数（T7 验收口径：20 格/秒冲刺不穿地形；1 格台阶可自动上步）
// ---------------------------------------------------------------

constexpr float kGravity                    = 24.0F;  ///< 重力加速度（格/秒²）
constexpr float kJumpSpeed                  = 8.0F;   ///< 起跳初速度（格/秒）
constexpr float kCharacterRadius            = 0.3F;   ///< 胶囊半径（格）
constexpr float kCharacterCylinderHalfHeight = 0.6F;  ///< 胶囊圆柱段半高（格）；总高 1.8 格
constexpr float kCharacterMaxSlopeDegrees   = 50.0F;  ///< 可行走最大坡度（度）
constexpr float kCharacterStepUpHeight      = 1.0F;   ///< **自动**上台阶高度（格）
constexpr float kSpawnClearance             = 0.5F;   ///< 出生点离地高度（格）

/// 本帧采样到的移动指令（供本帧所有固定逻辑步复用；模拟量只在帧边界读取一次）。
struct MoveCommand {
    float forward = 0.0F;  ///< +1 前、-1 后
    float strafe  = 0.0F;  ///< +1 右、-1 左
    float speed   = 0.0F;  ///< 格 / 秒
    bool  jump    = false; ///< 本帧是否请求起跳
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

/// 推进一个固定逻辑步：把移动指令按相机 yaw 转到世界方向，交给 Jolt `CharacterVirtual` 求解
/// （重力 / 上坡 / **自动上台阶**都在物理层内完成），再让第三人称相机跟随角色。
void StepCharacter(vx::PhysicsWorld& physics, vx::PhysicsWorld::CharacterHandle character,
                   vx::ThirdPersonCamera& camera, const MoveCommand& command) {
    const vx::PhysicsWorld::CharacterState state = physics.GetCharacterState(character);

    const float     yaw    = camera.Yaw();
    const float     sinYaw = std::sin(yaw);
    const float     cosYaw = std::cos(yaw);
    const glm::vec3 forward(sinYaw, 0.0F, cosYaw);
    const glm::vec3 right(cosYaw, 0.0F, -sinYaw);
    glm::vec3       direction = forward * command.forward + right * command.strafe;

    glm::vec3 velocity = state.velocity;
    velocity.x         = 0.0F;
    velocity.z         = 0.0F;
    const float lengthSq = glm::dot(direction, direction);
    if (lengthSq > 0.0F) {
        direction /= std::sqrt(lengthSq);
        velocity.x = direction.x * command.speed;
        velocity.z = direction.z * command.speed;
    }
    if (command.jump && state.onGround) {
        velocity.y = kJumpSpeed;  // 竖直分量由玩法层给冲量，重力由物理层在步内累加
    }

    physics.SetCharacterVelocity(character, velocity);
    physics.MoveCharacter(character, static_cast<float>(vx::kFixedDt), glm::vec3(0.0F, -kGravity, 0.0F));

    const vx::PhysicsWorld::CharacterState after = physics.GetCharacterState(character);
    camera.Advance(glm::vec3(static_cast<float>(after.position.x), static_cast<float>(after.position.y),
                             static_cast<float>(after.position.z)));
}

/// 对当前注视点执行一次笔刷挖 / 堆，**只重网格、只重传、只重建**受影响的 tile
/// （红线：禁止整世界重网格）。
/// 返回本次被弄脏的 tile 数（供调试面板显示）。
std::size_t ApplyBrush(vx::TerrainWorld& world, vx::TerrainCollision& collision, vx::MeshRenderer& renderer,
                       const std::vector<vx::TileCoord>& tileCoords, std::vector<vx::MeshHandle>& tileHandles,
                       const glm::vec3& focus, int deltaUnits, const glm::dvec3& renderOrigin) {
    vx::BrushPose brush;
    brush.centerX = focus.x;
    brush.centerZ = focus.z;
    brush.radius  = kBrushRadius;

    const vx::BrushResult result = vx::ApplyTerrainBrush(world, brush, deltaUnits);
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

    VX_LOG_INFO("笔刷（半径 %.1f 格）作用于 (%.1f, %.1f)：改动 %zu 列，重网格 %zu 个 tile，重建碰撞体 %zu 个",
                static_cast<double>(kBrushRadius), static_cast<double>(brush.centerX),
                static_cast<double>(brush.centerZ), result.changedColumns, remeshed, rebuiltBodies);
    return result.dirtyTiles.size();
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

        vx::Window window("Voxel Engine - V0.1 terrain", 1280, 720);

        vx::TerrainWorld world(kWorldSeed, materials);

        // 3×3 tile 的小场景：tile 坐标 -1 ~ +1 → 世界列 -64 ~ +128。
        std::vector<vx::TileCoord>  tileCoords;
        std::vector<vx::MeshHandle> tileHandles;
        const std::size_t           tileCount =
            static_cast<std::size_t>(2 * kTileRadius + 1) * static_cast<std::size_t>(2 * kTileRadius + 1);
        tileCoords.reserve(tileCount);
        tileHandles.reserve(tileCount);
        for (int tileZ = -kTileRadius; tileZ <= kTileRadius; ++tileZ) {
            for (int tileX = -kTileRadius; tileX <= kTileRadius; ++tileX) {
                world.LoadTile(tileX, tileZ);  // 生成 + 网格化
                tileCoords.push_back(vx::TileCoord { tileX, tileZ });
                tileHandles.push_back(vx::MeshHandle {});
            }
        }

        // 物理世界 + 地表碰撞体（每个地表 tile 一个 HeightFieldShape）。
        vx::PhysicsWorld    physics;
        vx::TerrainCollision terrainCollision(physics);
        const std::size_t    collisionTiles = terrainCollision.SyncTiles(world, tileCoords);

        vx::MeshRenderer renderer(window.device(), window.handle(), shaderDir);

        // 出生点：场景中心列的地表高度；角色脚底抬离地表一点，随后自然落到地表。
        const float spawnX = static_cast<float>(kSceneCenterColumn);
        const float spawnZ = static_cast<float>(kSceneCenterColumn);
        float       spawnHeight = static_cast<float>(kFallbackSpawnHeight);
        if (!world.QueryHeight(spawnX, spawnZ, spawnHeight)) {
            VX_LOG_WARN("出生列 (%d, %d) 无地表数据，改用回退高度 %d 格", kSceneCenterColumn, kSceneCenterColumn,
                        kFallbackSpawnHeight);
        }

        vx::PhysicsWorld::CapsuleDesc capsule;
        capsule.radius             = kCharacterRadius;
        capsule.cylinderHalfHeight = kCharacterCylinderHalfHeight;
        capsule.maxSlopeAngleDeg   = kCharacterMaxSlopeDegrees;
        capsule.stepUpHeight       = kCharacterStepUpHeight;
        capsule.position = glm::dvec3(static_cast<double>(spawnX),
                                      static_cast<double>(spawnHeight) + static_cast<double>(kSpawnClearance),
                                      static_cast<double>(spawnZ));

        const vx::PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
        if (character == 0) {
            VX_LOG_ERROR("角色胶囊创建失败，无法继续");
            return EXIT_FAILURE;
        }

        vx::CameraSettings settings;
        settings.aspectRatio    = 1280.0F / 720.0F;
        settings.followDistance = kCameraFollowDistance;

        vx::ThirdPersonCamera camera(settings);
        camera.SnapTo(glm::vec3(spawnX, static_cast<float>(capsule.position.y), spawnZ));
        camera.SetYaw(0.7F);
        camera.SetPitch(-0.42F);  // 略微俯视地表

        // 调试面板（T9）：基于 imgui 的 SDL3 + SDL3_gpu 后端；F1 开关。
        vx::DebugOverlay debugOverlay(window.device(), window.handle());

        // 渲染原点：整数世界定位，上传的 float 顶点都以它为基准（红线 6）。
        glm::dvec3 renderOrigin(std::floor(static_cast<double>(spawnX)), std::floor(static_cast<double>(spawnHeight)),
                                std::floor(static_cast<double>(spawnZ)));
        for (std::size_t i = 0; i < tileCoords.size(); ++i) {
            UploadTileMesh(renderer, tileHandles[i], world, tileCoords[i], renderOrigin);
        }

        vx::InputMap input;
        input.BindKey(vx::ActionId::MoveForward, SDL_SCANCODE_W);
        input.BindKey(vx::ActionId::MoveBackward, SDL_SCANCODE_S);
        input.BindKey(vx::ActionId::MoveLeft, SDL_SCANCODE_A);
        input.BindKey(vx::ActionId::MoveRight, SDL_SCANCODE_D);
        input.BindKey(vx::ActionId::Jump, SDL_SCANCODE_SPACE);
        input.BindKey(vx::ActionId::Sprint, SDL_SCANCODE_LSHIFT);
        input.BindKey(vx::ActionId::ToggleDebugPanel, SDL_SCANCODE_F1);
        input.BindMouseButton(vx::ActionId::Attack, SDL_BUTTON_LEFT);   // 主笔刷：挖
        input.BindMouseButton(vx::ActionId::Use, SDL_BUTTON_RIGHT);     // 副笔刷：堆
        input.BindMouseAxis(vx::ActionId::LookX, vx::MouseAxis::X);
        input.BindMouseAxis(vx::ActionId::LookY, vx::MouseAxis::Y);

        vx::Clock                clock;
        vx::FixedStepAccumulator accumulator(vx::kFixedDt);
        const SDL_FColor         clearColor { 0.45F, 0.62F, 0.85F, 1.0F };

        VX_LOG_INFO("地表世界就绪：种子 %llu，tile %zu 个，材质表 schema_version=%d，笔刷半径 %.1f 格",
                    static_cast<unsigned long long>(kWorldSeed), tileCoords.size(), materials.SchemaVersion(),
                    static_cast<double>(kBrushRadius));
        VX_LOG_INFO("角色物理就绪：地表碰撞体 %zu 个 tile；胶囊 半径 %.2f / 总高 %.2f 格；"
                    "重力 %.1f、跳跃 %.1f、最大坡度 %.0f°、自动上台阶 %.1f 格（dt=1/60）",
                    collisionTiles, static_cast<double>(kCharacterRadius),
                    static_cast<double>(2.0F * (kCharacterCylinderHalfHeight + kCharacterRadius)),
                    static_cast<double>(kGravity), static_cast<double>(kJumpSpeed),
                    static_cast<double>(kCharacterMaxSlopeDegrees), static_cast<double>(kCharacterStepUpHeight));
        VX_LOG_INFO("ImGui 调试面板已启用：F1 开关（当前%s）", debugOverlay.Visible() ? "显示" : "隐藏");

        std::size_t lastDirtyTiles = 0;

        while (window.pump_events(input)) {
            input.BeginFrame();  // 每帧采样一次，且只在固定步循环之外

            // 相机环绕：模拟量每帧消费一次，同一帧的多个逻辑步不会重复消费（见 InputMap 契约 3）。
            camera.AddYaw(-input.ConsumeValue(vx::ActionId::LookX) * kLookSensitivity);
            camera.AddPitch(-input.ConsumeValue(vx::ActionId::LookY) * kLookSensitivity);

            // 调试面板开关：本帧按下边沿消费一次。
            if (input.ConsumePressed(vx::ActionId::ToggleDebugPanel)) {
                debugOverlay.Toggle();
                VX_LOG_INFO("调试面板：%s", debugOverlay.Visible() ? "显示" : "隐藏");
            }

            // 笔刷：本帧按下边沿消费一次。
            const bool digRequested  = input.ConsumePressed(vx::ActionId::Attack);
            const bool pileRequested = input.ConsumePressed(vx::ActionId::Use);

            // 移动输入本帧只读一次，供本帧全部固定逻辑步复用。
            MoveCommand command;
            command.forward = (input.Held(vx::ActionId::MoveForward) ? 1.0F : 0.0F) -
                              (input.Held(vx::ActionId::MoveBackward) ? 1.0F : 0.0F);
            command.strafe = (input.Held(vx::ActionId::MoveRight) ? 1.0F : 0.0F) -
                             (input.Held(vx::ActionId::MoveLeft) ? 1.0F : 0.0F);
            command.speed = input.Held(vx::ActionId::Sprint) ? kSprintSpeed : kWalkSpeed;
            command.jump  = input.ConsumePressed(vx::ActionId::Jump);

            const vx::StepPlan plan = accumulator.Advance(clock.Tick());
            for (int step = 0; step < plan.steps; ++step) {
                StepCharacter(physics, character, camera, command);
            }

            if (digRequested || pileRequested) {
                lastDirtyTiles = ApplyBrush(world, terrainCollision, renderer, tileCoords, tileHandles,
                                            camera.TargetCurrent(), digRequested ? -kBrushDeltaUnits : kBrushDeltaUnits,
                                            renderOrigin);
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

            // 渲染：alpha 只用于在上一 / 当前逻辑状态之间插值，绝不回写模拟状态（红线 11）。
            const vx::CameraView view = camera.Evaluate(plan.alpha, &world);

            // 调试面板：统计经独立接口采集，只在渲染线程构建，不进世界层热路径。
            vx::DebugStats stats;
            const vx::PhysicsWorld::CharacterState characterState = physics.GetCharacterState(character);
            stats.frameSeconds      = clock.DeltaSeconds();
            stats.stepsThisFrame     = plan.steps;
            stats.characterPosition = characterState.position;
            stats.characterOnGround = characterState.onGround;
            stats.cameraYaw         = camera.Yaw();
            stats.cameraPitch       = camera.Pitch();
            stats.cameraDistance    = view.distance;
            stats.brushRadius       = kBrushRadius;
            stats.loadedTileCount   = tileCoords.size();
            stats.lastDirtyTileCount = lastDirtyTiles;
            stats.tileBodyCount     = terrainCollision.TileBodyCount();
            stats.physicsReady      = true;
            debugOverlay.BeginFrame();
            debugOverlay.BuildUI(stats);
            debugOverlay.EndFrame();

            renderer.SetCamera(RelativeCameraView(view, renderOrigin));
            if (!renderer.RenderFrame(tileHandles.data(), tileHandles.size(), clearColor, &debugOverlay)) {
                VX_LOG_DEBUG("本帧未取得交换链纹理（窗口最小化？），跳过渲染");
            }
        }

        VX_LOG_INFO("收到退出请求，主循环结束（累计 %.1f s）", clock.ElapsedSeconds());
    } catch (const std::exception& error) {
        VX_LOG_ERROR("启动或主循环失败：%s", error.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
