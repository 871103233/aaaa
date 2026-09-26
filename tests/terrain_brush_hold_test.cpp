// 缺陷 B2 回归测试：把笔刷当作"按住不放"长时间连续驱动（60 Hz × 若干秒），
// 并在世界垂直范围两端反复堆 / 挖，锁定三条几何不变量：
//   1) 所有列高度始终落在文档范围 [0, 512 格]（ADR 0008）；
//   2) 所有网格顶点浮点全部有限（无 NaN / Inf）；
//   3) 所有索引都在顶点范围内。
//
// 关于 B2 的定位结论（见本轮报告 / devlog）：蓝屏**不是**几何失效（高度已钳制、int16 不会溢出），
// 也**不是**每帧重网格风暴（笔刷是"本帧按下"边沿触发，长按只应用一次）；
// 真正的根因是相机在"地表被抬到注视点之上"时退化出 NaN 视图矩阵、整帧几何被丢弃。
// 相机侧的回归见 tests/render_camera_test.cpp 的
// ThirdPersonCamera.DegenerateViewIsAvoidedWhenFollowDistanceCollapses。

#include "dig/terrain_brush.hpp"
#include "generation/map_preset.hpp"
#include "physics/physics_world.hpp"
#include "render/camera.hpp"
#include "terrain/material_table.hpp"
#include "terrain/terrain_collision.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace {

using vx::ApplyTerrainBrush;
using vx::BrushPose;
using vx::BrushResult;
using vx::CameraSettings;
using vx::CameraView;
using vx::Height;
using vx::HeightToBlocks;
using vx::kHeightUnitsPerBlock;
using vx::kMaxTerrainHeightUnits;
using vx::kMinTerrainHeightUnits;
using vx::kTerrainTileVertexCount;
using vx::MapPreset;
using vx::MeshVertex;
using vx::PhysicsWorld;
using vx::TerrainCollision;
using vx::TerrainMaterialTable;
using vx::TerrainTile;
using vx::TerrainTileMesh;
using vx::TerrainWorld;
using vx::ThirdPersonCamera;
using vx::TileCoord;

constexpr std::uint64_t kSeed = 0x5EED1234ULL;

[[nodiscard]] std::vector<TileCoord> TileList(int radius) {
    std::vector<TileCoord> tiles;
    for (int tileZ = -radius; tileZ <= radius; ++tileZ) {
        for (int tileX = -radius; tileX <= radius; ++tileX) {
            tiles.push_back(TileCoord { tileX, tileZ });
        }
    }
    return tiles;
}

void LoadTiles(TerrainWorld& world, int radius) {
    for (int tileZ = -radius; tileZ <= radius; ++tileZ) {
        for (int tileX = -radius; tileX <= radius; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }
}

/// 断言三条几何不变量：高度在文档范围、顶点浮点全部有限、索引在范围内。
void ExpectGeometryValid(const TerrainWorld& world, const std::vector<TileCoord>& tiles, const char* context) {
    for (const TileCoord& coord : tiles) {
        const TerrainTile* tile = world.FindTile(coord.x, coord.z);
        ASSERT_NE(tile, nullptr) << context;
        for (int j = 0; j < kTerrainTileVertexCount; ++j) {
            for (int i = 0; i < kTerrainTileVertexCount; ++i) {
                const Height height = tile->At(i, j);
                EXPECT_GE(static_cast<int>(height), kMinTerrainHeightUnits) << context;
                EXPECT_LE(static_cast<int>(height), kMaxTerrainHeightUnits) << context;
                EXPECT_TRUE(std::isfinite(HeightToBlocks(height))) << context;
            }
        }

        const TerrainTileMesh* mesh = world.FindMesh(coord.x, coord.z);
        ASSERT_NE(mesh, nullptr) << context;
        ASSERT_FALSE(mesh->mesh.vertices.empty()) << context;
        ASSERT_FALSE(mesh->mesh.indices.empty()) << context;
        for (const MeshVertex& vertex : mesh->mesh.vertices) {
            for (const float value : vertex.position) {
                EXPECT_TRUE(std::isfinite(value)) << context;
            }
            for (const float value : vertex.normal) {
                EXPECT_TRUE(std::isfinite(value)) << context;
            }
        }
        for (const std::uint32_t index : mesh->mesh.indices) {
            EXPECT_LT(index, mesh->mesh.vertices.size()) << context;
        }
    }
}

}  // namespace

// "按住右键"连续抬升 5 秒 @60 Hz：多个笔刷位置（含跨 tile 共享边界）反复施加，几何始终合法。
TEST(TerrainBrushHold, HeldRaiseKeepsGeometryWithinLimits) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    const std::vector<TileCoord> tiles = TileList(1);
    LoadTiles(world, 1);

    // 半径 6 的笔刷会同时触及多个 tile；位置刻意包含共享边界与角落。
    const BrushPose kPositions[] = {
        BrushPose { 0.0F, 0.0F, 6.0F },
        BrushPose { 32.0F, 32.0F, 6.0F },
        BrushPose { -32.0F, 32.0F, 6.0F },
        BrushPose { 0.0F, 63.0F, 6.0F },
    };
    constexpr int kHoldFrames = 60 * 5;  // 5 秒 @60 Hz

    for (int frame = 0; frame < kHoldFrames; ++frame) {
        const BrushResult result = ApplyTerrainBrush(world, kPositions[frame % 4], kHeightUnitsPerBlock);
        (void)world.RemeshDirtyTiles(result.dirtyTiles);
    }

    ExpectGeometryValid(world, tiles, "长按抬升 5 秒后");
}

// "按住右键"一直抬到垂直上限并继续按住：高度精确停在 512 格（含共享边界列），不越界、不溢出。
TEST(TerrainBrushHold, HeldRaiseClampsAtVerticalLimit) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    const std::vector<TileCoord> tiles = TileList(1);
    LoadTiles(world, 1);

    const BrushPose kCenter { 0.0F, 0.0F, 6.0F };

    // 先一步顶到上限（覆盖 int16 溢出最危险的路径），再长按 2 秒持续施加。
    (void)ApplyTerrainBrush(world, kCenter, kMaxTerrainHeightUnits);
    for (int frame = 0; frame < 60 * 2; ++frame) {
        const BrushResult result = ApplyTerrainBrush(world, kCenter, kHeightUnitsPerBlock);
        (void)world.RemeshDirtyTiles(result.dirtyTiles);
    }

    Height center = 0;
    ASSERT_TRUE(world.ReadColumnHeight(0, 0, center));
    EXPECT_EQ(static_cast<int>(center), kMaxTerrainHeightUnits);

    // 世界列 0 被 tile(-1,0)（本地 i = 64，共享边界层）与 tile(0,0)（本地 i = 0）共同持有：
    // 两侧的定点高度必须逐位一致地停在上限，边界不得错开。
    const TerrainTile* leftTile  = world.FindTile(-1, 0);
    const TerrainTile* rightTile = world.FindTile(0, 0);
    ASSERT_NE(leftTile, nullptr);
    ASSERT_NE(rightTile, nullptr);
    EXPECT_EQ(static_cast<int>(leftTile->At(kTerrainTileVertexCount - 1, 0)), kMaxTerrainHeightUnits);
    EXPECT_EQ(static_cast<int>(rightTile->At(0, 0)), kMaxTerrainHeightUnits);

    ExpectGeometryValid(world, tiles, "在垂直上限继续长按抬升后");
}

// "按住左键"一直挖到垂直下限并继续按住：高度精确停在 0 格，几何始终合法。
TEST(TerrainBrushHold, HeldDigClampsAtVerticalLimit) {
    TerrainWorld world(kSeed, TerrainMaterialTable::Default());
    const std::vector<TileCoord> tiles = TileList(1);
    LoadTiles(world, 1);

    const BrushPose kCenter { 32.0F, 32.0F, 6.0F };

    (void)ApplyTerrainBrush(world, kCenter, -kMaxTerrainHeightUnits);
    for (int frame = 0; frame < 60 * 2; ++frame) {
        const BrushResult result = ApplyTerrainBrush(world, kCenter, -kHeightUnitsPerBlock);
        (void)world.RemeshDirtyTiles(result.dirtyTiles);
    }

    Height center = 0;
    ASSERT_TRUE(world.ReadColumnHeight(32, 32, center));
    EXPECT_EQ(static_cast<int>(center), kMinTerrainHeightUnits);

    ExpectGeometryValid(world, tiles, "在垂直下限继续长按下挖后");
}

// 缺陷 B2 的**物理侧**回归：在角色脚下反复堆地形时，角色必须被顶到新地表（而不是被埋住后穿过高度场），
// 且相机视图矩阵始终有限。复刻 game/main.cpp 的"堆完后把低于地表的角色放回地表"。
TEST(TerrainBrushHold, PileUnderPlayerLiftsCharacterInsteadOfSinkingThrough) {
    const MapPreset preset = MapPreset::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) /
                                                     "assets/maps/test_range.toml");
    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);

    std::vector<TileCoord> coords;
    for (int tileZ = -preset.tileRadiusZ; tileZ <= preset.tileRadiusZ; ++tileZ) {
        for (int tileX = -preset.tileRadiusX; tileX <= preset.tileRadiusX; ++tileX) {
            world.LoadTile(tileX, tileZ);
            coords.push_back(TileCoord { tileX, tileZ });
        }
    }

    PhysicsWorld     physics;
    TerrainCollision collision(physics);
    (void)collision.SyncTiles(world, coords);

    float surface = 0.0F;
    ASSERT_TRUE(world.QueryHeight(static_cast<float>(preset.spawnX), static_cast<float>(preset.spawnZ), surface));

    PhysicsWorld::CapsuleDesc capsule;
    capsule.position = glm::dvec3(preset.spawnX, static_cast<double>(surface) + 0.5, preset.spawnZ);
    const PhysicsWorld::CharacterHandle character = physics.CreateCharacter(capsule);
    ASSERT_NE(character, 0u);

    constexpr float kGravity = 24.0F;
    constexpr float kDt      = 1.0F / 60.0F;
    const glm::vec3 gravity(0.0F, -kGravity, 0.0F);
    for (int i = 0; i < 120; ++i) {
        physics.MoveCharacter(character, kDt, gravity);
    }

    CameraSettings settings;
    settings.followDistance = 14.0F;  // 与 game/main.cpp 一致
    ThirdPersonCamera camera(settings);
    const PhysicsWorld::CharacterState settled = physics.GetCharacterState(character);
    camera.SnapTo(glm::vec3(static_cast<float>(settled.position.x), static_cast<float>(settled.position.y),
                            static_cast<float>(settled.position.z)));
    camera.SetYaw(0.7F);
    camera.SetPitch(-0.42F);

    for (int click = 0; click < 6; ++click) {  // 模拟连续多次"按住右键"
        PhysicsWorld::CharacterState state = physics.GetCharacterState(character);
        const BrushPose brush { static_cast<float>(state.position.x), static_cast<float>(state.position.z), 6.0F };
        const BrushResult result = ApplyTerrainBrush(world, brush, kHeightUnitsPerBlock);
        (void)world.RemeshDirtyTiles(result.dirtyTiles);
        (void)collision.SyncTiles(world, result.dirtyTiles);

        // 复刻 game/main.cpp：把被新地表埋住的角色放回地表。
        state = physics.GetCharacterState(character);
        float newSurface = 0.0F;
        if (world.QueryHeight(static_cast<float>(state.position.x), static_cast<float>(state.position.z), newSurface) &&
            state.position.y < static_cast<double>(newSurface)) {
            physics.SetCharacterPosition(character, glm::dvec3(state.position.x, static_cast<double>(newSurface),
                                                               state.position.z));
        }

        for (int step = 0; step < 30; ++step) {
            physics.MoveCharacter(character, kDt, gravity);
        }
        state = physics.GetCharacterState(character);
        camera.Advance(glm::vec3(static_cast<float>(state.position.x), static_cast<float>(state.position.y),
                                 static_cast<float>(state.position.z)));

        // 不变量①：角色脚底不得低于其所在列的地表（未被埋住 / 未穿下去）。
        float ground = 0.0F;
        ASSERT_TRUE(world.QueryHeight(static_cast<float>(state.position.x), static_cast<float>(state.position.z), ground));
        EXPECT_GE(state.position.y, static_cast<double>(ground) - 0.2) << "第 " << click << " 次堆土后角色被埋 / 穿到地表以下";

        // 不变量②：相机视图矩阵必须有限（否则整帧几何失效、只剩清屏色）。
        const CameraView view = camera.Evaluate(0.0, &world);
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_TRUE(std::isfinite(view.viewProjection[column][row])) << "第 " << click << " 次堆土后相机视图失效";
            }
        }
    }
}
