// 破坏后的倒塌（T33 / ADR 0015：**统一连通分量刚体化**）单元测试。
//
// 判据（阶段计划 T33）：
//   ① **稳定面不塌**：由高度场初始化的平坦地表上，不产生任何倒塌整体、不改数据；
//   ② **失去支撑 ⇒ 一个整体**：悬空石板成为**一个**连通分量（`units.size() == 1`、体素数 = 石板体素数），
//      并从体积中**抽出**（原处变空）；
//   ③ **体素化回写**：按落定位姿写回 ⇒ 体素落回地形（守恒：`writtenVoxels == 体素数`、`droppedVoxels == 0`）；
//   ④ **会旋转**：90° 落定的姿态写回后，形状与"原地不动"显著不同（这正是"逐列下落"做不到的）；
//   ⑤ **确定性**：同输入两次运行得到同一分组；
//   ⑥ 参数非法即抛异常（配置表校验）。

#include "core/clock.hpp"
#include "dig/collapse_table.hpp"
#include "dig/dig_volume.hpp"
#include "dig/volume_collapse.hpp"
#include "dig/volume_mesher.hpp"

#include "terrain/material_table.hpp"
#include "terrain/terrain_types.hpp"
#include "terrain/terrain_world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <gtest/gtest.h>

namespace {

using vx::BlockCoord;
using vx::CollapsePlan;
using vx::CollapsePose;
using vx::CollapseSpec;
using vx::CollapseTable;
using vx::DensityRegion;
using vx::DigRegion;
using vx::DigRegionTable;
using vx::DigVolumeWorld;
using vx::MapEdit;
using vx::MapEditMode;
using vx::MapPreset;
using vx::TerrainMaterialTable;
using vx::TerrainWorld;

constexpr int kFlatHeightBlocks = 120;
/// 地表所在的块（块 y = 3 ⇒ 覆盖世界高度 [96, 128)，正好含 120 格平地）。
constexpr BlockCoord kSurfaceBlock { 0, 3, 0 };
/// "悬空石板"场景的块范围：**必须含更低的块 2**，否则"地板"接不到区域底面（= 判据里的地面）。
constexpr BlockCoord kSceneBlockMin { 0, 2, 0 };
constexpr BlockCoord kSceneBlockMax { 0, 3, 0 };
/// 场景覆盖的世界高度起点（= 块 2 的原点）：石板场景的采样范围是 [64, 128]。
constexpr int kSceneOriginY = 64;
/// 悬空石板的体素数（8 × 8 × 2 个世界列）。
constexpr std::size_t kSlabVoxels = 8 * 8 * 2;
/// 单次规划最多抽出几个整体（测试用；= 渲染网格池槽位数）。
constexpr std::size_t kMaxUnitsInTest = 4;

[[nodiscard]] DigRegion MakeRegion(BlockCoord minimum, BlockCoord maximum) {
    DigRegion region;
    region.name     = "collapse_test";
    region.diggable = true;
    region.priority = 0;
    region.blockMin = minimum;
    region.blockMax = maximum;
    return region;
}

[[nodiscard]] MapPreset FlatPreset() {
    MapPreset preset;
    preset.name = "volume collapse test（整图压平）";
    preset.seed = 20260927;

    MapEdit flatten;
    flatten.name        = "flat";
    flatten.mode        = MapEditMode::Flatten;
    flatten.minX        = -64;
    flatten.maxX        = 64;
    flatten.minZ        = -64;
    flatten.maxZ        = 64;
    flatten.heightUnits = kFlatHeightBlocks * vx::kHeightUnitsPerBlock;
    preset.edits.push_back(flatten);
    return preset;
}

/// "地板 + 空腔 + 悬空石板"的合成密度场（世界高度 [64, 128]，即两个块的采样范围）：
///   地板 = 高度 < 105 全实心（**自区域底面 64 起连续实心 ⇒ 接地**）；空腔 = 105..108；
///   石板 = 109..110、世界列 x/z ∈ [4, 12)；其上空。
[[nodiscard]] DensityRegion BuildFloatingSlabScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor = worldY < 105;
                const bool slab  = (worldY >= 109) && (worldY < 111) && (worldX >= 4) && (worldX < 12) &&
                                  (worldZ >= 4) && (worldZ < 12);
                region.values[region.Index(i, j, k)] =
                    (floor || slab) ? static_cast<std::int8_t>(vx::kDensityMin)
                                    : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

/// **断口闭合**用例的场景（T46 修订）：一块**仍连在未塌岩体上**的悬挑板。
///   地板 = 高度 < 105 全实心（区域底面起连续实心 ⇒ 接地）；
///   立柱 = x∈[4,6)、z∈[4,6)、y∈[105,110)（接地 ⇒ 有支撑）；
///   悬挑板 = x∈[4,12)、z∈[4,6)、y∈[110,112)。
/// 取 `maxCantileverBlocks = 1`：板在 x ≤ 6 处仍算有支撑，x ≥ 7 失去支撑（成为整体），
/// 而它**仍与 x = 6 的实心相邻** ⇒ 断口处必须也生成等值面（否则掉落中能从断口看穿它）。
[[nodiscard]] DensityRegion BuildAttachedShelfScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor  = worldY < 105;
                const bool pillar = (worldY >= 105) && (worldY < 110) && (worldX >= 4) && (worldX < 6) &&
                                    (worldZ >= 4) && (worldZ < 6);
                const bool shelf = (worldY >= 110) && (worldY < 112) && (worldX >= 4) && (worldX < 12) &&
                                   (worldZ >= 4) && (worldZ < 6);
                region.values[region.Index(i, j, k)] =
                    (floor || pillar || shelf) ? static_cast<std::int8_t>(vx::kDensityMin)
                                               : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

/// **长条**场景（T47 / BUG3 取证）：与项目所有者实测的"掉落中的长条"同形 —— 细而长的一块**悬空**体。
///   地板 = 高度 < 105 全实心（区域底面起连续实心 ⇒ 接地）；
///   长条 = x∈[1,31)、z∈[10,12)、y∈[109,111)（30 × 2 × 2 = 120 体素，长径比 15:1），四周为空腔。
[[nodiscard]] DensityRegion BuildLongBarScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor = worldY < 105;
                const bool bar   = (worldY >= 109) && (worldY < 111) && (worldX >= 1) && (worldX < 31) &&
                                 (worldZ >= 10) && (worldZ < 12);
                region.values[region.Index(i, j, k)] =
                    (floor || bar) ? static_cast<std::int8_t>(vx::kDensityMin)
                                   : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

/// 闭合判据（T46 修订的"每条无向边恰被 2 个三角形共用"）现由**生产代码里的同一份实现**承担：
/// `vx::CountBoundaryEdges`（`world/dig/volume_mesher.*`）。测试与"整体外观网格插桩"（T47）共用一份口径，
/// 不再各写一套。

/// **T49 判据①** 的场景：30 格长条**只由两端立柱撑着**（两端接地 ⇒ 原样稳定）。
///   地板 = 高度 < 105 全实心（自区域底面连续实心 ⇒ 接地）；
///   两根立柱 = x∈[1,3) 与 x∈[29,31)、z∈[10,12)、y∈[105,110)（接地 ⇒ 有支撑）；
///   长条 = x∈[1,31)、z∈[10,12)、y∈[110,112)（30 × 2 × 2 = 120 体素）。
/// 两次爆破各炸一侧立柱的**根部** ⇒ 长条**整段**失去支撑 ⇒ 区域内不得再有"下方为空"的实心体素。
[[nodiscard]] DensityRegion BuildTwoPillarBarScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor  = worldY < 105;
                const bool pillar = (worldY >= 105) && (worldY < 110) && (worldZ >= 10) && (worldZ < 12) &&
                                    ((worldX >= 1 && worldX < 3) || (worldX >= 29 && worldX < 31));
                const bool bar = (worldY >= 110) && (worldY < 112) && (worldX >= 1) && (worldX < 31) &&
                                 (worldZ >= 10) && (worldZ < 12);
                region.values[region.Index(i, j, k)] =
                    (floor || pillar || bar) ? static_cast<std::int8_t>(vx::kDensityMin)
                                             : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

/// **T49 判据③**（超上限 ⇒ 回退固定窗口）的场景：**全长**横跨 `blockCountX` 个块的悬空长条。
/// 它远长于任何固定窗口 ⇒ 爆破其一端后连通域一直延伸到另一端，窗口反复翻倍直到撞上
/// `kMaxRegionSamples` ⇒ 走"告警 + 保守回退固定窗口"这条显式例外（ADR 0018 决策一）。
///
/// 竖直只占**一个块**（y 块 3 = [96, 128)）⇒ 窗口的竖直采样数从 65 降到 33，
/// 让"翻倍到超上限"这一步在测试里仍然便宜（否则光是读窗口就要几十 MB）。
[[nodiscard]] DensityRegion BuildLongSprawlSlabScene(int blockCountX) {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = 3 * vx::kVolumeBlockSize;  // 96
    region.minZ  = 0;
    region.sizeX = blockCountX * vx::kVolumeBlockSize + 1;
    region.sizeY = vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int  worldY = region.minY + j;
                const int  worldZ = region.minZ + k;
                const bool floor  = worldY < 105;  // 区域底面 (96) 起连续实心 ⇒ 接地
                const bool slab   = (worldY >= 109) && (worldY < 111) && (worldZ >= 10) && (worldZ < 12) &&
                                  (region.minX + i < region.minX + region.sizeX - 1);
                region.values[region.Index(i, j, k)] =
                    (floor || slab) ? static_cast<std::int8_t>(vx::kDensityMin)
                                    : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

/// **T51** 判据的场景：**深井**（地板只到 `worldY < 70`）+ 高空悬空石板（y ∈ [109, 111)、x/z ∈ [4, 12)）。
///
/// 用途：石板落在**下方 32 格内没有任何支撑**的地带 —— 接地沉降走满上限也接不到地
/// ⇒ 必须走"仍悬空 ⇒ 清除"这条路（否则就会静默留下悬空的泥土，正是所有者实测到的现象）。
[[nodiscard]] DensityRegion BuildDeepPitScene() {
    DensityRegion region;
    region.minX  = 0;
    region.minY  = kSceneOriginY;
    region.minZ  = 0;
    region.sizeX = vx::kVolumeBlockSize + 1;
    region.sizeY = 2 * vx::kVolumeBlockSize + 1;
    region.sizeZ = vx::kVolumeBlockSize + 1;
    region.values.assign(static_cast<std::size_t>(region.sizeX) * static_cast<std::size_t>(region.sizeY) *
                             static_cast<std::size_t>(region.sizeZ),
                         static_cast<std::int8_t>(vx::kDensityMax));

    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 0; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                const int worldX = region.minX + i;
                const int worldY = region.minY + j;
                const int worldZ = region.minZ + k;

                const bool floor = worldY < 70;  // 深井：地表比石板低 40 格
                const bool slab  = (worldY >= 109) && (worldY < 111) && (worldX >= 4) && (worldX < 12) &&
                                  (worldZ >= 4) && (worldZ < 12);
                region.values[region.Index(i, j, k)] =
                    (floor || slab) ? static_cast<std::int8_t>(vx::kDensityMin)
                                    : static_cast<std::int8_t>(vx::kDensityMax);
            }
        }
    }
    return region;
}

[[nodiscard]] std::size_t CountSolidAtHeight(const DensityRegion& region, int worldY) {
    const int localY = worldY - region.minY;
    if (localY < 0 || localY >= region.sizeY) {
        return 0;
    }
    std::size_t count = 0;
    for (int k = 0; k < region.sizeZ; ++k) {
        for (int i = 0; i < region.sizeX; ++i) {
            if (region.values[region.Index(i, localY, k)] < 0) {
                ++count;
            }
        }
    }
    return count;
}

/// 统计区域内"**下方为空**"的实心体素（T46 / ADR 0017：散体"不能悬空"的可判定判据）。
/// 区域底面（`localY == 0`）不计：它没有下方采样，与"支撑检查把区域底面视作地面"同口径。
[[nodiscard]] std::size_t CountFloatingSolid(const DensityRegion& region) {
    std::size_t count = 0;
    for (int k = 0; k < region.sizeZ; ++k) {
        for (int j = 1; j < region.sizeY; ++j) {
            for (int i = 0; i < region.sizeX; ++i) {
                if (region.values[region.Index(i, j, k)] < 0 && region.values[region.Index(i, j - 1, k)] >= 0) {
                    ++count;
                }
            }
        }
    }
    return count;
}

/// 造一个"平地 + 悬空石板"的世界，返回其中的体积世界（调用方持有 `world` / `regions` 的生命周期）。
struct SlabScene {
    TerrainWorld       world;
    DigRegionTable     regions;
    DigVolumeWorld     volumes;

    /// `materials`（T43）：缺省 = 内置表；需要"不可破坏"等**自定义物理参数**时传入临时加载的表。
    /// `scene`（T46 修订）：缺省 = "地板 + 空腔 + 悬空石板"；断口闭合用例传入"悬挑板"场景。
    explicit SlabScene(const MapPreset& preset, DigRegionTable regionTable, bool writeScene,
                       const TerrainMaterialTable& materials = TerrainMaterialTable::Default(),
                       const DensityRegion& scene = BuildFloatingSlabScene())
        : world(preset.seed, materials), regions(std::move(regionTable)), volumes(world, regions) {
        world.SetMapPreset(preset);
        world.LoadTile(0, 0);
        volumes.InitFromHeightField();
        if (writeScene) {
            volumes.WriteDensityRegion(scene);
        }
    }
};

[[nodiscard]] DensityRegion ReadScene(const DigVolumeWorld& volumes) {
    const DensityRegion scene = BuildFloatingSlabScene();
    return volumes.ReadDensityRegion(scene.minX, scene.minY, scene.minZ, scene.sizeX, scene.sizeY, scene.sizeZ);
}

/// 把悬空石板的**全部体素**显式打上材质槽位（T43）：这样 `SampleMaterialSlot` 走"已写入优先"路径，
/// 质量 / 摩擦 / 弹性 / 清除守卫的判据不受"该列地表派生材质"影响（可确定断言）。
void MarkSlabMaterial(DigVolumeWorld& volumes, std::uint8_t slot) {
    for (int x = 4; x < 12; ++x) {
        for (int z = 4; z < 12; ++z) {
            for (int y = 109; y < 111; ++y) {
                volumes.SetMaterialSlot(x, y, z, slot);
            }
        }
    }
}

/// 按谓词给石板的每个体素分配材质槽位（T43：混合材质 / 多数票用）。
template <typename Fn>
void MarkSlabMaterialBy(DigVolumeWorld& volumes, Fn slotOf) {
    for (int x = 4; x < 12; ++x) {
        for (int z = 4; z < 12; ++z) {
            for (int y = 109; y < 111; ++y) {
                volumes.SetMaterialSlot(x, y, z, slotOf(x, y, z));
            }
        }
    }
}

/// 写一份**临时材质表**并加载（T43）：四层与内置表逐值一致，仅把槽位 2「岩」的 `indestructible`
/// 变成可切换 —— 仓库内提交的 `materials.toml` 里没有不可破坏的层，而清除守卫必须被验证。
[[nodiscard]] TerrainMaterialTable LoadMaterialsWithIndestructibleRock(bool rockIndestructible) {
    // 每行 = 一个槽位：name / texture_layer / 高度带(3) / 坡度带(3) / uv_scale / tint(3) / roughness / ao /
    //             macro_uv_scale / macro_strength / density / friction / restitution
    struct Row {
        const char* name;
        int         textureLayer;
        double      heightMin, heightMax, heightBlend;
        double      slopeMin, slopeMax, slopeBlend;
        double      uvScale, tintR, tintG, tintB, roughness, ao, macroUvScale, macroStrength;
        double      density, friction, restitution;
    };
    const Row rows[4] = {
        { "grass", 1, 0.0, 320.0, 60.0, 0.0, 0.45, 0.10, 0.12, 0.31, 0.55, 0.24, 0.90, 0.85, 0.020, 0.35, 1.3, 0.75, 0.02 },
        { "dirt", 2, 300.0, 512.0, 60.0, 0.0, 0.55, 0.10, 0.10, 0.45, 0.33, 0.21, 0.88, 0.75, 0.015, 0.40, 1.5, 0.60, 0.02 },
        { "rock", 3, 0.0, 512.0, 0.0, 0.55, 1.0, 0.10, 0.16, 0.55, 0.55, 0.56, 0.40, 0.70, 0.030, 0.30, 2.6, 0.70, 0.12 },
        { "sand", 4, 0.0, 6.0, 3.0, 0.0, 0.30, 0.10, 0.18, 0.83, 0.74, 0.48, 0.95, 0.90, 0.025, 0.25, 1.6, 0.50, 0.05 },
    };

    std::ostringstream out;
    out << "schema_version = 5\n";
    for (const Row& row : rows) {
        out << "[[layer]]\n"
            << "name = \"" << row.name << "\"\n"
            << "texture_layer = " << row.textureLayer << "\n"
            << "height_min = " << row.heightMin << "\n"
            << "height_max = " << row.heightMax << "\n"
            << "height_blend = " << row.heightBlend << "\n"
            << "slope_min = " << row.slopeMin << "\n"
            << "slope_max = " << row.slopeMax << "\n"
            << "slope_blend = " << row.slopeBlend << "\n"
            << "uv_scale = " << row.uvScale << "\n"
            << "tint_r = " << row.tintR << "\n"
            << "tint_g = " << row.tintG << "\n"
            << "tint_b = " << row.tintB << "\n"
            << "roughness = " << row.roughness << "\n"
            << "ao = " << row.ao << "\n"
            << "macro_uv_scale = " << row.macroUvScale << "\n"
            << "macro_strength = " << row.macroStrength << "\n"
            << "density = " << row.density << "\n"
            << "friction = " << row.friction << "\n"
            << "restitution = " << row.restitution << "\n";
        // T31：材质表 v5 起 `toughness` 为**必填**（本用例只钉倒塌 / 清除逻辑，与破坏预算无关 ⇒ 统一给 1）。
        out << "toughness = 1.0\n";
        if (rockIndestructible && std::string(row.name) == "rock") {
            out << "indestructible = true\n";
        }
    }
    out << "[triplanar]\n"
        << "enabled = true\n"
        << "slope_min = 0.45\n"
        << "slope_max = 0.70\n"
        << "sharpness = 4.0\n";

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        (rockIndestructible ? "vx_collapse_materials_indestructible.toml" : "vx_collapse_materials_plain.toml");
    {
        std::ofstream outFile(path, std::ios::trunc);
        outFile << out.str();
    }
    return TerrainMaterialTable::LoadFromFile(path);
}

/// 带爆心的种子：`bounds` 与"块对齐种子"一致，另把爆心 / 半径填进去（T43 的冲量来源）。
[[nodiscard]] vx::CollapseSeed SeedWithEpicenter(const glm::dvec3& epicenter, double radius) {
    vx::CollapseSeed seed = vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax);
    seed.epicenter        = epicenter;
    seed.radius           = radius;
    return seed;
}

}  // namespace

// ① 稳定面不塌：平坦地表（由高度场初始化）上跑规划，不应产生任何整体、也不改数据。
TEST(VolumeCollapseRigid, FlatGroundIsStable) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSurfaceBlock, kSurfaceBlock) }), false);

    const CollapseSpec spec;
    const CollapsePlan plan =
        vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSurfaceBlock, kSurfaceBlock), spec, kMaxUnitsInTest);
    EXPECT_TRUE(plan.units.empty()) << "平地处处有支撑，不该有任何整体倒塌";
    EXPECT_EQ(plan.unsupportedVoxels, 0U);
    EXPECT_TRUE(plan.dirty.empty());
}

// ② 悬空石板成为**一个整体**并从体积中抽出（不再逐列下落）。
TEST(VolumeCollapseRigid, FloatingSlabBecomesSingleUnitAndIsExtracted) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);

    ASSERT_EQ(plan.units.size(), 1U) << "整块石板应成为**一个**连通分量（一个整体）";
    EXPECT_EQ(plan.units.front().voxels.size(), kSlabVoxels);
    EXPECT_EQ(plan.units.front().hullPoints.size() % 3U, 0U);
    EXPECT_GE(plan.units.front().hullPoints.size() / 3U, 4U) << "凸包至少需要 4 个点";
    EXPECT_FALSE(plan.dirty.empty()) << "抽出必须标脏块（随后重网格 + 重建碰撞体）";

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 0U) << "抽出的意思就是原处变空";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 0U);
    EXPECT_GT(CountSolidAtHeight(after, 104), 0U) << "地板仍在（它接地，不该被动）";
}

// ③ 体素化回写：按**原姿态**落定 ⇒ 体素回到原位，守恒。
TEST(VolumeCollapseRigid, WritebackAtOriginalPoseRestoresVoxels) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    // T46：把石板标成**刚性**（岩 = 槽位 2）⇒ 不做接地沉降，本用例只钉"回写数学本身"（守恒 / 落位）；
    // 散体的接地沉降另有专门用例（`VolumeCollapseLanding.GranularWritebackSinksVoxelsOntoSupport`）。
    MarkSlabMaterial(scene.volumes, 2U);

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    ASSERT_TRUE(plan.units.front().rigidDebris) << "整块岩 ⇒ 刚性";

    const CollapsePose pose { plan.units.front().centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };
    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);
    EXPECT_EQ(writeback.writtenVoxels, kSlabVoxels) << "原姿态回写应全部落回原位";
    EXPECT_EQ(writeback.droppedVoxels, 0U);
    EXPECT_EQ(writeback.settledVoxels, 0U) << "刚性不做沉降（形状不变）";
    EXPECT_EQ(writeback.removedFloatingVoxels, 0U);
    EXPECT_FALSE(writeback.dirty.empty());

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 8U * 8U) << "石板应回到原高度（8×8 列）";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 8U * 8U);
    EXPECT_FLOAT_EQ(pose.TiltDegrees(), 0.0F);
}

// ④ 会旋转：以 90° 姿态回写后，形状与"原地不动"显著不同 —— 这正是"逐列下落"原理上做不到的。
TEST(VolumeCollapseRigid, WritebackWithQuarterTurnChangesShape) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 2U);  // T46：刚性（岩）⇒ 不做沉降，只验证"转 90° 后形状变了"

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);

    const CollapsePose pose { plan.units.front().centroid,
                              glm::angleAxis(glm::radians(90.0F), glm::vec3(1.0F, 0.0F, 0.0F)) };
    EXPECT_NEAR(pose.TiltDegrees(), 90.0F, 1.0F) << "倾角判据（阶段计划 T33）依赖它";

    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);
    EXPECT_EQ(writeback.writtenVoxels, kSlabVoxels) << "90° 回写也必须守恒（落点区域内有空位）";
    EXPECT_EQ(writeback.droppedVoxels, 0U);

    const DensityRegion after = ReadScene(scene.volumes);
    const std::size_t   atOriginalHeights = CountSolidAtHeight(after, 109) + CountSolidAtHeight(after, 110);
    EXPECT_LT(atOriginalHeights, kSlabVoxels) << "转了 90° 之后不应还堆在原高度上（8 层高 ⇒ 每层只剩约 1/4）";
}

// ⑤ 确定性：同输入两次运行得到同一分组（红线 7）。
TEST(VolumeCollapseRigid, GroupingIsDeterministic) {
    const MapPreset preset = FlatPreset();
    SlabScene       first(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    SlabScene       second(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    const CollapseSpec spec;
    const CollapsePlan left = vx::ApplyCollapse(first.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    const CollapsePlan right = vx::ApplyCollapse(second.volumes,
                                                 vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                 kMaxUnitsInTest);

    ASSERT_EQ(left.units.size(), right.units.size());
    for (std::size_t i = 0; i < left.units.size(); ++i) {
        EXPECT_EQ(left.units[i].voxels.size(), right.units[i].voxels.size());
        ASSERT_FALSE(left.units[i].voxels.empty());
        EXPECT_TRUE(left.units[i].voxels.front() == right.units[i].voxels.front());
        EXPECT_TRUE(left.units[i].voxels.back() == right.units[i].voxels.back());
        EXPECT_NEAR(left.units[i].centroid.x, right.units[i].centroid.x, 1.0e-9);
        EXPECT_NEAR(left.units[i].centroid.y, right.units[i].centroid.y, 1.0e-9);
        EXPECT_NEAR(left.units[i].centroid.z, right.units[i].centroid.z, 1.0e-9);
    }
}

// ⑥ 关闭时不动数据。
TEST(VolumeCollapseRigid, DisabledSpecMovesNothing) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    CollapseSpec spec;
    spec.enabled = false;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    EXPECT_TRUE(plan.units.empty());
    EXPECT_TRUE(plan.dirty.empty());

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 8U * 8U) << "关闭时数据必须原样";
}

// ⑦ 超上限时**不抽出**（保持原状），只计数 —— 绝不出现"被抽出又没有刚体承载"的空洞。
// 场景 = **两块互不相连**的悬空石板 ⇒ 两个整体；`maxUnits = 1` 只抽出第一块（x 小的那块，确定序）。
TEST(VolumeCollapseRigid, OverUnitLimitKeepsVoxelsInPlace) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), false);

    // 两块石板（x ∈ [4,12) 与 x ∈ [18,26)，其余同）—— 在 (x, z, y) 上互不相邻 ⇒ 两个连通分量。
    DensityRegion twoSlabs = BuildFloatingSlabScene();
    for (int k = 0; k < twoSlabs.sizeZ; ++k) {
        for (int j = 0; j < twoSlabs.sizeY; ++j) {
            for (int i = 0; i < twoSlabs.sizeX; ++i) {
                const int worldX = twoSlabs.minX + i;
                const int worldY = twoSlabs.minY + j;
                const int worldZ = twoSlabs.minZ + k;
                if (worldY >= 109 && worldY < 111 && worldX >= 18 && worldX < 26 && worldZ >= 4 && worldZ < 12) {
                    twoSlabs.values[twoSlabs.Index(i, j, k)] = static_cast<std::int8_t>(vx::kDensityMin);
                }
            }
        }
    }
    scene.volumes.WriteDensityRegion(twoSlabs);

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                /*maxUnits=*/1);
    ASSERT_EQ(plan.units.size(), 1U) << "上限 1 ⇒ 只抽出一个整体";
    EXPECT_EQ(plan.skippedUnits, 1U) << "另一块必须被计数并**保持原状**";
    EXPECT_EQ(plan.unsupportedVoxels, 2U * kSlabVoxels);
    EXPECT_EQ(plan.units.front().voxels.front().x, 4) << "确定序：按体素数降序、同数按最小角点升序";

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 8U * 8U) << "被抽出的那块不见了，另一块仍在（8×8 列）";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 8U * 8U);
}

// ⑧（T42）抽出时抓的**体素补丁**必须是"它原本的样子"，且能网格化成**与地形同源的等值面**：
//   ① 体积里这些体素已被清空，补丁里仍是实心；② 补丁尺寸 = 包围盒向外各扩 1 格采样；
//   ③ 网格非空、索引合法、顶点材质 = **被切开的实体**的材质（不是列派生）。
TEST(VolumeCollapseRigid, UnitPatchAndMeshAreTheOriginalIsosurface) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    // 给石板打一个**与列派生不同**的材质，验证"材质随整体一起走"。
    const std::uint8_t derived = scene.volumes.SampleMaterialSlot(6, 109, 6);
    const std::uint8_t marked  = (derived == 0U) ? 1U : 0U;
    ASSERT_NE(marked, derived);
    for (int x = 4; x < 12; ++x) {
        for (int z = 4; z < 12; ++z) {
            for (int y = 109; y < 111; ++y) {
                scene.volumes.SetMaterialSlot(x, y, z, marked);
            }
        }
    }

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    const vx::CollapseUnit& unit = plan.units.front();

    // ② 补丁 = 包围盒向外各扩 1 格采样（等值面在边界处要用到外围采样）。
    EXPECT_EQ(unit.patchSizeX, unit.bounds.maxX - unit.bounds.minX + 3);
    EXPECT_EQ(unit.patchSizeY, unit.bounds.maxY - unit.bounds.minY + 3);
    EXPECT_EQ(unit.patchSizeZ, unit.bounds.maxZ - unit.bounds.minZ + 3);
    ASSERT_EQ(unit.patchMaterial.size(), unit.patchDensity.size());
    ASSERT_FALSE(unit.patchDensity.empty()) << "抽出前必须抓体素补丁（网格化的原料）";

    // ① 补丁是"抽出前"的证据：体积里已清空，补丁里仍实心。
    const auto patchIndex = [&unit](int x, int y, int z) {
        return static_cast<std::size_t>(x) +
               static_cast<std::size_t>(unit.patchSizeX) *
                   (static_cast<std::size_t>(y) +
                    static_cast<std::size_t>(unit.patchSizeY) * static_cast<std::size_t>(z));
    };
    for (const vx::CollapseUnit::Voxel& voxel : unit.voxels) {
        const int px = voxel.x - unit.patchBounds.minX;
        const int py = voxel.y - unit.patchBounds.minY;
        const int pz = voxel.z - unit.patchBounds.minZ;
        ASSERT_TRUE(px >= 0 && px < unit.patchSizeX && py >= 0 && py < unit.patchSizeY && pz >= 0 &&
                    pz < unit.patchSizeZ);
        EXPECT_LT(unit.patchDensity[patchIndex(px, py, pz)], 0) << "补丁必须是抽出前的密度（否则网格化出来是空的）";
        EXPECT_EQ(unit.patchMaterial[patchIndex(px, py, pz)], marked);
    }
    const DensityRegion afterExtraction = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(afterExtraction, 109), 0U) << "而体积里已经空了";

    // ③ 网格：与地形同源的等值面（顶点为"相对质心"的局部坐标 ⇒ 与凸包同源）。
    const vx::MeshData mesh = vx::BuildCollapseUnitMesh(unit);
    ASSERT_FALSE(mesh.vertices.empty()) << "补丁必须能网格化出等值面";
    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 6U, 0U) << "每个四边形 6 个索引";
    for (const std::uint32_t index : mesh.indices) {
        EXPECT_LT(index, mesh.vertices.size()) << "索引必须落在顶点范围内";
    }
    for (const vx::MeshVertex& vertex : mesh.vertices) {
        EXPECT_GE(vertex.position[0], static_cast<float>(unit.bounds.minX - 2) - static_cast<float>(unit.centroid.x));
        EXPECT_LE(vertex.position[0], static_cast<float>(unit.bounds.maxX + 2) - static_cast<float>(unit.centroid.x));
        EXPECT_FLOAT_EQ(vertex.material, static_cast<float>(marked)) << "顶点材质必须是**被切开的实体**的材质";
    }
}

// ⑨（T42）回写把**材质**一起搬到落点：残骸落地后仍是它抽走时的材质，
//   而不是按"落点那一列的地表材质"重算（否则一块岩体落地会变成泥土的颜色）。
TEST(VolumeCollapseRigid, WritebackCarriesMaterialToLandingSite) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    const std::uint8_t derived = scene.volumes.SampleMaterialSlot(6, 104, 6);  // 落点那一列（地板）的派生材质
    // T46：标记材质取**岩**（刚性）⇒ 回写不做沉降，落点即原位 —— 本用例只验证"材质随残骸搬走"，
    // 若标成散体（土）则体素会接地沉降到地板顶面，断言位置随之改变（那是另一个用例的事）。
    const std::uint8_t marked  = 2U;
    ASSERT_NE(marked, derived) << "本用例要求标记材质与列派生**不同**，否则判据无意义";
    for (int x = 4; x < 12; ++x) {
        for (int z = 4; z < 12; ++z) {
            for (int y = 109; y < 111; ++y) {
                scene.volumes.SetMaterialSlot(x, y, z, marked);
            }
        }
    }
    EXPECT_GT(scene.volumes.MaterialBytes(), 0U) << "写入材质必须懒分配（此处已分配）";

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);

    const CollapsePose pose { plan.units.front().centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };
    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);
    EXPECT_EQ(writeback.writtenVoxels, kSlabVoxels);

    EXPECT_EQ(scene.volumes.SampleMaterialSlot(6, 109, 6), marked) << "回写后仍应是它原本的材质（不按落点列重算）";
    EXPECT_EQ(scene.volumes.SampleMaterialSlot(6, 104, 6), derived) << "未被写过的地层仍回落列派生（零回归）";
}

// ⑩（T42）"抽出前后外观连续"的**自动化判据**：倒塌整体的网格必须是**它被切下来之前那片地形表面**的子集
//   —— 逐三角形在**世界坐标**下都能在该体积块的网格里找到对应（位置一致到 4 ULP 量级、材质相同）。
//   为什么不用截图隔离：挖除与抽出**同帧发生**，洞口本身也在变，截图无法把"口径切换"单独量出来。
TEST(VolumeCollapseRigid, UnitMeshIsSubsetOfTheTerrainSurfaceItWasCutFrom) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    // 抽出**之前**：石板所在块的地形网格（块内局部坐标；世界定位由块坐标承担 —— 红线 6）。
    // 注意：`WriteDensityRegion` 只改密度、**不重网格** ⇒ 必须先重网格该块，拿到的才是"当时的地形表面"。
    const BlockCoord slabBlock { 0, 3, 0 };  // 世界 y ∈ [96, 128)，覆盖石板（109..110）
    ASSERT_TRUE(scene.volumes.RemeshBlock(slabBlock));
    const vx::MeshData* blockMeshPointer = scene.volumes.FindMesh(slabBlock);
    ASSERT_NE(blockMeshPointer, nullptr);
    ASSERT_FALSE(blockMeshPointer->indices.empty());
    const vx::MeshData blockMesh = *blockMeshPointer;  // 拷贝：`ApplyCollapse` 之后指针可能失效
    const glm::dvec3   blockOrigin(vx::BlockOriginBlocks(slabBlock.x), vx::BlockOriginBlocks(slabBlock.y),
                                   vx::BlockOriginBlocks(slabBlock.z));

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    const vx::MeshData unitMesh = vx::BuildCollapseUnitMesh(plan.units.front());
    ASSERT_FALSE(unitMesh.indices.empty());

    constexpr double kTolerance = 1.0e-4;
    std::size_t      matched   = 0;
    for (std::size_t triangle = 0; triangle + 2 < unitMesh.indices.size(); triangle += 3) {
        bool found = false;
        for (std::size_t reference = 0; reference + 2 < blockMesh.indices.size() && !found; reference += 3) {
            bool sameTriangle = true;
            for (int corner = 0; corner < 3 && sameTriangle; ++corner) {
                const vx::MeshVertex& blockVertex = blockMesh.vertices[blockMesh.indices[reference + corner]];
                const vx::MeshVertex& unitVertex  = unitMesh.vertices[unitMesh.indices[triangle + corner]];
                for (int axis = 0; axis < 3 && sameTriangle; ++axis) {
                    const double blockWorld = (axis == 0) ? blockOrigin.x + static_cast<double>(blockVertex.position[0])
                                             : (axis == 1) ? blockOrigin.y + static_cast<double>(blockVertex.position[1])
                                                           : blockOrigin.z + static_cast<double>(blockVertex.position[2]);
                    const double unitWorld  = (axis == 0)
                                                  ? plan.units.front().centroid.x + static_cast<double>(unitVertex.position[0])
                                              : (axis == 1) ? plan.units.front().centroid.y + static_cast<double>(unitVertex.position[1])
                                                            : plan.units.front().centroid.z + static_cast<double>(unitVertex.position[2]);
                    sameTriangle = std::abs(blockWorld - unitWorld) <= kTolerance;
                }
                sameTriangle = sameTriangle && blockVertex.material == unitVertex.material;
            }
            found = sameTriangle;
        }
        EXPECT_TRUE(found) << "整体网格的第 " << triangle / 3 << " 个三角形在“原地形网格”里找不到对应";
        matched += found ? 1U : 0U;
    }
    EXPECT_EQ(matched, unitMesh.indices.size() / 3U)
        << "倒塌整体的每一个三角形都必须来自它**原本那片等值面**（口径同源，掉落中不再是另一套方块外观）";
}

// ⑪ T30 口径的性能基准：真实游戏规模下一次爆炸的支撑检查 + 分组 + 抽出的耗时。只打印、不断言时间。
TEST(VolumeCollapseRigid, ProfileGameScaleExplosion) {
    const MapPreset preset = FlatPreset();
    TerrainWorld    world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    for (int tileZ = -1; tileZ <= 0; ++tileZ) {
        for (int tileX = -1; tileX <= 0; ++tileX) {
            world.LoadTile(tileX, tileZ);
        }
    }

    const DigRegionTable regions =
        DigRegionTable::FromRegions({ MakeRegion(BlockCoord { -2, 0, -2 }, BlockCoord { 0, 8, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();

    const CollapseSpec spec;
    vx::Clock         clock;
    (void)clock.Tick();  // 构造后的首值无意义（计数器从 0 起步），丢弃
    const auto lapMs = [&clock]() { return clock.Tick() * 1000.0; };

    const glm::dvec3 centers[3] = { { -16.0, 112.0, -16.0 }, { -8.0, 112.0, -16.0 }, { -24.0, 112.0, -16.0 } };
    double           carveMs = 0.0;
    double           collapseMs = 0.0;
    std::size_t      regionSamples = 0;
    std::size_t      unsupported = 0;
    std::size_t      units = 0;

    for (const glm::dvec3& center : centers) {
        std::vector<BlockCoord> dirty;
        vx::VoxelBounds         carvedBounds;
        const bool              carved = volumes.CarveSphere(center, 3.0F, dirty, &carvedBounds);
        carveMs += lapMs();
        ASSERT_TRUE(carved) << "前置条件：球心必须落在实心岩体里";

        const CollapsePlan plan = vx::ApplyCollapse(volumes, vx::CollapseSeed { carvedBounds }, spec, kMaxUnitsInTest);
        collapseMs += lapMs();

        regionSamples = plan.regionSamples;
        unsupported += plan.unsupportedVoxels;
        units += plan.units.size();
    }

    std::printf("[T33 基准] 支撑检查 + 分组 + 抽出：邻域 %zu 采样；3 次爆炸平均 挖除 %.2f ms + 倒塌 %.2f ms"
                "（合计失去支撑 %zu 体素、刚体化 %zu 个整体）\n",
                regionSamples, carveMs / 3.0, collapseMs / 3.0, unsupported, units);
    std::fflush(stdout);
}

// 配置表：仓库内已提交的表必须能加载，且与 `Default()` 取值一致（防"配置与默认值漂移"）。
TEST(CollapseTable, LoadsShippedTableAndMatchesDefault) {
    const CollapseTable fromFile =
        CollapseTable::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets/config/collapse.toml");
    EXPECT_EQ(fromFile.SchemaVersion(), CollapseTable::kSchemaVersion);
    EXPECT_TRUE(fromFile.Spec().enabled);
    EXPECT_FLOAT_EQ(fromFile.Spec().maxCantileverBlocks, 4.0F);
    EXPECT_EQ(fromFile.Spec().neighborhoodMarginBlocks, 0);
    EXPECT_FLOAT_EQ(fromFile.Spec().settleLinearSpeed, 0.6F);
    EXPECT_FLOAT_EQ(fromFile.Spec().settleAngularSpeed, 0.7F);
    EXPECT_EQ(fromFile.Spec().settleSteps, 12);
    EXPECT_FLOAT_EQ(fromFile.Spec().initialTiltSpeed, 0.9F);
    EXPECT_FLOAT_EQ(fromFile.Spec().impulseSpeed, 8.0F);
    EXPECT_EQ(fromFile.Spec().debrisDeleteMaxVoxels, 24);
    EXPECT_EQ(fromFile.Spec().maxActiveUnits, 4);

    const CollapseTable defaults = CollapseTable::Default();
    EXPECT_EQ(defaults.Spec().enabled, fromFile.Spec().enabled);
    EXPECT_FLOAT_EQ(defaults.Spec().maxCantileverBlocks, fromFile.Spec().maxCantileverBlocks);
    EXPECT_EQ(defaults.Spec().neighborhoodMarginBlocks, fromFile.Spec().neighborhoodMarginBlocks);
    EXPECT_FLOAT_EQ(defaults.Spec().settleLinearSpeed, fromFile.Spec().settleLinearSpeed);
    EXPECT_FLOAT_EQ(defaults.Spec().settleAngularSpeed, fromFile.Spec().settleAngularSpeed);
    EXPECT_EQ(defaults.Spec().settleSteps, fromFile.Spec().settleSteps);
    EXPECT_FLOAT_EQ(defaults.Spec().initialTiltSpeed, fromFile.Spec().initialTiltSpeed);
    EXPECT_FLOAT_EQ(defaults.Spec().impulseSpeed, fromFile.Spec().impulseSpeed);
    EXPECT_EQ(defaults.Spec().debrisDeleteMaxVoxels, fromFile.Spec().debrisDeleteMaxVoxels);
    EXPECT_EQ(defaults.Spec().maxActiveUnits, fromFile.Spec().maxActiveUnits);

    EXPECT_THROW(static_cast<void>(CollapseTable::LoadFromFile("no_such_collapse.toml")), std::runtime_error);
}

// ---------------------------------------------------------------------------
// T43 / ADR 0016：倒塌真实感（① 爆心冲量、② 材质化物理参数、③ 小碎片清除）
// ---------------------------------------------------------------------------

// ⑫ 爆心冲量：方向**朝外**（背离爆心）、大小随距离**衰减**（同半径下更远 ⇒ 更小），
//    并且因冲量在分量上分布不均而产生角速度 —— 这就是"被炸飞 + 翻滚"与"原地垂直落下"的差别。
TEST(VolumeCollapseImpulse, PointsAwayFromEpicenterAndDecaysWithDistance) {
    const MapPreset preset = FlatPreset();
    const CollapseSpec spec;
    const auto makeScene = [&preset]() {
        return SlabScene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    };

    // 石板列中心 ≈ (8, 110, 8)。爆心放在 −x 一侧、与石板同高、z 取 8（= 列中心 ⇒ z 分量应精确抵消）。
    // 两次运行**半径相同**（200）⇒ 唯一变量是距离，速度差只可能来自 `falloff = 1 − d / R`。
    SlabScene nearScene = makeScene();
    const CollapsePlan nearPlan = vx::ApplyCollapse(nearScene.volumes, SeedWithEpicenter(glm::dvec3(-20.0, 109.5, 8.0), 200.0),
                                                    spec, kMaxUnitsInTest);
    ASSERT_EQ(nearPlan.units.size(), 1U);
    const vx::CollapseUnit& nearUnit = nearPlan.units.front();
    ASSERT_TRUE(nearUnit.hasImpulse) << "有爆心 + 正冲量速度 ⇒ 必须给出冲量";
    const glm::vec3 nearVelocity = nearUnit.impulseLinearVelocity;
    const glm::vec3 nearAngular  = nearUnit.impulseAngularVelocity;

    EXPECT_GT(nearVelocity.x, 0.0F) << "冲量必须把分量**推离**爆心（爆心在 −x ⇒ 速度向 +x）";
    EXPECT_NEAR(nearVelocity.z, 0.0F, 1.0e-4F) << "爆心与石板列中心同 z ⇒ z 分量成对抵消";
    EXPECT_TRUE(std::isfinite(nearVelocity.x) && std::isfinite(nearVelocity.y) && std::isfinite(nearVelocity.z))
        << "绝不把 NaN / Inf 喂给物理";
    EXPECT_TRUE(std::isfinite(nearAngular.x) && std::isfinite(nearAngular.y) && std::isfinite(nearAngular.z));
    EXPECT_LE(glm::length(nearVelocity), spec.impulseSpeed + 1.0e-4F) << "上界：逐体素 falloff ≤ 1 ⇒ 平均 ≤ 冲量速度";
    EXPECT_GT(glm::length(nearAngular), 0.0F) << "冲量沿 −x 分布不均（近处快、远处慢）⇒ 力矩非零 ⇒ 会翻滚";

    // 更远：同方向、同半径，只是爆心退到 −70 ⇒ 每个体素的 `falloff` 更小 ⇒ |V| 更小。
    SlabScene farScene = makeScene();
    const CollapsePlan farPlan = vx::ApplyCollapse(farScene.volumes, SeedWithEpicenter(glm::dvec3(-70.0, 109.5, 8.0), 200.0),
                                                   spec, kMaxUnitsInTest);
    ASSERT_EQ(farPlan.units.size(), 1U);
    ASSERT_TRUE(farPlan.units.front().hasImpulse);
    EXPECT_GT(farPlan.units.front().impulseLinearVelocity.x, 0.0F) << "方向与距离无关（仍背离爆心）";
    EXPECT_LT(glm::length(farPlan.units.front().impulseLinearVelocity), glm::length(nearVelocity))
        << "同一半径下更远 ⇒ `falloff = 1 − d / R` 更小 ⇒ 初速度更小";
}

// ⑬ 确定性（红线 7）：同输入两次 ⇒ 分组、材质统计与冲量折算**逐位相同**（整条链不含随机 / 时序依赖）。
TEST(VolumeCollapseImpulse, ImpulseIsDeterministic) {
    const MapPreset preset = FlatPreset();
    const CollapseSpec spec;
    SlabScene          first(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    SlabScene          second(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(first.volumes, 2U);
    MarkSlabMaterial(second.volumes, 2U);

    const vx::CollapseSeed seed = SeedWithEpicenter(glm::dvec3(-20.0, 100.0, 8.0), 60.0);
    const CollapsePlan     left  = vx::ApplyCollapse(first.volumes, seed, spec, kMaxUnitsInTest);
    const CollapsePlan     right = vx::ApplyCollapse(second.volumes, seed, spec, kMaxUnitsInTest);

    ASSERT_EQ(left.units.size(), 1U);
    ASSERT_EQ(right.units.size(), 1U);
    ASSERT_TRUE(left.units.front().hasImpulse);

    EXPECT_EQ(left.units.front().mass, right.units.front().mass);
    EXPECT_EQ(left.units.front().friction, right.units.front().friction);
    EXPECT_EQ(left.units.front().restitution, right.units.front().restitution);
    for (int axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(left.units.front().impulseLinearVelocity[axis], right.units.front().impulseLinearVelocity[axis]);
        EXPECT_EQ(left.units.front().impulseAngularVelocity[axis], right.units.front().impulseAngularVelocity[axis]);
    }
    // 同时确认这一路数值**非平凡**（否则"逐位相同"可能只是两个零）。
    EXPECT_GT(glm::length(left.units.front().impulseLinearVelocity), 1.0e-3F);
}

// ⑭ 没有冲量来源时**绝不假装有冲量**（否则调用方不会退回人工倾斜，细长塔会竖直落下而不倒）：
//    `impulse_speed = 0` / 种子无爆心（`radius = 0`）/ 整个分量都在半径之外 ⇒ 三种情形都 `hasImpulse == false`。
TEST(VolumeCollapseImpulse, ZeroImpulseSpeedProducesNoImpulse) {
    const MapPreset preset = FlatPreset();
    const auto       run   = [&preset](const CollapseSpec& spec, const vx::CollapseSeed& seed) {
        SlabScene          scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        EXPECT_EQ(plan.units.size(), 1U);
        return plan.units.front();
    };

    CollapseSpec noImpulse;
    noImpulse.impulseSpeed = 0.0F;
    const vx::CollapseUnit disabled = run(noImpulse, SeedWithEpicenter(glm::dvec3(0.0, 100.0, 0.0), 60.0));
    EXPECT_FALSE(disabled.hasImpulse) << "impulse_speed = 0 ⇒ 没有冲量来源";
    EXPECT_FLOAT_EQ(glm::length(disabled.impulseLinearVelocity), 0.0F);
    EXPECT_FLOAT_EQ(glm::length(disabled.impulseAngularVelocity), 0.0F);

    const CollapseSpec     spec;
    const vx::CollapseUnit noEpicenter = run(spec, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax));
    EXPECT_FALSE(noEpicenter.hasImpulse) << "种子没有爆心（radius = 0）⇒ 退回人工倾斜";

    const vx::CollapseUnit outOfRange = run(spec, SeedWithEpicenter(glm::dvec3(-500.0, 109.5, 8.0), 60.0));
    EXPECT_FALSE(outOfRange.hasImpulse) << "整个分量都在半径之外 ⇒ 折算出的速度为零 ⇒ 也算没有冲量";
}

// ⑮ 物理参数按**材质表**（ADR 0016 决策二）：质量 = Σ（逐体素密度）、摩擦 / 弹性 = 材质表的值。
TEST(VolumeCollapseMaterial, MassFrictionAndRestitutionFollowVoxelMaterial) {
    const MapPreset            preset    = FlatPreset();
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    ASSERT_GT(materials.Layer(2).density, materials.Layer(1).density) << "岩必须比土重（判据 ④ 的前提）";

    SlabScene scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 2U);  // 整块 = 岩

    const CollapseSpec spec;  // 无爆心 ⇒ 本用例只验证物理参数
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                                spec, kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    const vx::CollapseUnit& unit = plan.units.front();
    ASSERT_EQ(unit.voxels.size(), kSlabVoxels);

    EXPECT_NEAR(unit.mass, static_cast<float>(kSlabVoxels) * materials.Layer(2).density, 0.05F)
        << "质量 = 逐体素累加密度（混合材质因此正确）";
    EXPECT_FLOAT_EQ(unit.friction, materials.Layer(2).friction);
    EXPECT_FLOAT_EQ(unit.restitution, materials.Layer(2).restitution);
}

// ⑯ 物理参数按**子块的材质**（T43 的质量 / T50 落地口径）：
//    T50 起抽出的连通分量先按**材质一致性**拆成子块 ⇒ 一个子块只有一种材质，
//    故"多数票 / 同票取更小槽位"这套折中**已不需要**（每个子块直接取自己材质的值）。
//    质量仍然**逐体素**累加（= 该子块全体材质的密度之和）。
TEST(VolumeCollapseMaterial, EachSubBlockTakesItsOwnMaterialParameters) {
    const MapPreset            preset    = FlatPreset();
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    const CollapseSpec         spec;

    // 沙（槽 3）x < 10 的 6 列 = 96 体素、土（槽 1）x ≥ 10 的 2 列 = 32 体素 ⇒ **两个子块**。
    SlabScene scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterialBy(scene.volumes, [](int x, int, int) -> std::uint8_t { return x < 10 ? 3U : 1U; });
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 2U) << "两种材质 ⇒ **两个子块**（T50 / ADR 0018 决策四）";

    // 确定序：体素数降序 ⇒ 沙（96）在前、土（32）在后。
    const vx::CollapseUnit& sand = plan.units[0];
    const vx::CollapseUnit& dirt = plan.units[1];
    EXPECT_EQ(sand.voxels.size(), 96U);
    EXPECT_EQ(dirt.voxels.size(), 32U);
    EXPECT_FLOAT_EQ(sand.friction, materials.Layer(3).friction) << "沙子块取自己的摩擦";
    EXPECT_FLOAT_EQ(dirt.friction, materials.Layer(1).friction) << "土子块取自己的摩擦";
    EXPECT_FLOAT_EQ(sand.restitution, materials.Layer(3).restitution);
    EXPECT_FLOAT_EQ(dirt.restitution, materials.Layer(1).restitution);
    EXPECT_NEAR(sand.mass, 96.0F * materials.Layer(3).density, 0.05F) << "质量逐体素累加";
    EXPECT_NEAR(dirt.mass, 32.0F * materials.Layer(1).density, 0.05F);
    for (const vx::CollapseUnit::Voxel& voxel : sand.voxels) {
        EXPECT_LT(voxel.x, 10) << "划分必须严格按材质边界（不许把土并进沙子块）";
    }
    for (const vx::CollapseUnit::Voxel& voxel : dirt.voxels) {
        EXPECT_GE(voxel.x, 10);
    }

    // 分层场景（y = 109 土 / y = 110 沙）⇒ 同样是两个子块：划分只看"材质 + 6 邻域连通"，与"层"无关。
    SlabScene layered(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterialBy(layered.volumes, [](int, int y, int) -> std::uint8_t { return y == 109 ? 1U : 3U; });
    const CollapsePlan layeredPlan = vx::ApplyCollapse(
        layered.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec, kMaxUnitsInTest);
    ASSERT_EQ(layeredPlan.units.size(), 2U);
    for (const vx::CollapseUnit& unit : layeredPlan.units) {
        EXPECT_EQ(unit.voxels.size(), 64U) << "每层 8 × 8 列 = 64 体素";
    }

    // 划分**确定性**（红线 7）：同输入两次 ⇒ 子块数、体素数、质心逐位相同。
    SlabScene second(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterialBy(second.volumes, [](int x, int, int) -> std::uint8_t { return x < 10 ? 3U : 1U; });
    const CollapsePlan repeated = vx::ApplyCollapse(second.volumes,
                                                    vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                                    spec, kMaxUnitsInTest);
    ASSERT_EQ(repeated.units.size(), plan.units.size());
    for (std::size_t i = 0; i < plan.units.size(); ++i) {
        EXPECT_EQ(repeated.units[i].voxels.size(), plan.units[i].voxels.size());
        EXPECT_TRUE(repeated.units[i].voxels.front() == plan.units[i].voxels.front());
        EXPECT_TRUE(repeated.units[i].voxels.back() == plan.units[i].voxels.back());
        EXPECT_NEAR(repeated.units[i].centroid.x, plan.units[i].centroid.x, 1.0e-9);
        EXPECT_NEAR(repeated.units[i].centroid.y, plan.units[i].centroid.y, 1.0e-9);
        EXPECT_NEAR(repeated.units[i].centroid.z, plan.units[i].centroid.z, 1.0e-9);
    }
}

// ⑰ 小碎片清除（ADR 0016 决策三）：体素数 ≤ 阈值的分量**直接清除**（"当炸没了"）——
//    不生成刚体、不回写，只计数；阈值边界精确（= 阈值清除、阈值 − 1 保留）。
TEST(VolumeCollapseDebris, SmallDebrisIsDeletedInsteadOfSpawningRigidBody) {
    const MapPreset preset = FlatPreset();
    const auto makeScene = [&preset]() {
        return SlabScene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    };

    CollapseSpec deletedSpec;
    deletedSpec.debrisDeleteMaxVoxels = static_cast<int>(kSlabVoxels);  // 阈值 = 石板体素数 ⇒ 清除
    SlabScene              deletedScene = makeScene();
    const vx::CollapseSeed seed         = vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax);
    const CollapsePlan     deleted      = vx::ApplyCollapse(deletedScene.volumes, seed, deletedSpec, kMaxUnitsInTest);

    EXPECT_TRUE(deleted.units.empty()) << "被清除的碎片**不得**进入刚体列表（也就不会生成刚体 / 回写）";
    EXPECT_EQ(deleted.deletedUnits, 1U);
    EXPECT_EQ(deleted.deletedVoxels, kSlabVoxels);
    EXPECT_EQ(deleted.unsupportedVoxels, kSlabVoxels) << "失去支撑的计数含被清除的碎片（诊断口径）";
    EXPECT_FALSE(deleted.dirty.empty()) << "清除也是体积改动 ⇒ 必须标脏块（重网格）";
    const DensityRegion afterDelete = ReadScene(deletedScene.volumes);
    EXPECT_EQ(CountSolidAtHeight(afterDelete, 109), 0U) << "清除 = 变空（当炸没了）";

    CollapseSpec keptSpec;
    keptSpec.debrisDeleteMaxVoxels = static_cast<int>(kSlabVoxels) - 1;  // 差 1 个体素 ⇒ 不算碎片
    SlabScene              keptScene = makeScene();
    const CollapsePlan     kept      = vx::ApplyCollapse(keptScene.volumes, seed, keptSpec, kMaxUnitsInTest);
    ASSERT_EQ(kept.units.size(), 1U) << "阈值边界：恰好多 1 个体素就必须走常规刚体化";
    EXPECT_EQ(kept.deletedUnits, 0U);
    EXPECT_EQ(kept.deletedVoxels, 0U);
}

// ⑱ 清除守卫（ADR 0016 决策三）：含**不可破坏**材质的分量**不被清除**（仍走常规流程）；
//    同场景换成"同一槽位但可破坏"的表 ⇒ 立刻被清除。对照让"守卫真的在起作用"可证伪。
TEST(VolumeCollapseDebris, IndestructibleDebrisIsNotDeleted) {
    const MapPreset            preset = FlatPreset();
    const TerrainMaterialTable guarded = LoadMaterialsWithIndestructibleRock(true);
    const TerrainMaterialTable plain   = LoadMaterialsWithIndestructibleRock(false);
    ASSERT_TRUE(guarded.Layer(2).indestructible);
    ASSERT_FALSE(plain.Layer(2).indestructible);

    CollapseSpec spec;
    spec.debrisDeleteMaxVoxels = static_cast<int>(kSlabVoxels) * 2;  // 阈值足够大 ⇒ 唯一阻止清除的是守卫
    const vx::CollapseSeed seed = vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax);

    SlabScene guardedScene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                           guarded);
    MarkSlabMaterial(guardedScene.volumes, 2U);  // 岩（不可破坏）
    const CollapsePlan kept = vx::ApplyCollapse(guardedScene.volumes, seed, spec, kMaxUnitsInTest);
    EXPECT_EQ(kept.deletedUnits, 0U) << "含不可破坏材质的分量必须**不被清除**";
    ASSERT_EQ(kept.units.size(), 1U) << "它仍走常规流程（刚体化）";

    SlabScene plainScene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                         plain);
    MarkSlabMaterial(plainScene.volumes, 2U);  // 同一槽位、同一密度，但可破坏
    const CollapsePlan removed = vx::ApplyCollapse(plainScene.volumes, seed, spec, kMaxUnitsInTest);
    EXPECT_EQ(removed.deletedUnits, 1U) << "对照：同材质但可破坏 ⇒ 被清除（证明上面的差别正是守卫）";
    EXPECT_TRUE(removed.units.empty());
}

// ---------------------------------------------------------------------------
// T46 / ADR 0017：落地后的表示按材质分流（刚性保留几何体 / 散体回写融合并**接地沉降**）
// ---------------------------------------------------------------------------

// ⑲ 落地口径（T50 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策四）
//    = **该子块自己的材质**（`materials.toml` 的 `rigid_debris`）。
//    T46 曾用"露在外面的**皮**上是否出现刚性材质"来判（在"整块一个 bool"的前提下最接近的近似）；
//    T50 起抽出的分量**按材质拆成子块** ⇒ 每个子块只有一种材质，直接取该材质的 `rigid_debris`。
//    这同时满足项目所有者实测的两条口径："**岩石始终不变**" + "**泥土下坠到有支撑为止**"：
//      · 岩子块 ⇒ 保留几何体（不回写 ⇒ 形状与掉落中一致）；
//      · 土 / 草 / 沙子块 ⇒ 回写并与地面融合（且接地沉降）。
TEST(VolumeCollapseLanding, RigidityFollowsEachSubBlockMaterial) {
    const MapPreset            preset    = FlatPreset();
    const CollapseSpec         spec;
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    ASSERT_TRUE(materials.Layer(2).rigidDebris) << "前提：岩 = 刚性（`rigid_debris = true`）";
    ASSERT_FALSE(materials.Layer(1).rigidDebris) << "前提：土 = 散体";
    const vx::CollapseSeed seed = vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax);
    const auto regions = []() { return DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }); };

    // ① 整块岩 ⇒ 单子块 ⇒ 刚性（落定后保留几何体）。
    {
        SlabScene scene(preset, regions(), true);
        MarkSlabMaterial(scene.volumes, 2U);
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        ASSERT_EQ(plan.units.size(), 1U);
        EXPECT_TRUE(plan.units.front().rigidDebris);
    }
    // ② 整块土 ⇒ 单子块 ⇒ 散体（落定后回写、与地面融合）。
    {
        SlabScene scene(preset, regions(), true);
        MarkSlabMaterial(scene.volumes, 1U);
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        ASSERT_EQ(plan.units.size(), 1U);
        EXPECT_FALSE(plan.units.front().rigidDebris);
    }
    // ③ **实测场景（BUG2 / "长条里岩石与泥土同时存在"）**：同一块里土 / 岩各半 ⇒ 拆成两个子块，
    //    岩子块刚性（形状不变）、土子块散体（下坠融合）—— 一个整体一个 bool 做不到这件事。
    {
        SlabScene scene(preset, regions(), true);
        MarkSlabMaterialBy(scene.volumes, [](int x, int, int) -> std::uint8_t { return x < 10 ? 1U : 2U; });
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        ASSERT_EQ(plan.units.size(), 2U) << "土 / 岩各半 ⇒ 两个子块";
        // 确定序：体素数降序 ⇒ 土（6 列 = 96）/ 岩（2 列 = 32）
        EXPECT_FALSE(plan.units[0].rigidDebris) << "土子块 = 散体（回写融合 + 接地沉降）";
        EXPECT_TRUE(plan.units[1].rigidDebris) << "岩子块 = 刚性（形状不变）";
        EXPECT_EQ(plan.units[1].voxels.size(), 32U);
    }
    // ④ 与 ③ 互补：岩占多数（6 列）/ 土占少数（2 列）⇒ 岩子块同样刚性、土子块同样散体。
    {
        SlabScene scene(preset, regions(), true);
        MarkSlabMaterialBy(scene.volumes, [](int x, int, int) -> std::uint8_t { return x < 10 ? 2U : 1U; });
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        ASSERT_EQ(plan.units.size(), 2U);
        EXPECT_TRUE(plan.units[0].rigidDebris);
        EXPECT_FALSE(plan.units[1].rigidDebris);
    }
    // ⑤ 土（y = 109）与草（y = 110）**都不是刚性** ⇒ 拆成两个子块，但**两个都是散体**（都要融合 + 沉降）。
    {
        SlabScene scene(preset, regions(), true);
        MarkSlabMaterialBy(scene.volumes, [](int, int y, int) -> std::uint8_t { return y == 109 ? 1U : 0U; });
        const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, seed, spec, kMaxUnitsInTest);
        ASSERT_EQ(plan.units.size(), 2U);
        EXPECT_FALSE(plan.units[0].rigidDebris);
        EXPECT_FALSE(plan.units[1].rigidDebris);
    }
}

// ⑲a（T46 修订 / 网格闭合）**四周都是空气的独立碎块**同样必须闭合。
//    缺陷（已修）：网格器"每条网格棱由 u/v 下侧的 cell 发射一次"——独立碎块**没有相邻块**替它发射
//    区域边界那一圈四边形 ⇒ 越界的那几面**整个缺失**（实测悬空石板 192 三角形 / 36 条边界边；
//    修好后 384 三角形 / 0 条）。这是项目所有者"部分面没有颜色、某些角度看过去透明"的**主因**。
TEST(VolumeCollapseLanding, DetachedUnitMeshIsWatertight) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    CollapseSpec spec;
    spec.debrisDeleteMaxVoxels = 0;  // 石板 128 体素 > 默认阈值 24 ⇒ 关掉清除
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    const vx::MeshData mesh = vx::BuildCollapseUnitMesh(plan.units.front());
    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(vx::CountBoundaryEdges(mesh), 0U) << "独立碎块的外观网格也必须闭合（不许看穿）";
}

// ⑲b（T46 修订 / 断口闭合）**仍连在未塌岩体上**的碎块，其外观网格必须**闭合**。
//    缺陷（已修）：网格化把"非本整体的实心采样"也当实心 ⇒ 断口那几面没有密度变号、**不生成等值面**
//    ⇒ 掉落中能从断口**看穿**它（项目所有者实测："部分面没有颜色、某些角度看过去透明，但物理碰撞真实存在"）。
TEST(VolumeCollapseLanding, AttachedUnitMeshIsWatertight) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                          TerrainMaterialTable::Default(), BuildAttachedShelfScene());

    CollapseSpec spec;
    spec.maxCantileverBlocks  = 1.0F;  // 立柱外 2 格的板失去支撑（但仍与立柱旁那圈实心相邻）
    spec.debrisDeleteMaxVoxels = 0;    // 本用例的整体很小 ⇒ 关掉"小碎片清除"，否则会被删掉
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U) << "悬挑板应作为一个整体被抽出";
    const vx::CollapseUnit& unit = plan.units.front();
    ASSERT_FALSE(unit.voxels.empty());

    // 场景前提：该整体**确实还连着未塌的实心**（否则这个用例测不到断口）。
    const auto isUnitVoxel = [&unit](int x, int y, int z) {
        return std::any_of(unit.voxels.begin(), unit.voxels.end(),
                           [x, y, z](const vx::CollapseUnit::Voxel& voxel) {
                               return voxel.x == x && voxel.y == y && voxel.z == z;
                           });
    };
    const int neighbours[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
    bool      touchesIntactSolid = false;
    for (const vx::CollapseUnit::Voxel& voxel : unit.voxels) {
        for (const int(&offset)[3] : neighbours) {
            const int nx = voxel.x + offset[0];
            const int ny = voxel.y + offset[1];
            const int nz = voxel.z + offset[2];
            if (scene.volumes.IsSolid(nx, ny, nz) && !isUnitVoxel(nx, ny, nz)) {
                touchesIntactSolid = true;
            }
        }
    }
    ASSERT_TRUE(touchesIntactSolid) << "前提：碎块还连着未塌的实心（断口存在）";

    const vx::MeshData unitMesh = vx::BuildCollapseUnitMesh(unit);
    ASSERT_FALSE(unitMesh.indices.empty());
    EXPECT_EQ(vx::CountBoundaryEdges(unitMesh), 0U)
        << "碎块外观网格必须闭合：断口处也要生成等值面，否则掉落中能看穿它";
}

// ⑲c（T47 / BUG3 取证）**长条**碎块的等值面也必须闭合。
//    项目所有者实测"掉落中的长条 / 落地后的长条上有些面直接透明"，故把该形状钉成**可复现判据**：
//    若长条的等值面本身有洞，该用例（与游戏内 T47 插桩的 WARN）会一起报出 ⇒ 命中候选②。
//    反之（本用例 0 条边界边）则说明"长条"的缺面不在整体网格里 ⇒ 指向候选③（地形↔体积交界缝）。
TEST(VolumeCollapseLanding, LongBarUnitMeshIsWatertight) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                          TerrainMaterialTable::Default(), BuildLongBarScene());

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U) << "细长悬空体应成为**一个**连通分量（一个整体）";
    EXPECT_EQ(plan.units.front().voxels.size(), 30U * 2U * 2U) << "长条 = 30 × 2 × 2 个体素";

    const vx::MeshData mesh = vx::BuildCollapseUnitMesh(plan.units.front());
    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(vx::CountBoundaryEdges(mesh), 0U) << "长条外观网格必须闭合（不许从任何角度看穿）";
}

// ⑳ 散体回写后**必须接地**（"泥土不能悬空"）：悬空石板（土）按原姿态回写 ⇒ 体素沿本列下落到地板顶面，
//    且区域内**不存在"下方为空"的实心体素**。
TEST(VolumeCollapseLanding, GranularWritebackSinksVoxelsOntoSupport) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 1U);  // 土 = 散体

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    ASSERT_FALSE(plan.units.front().rigidDebris);

    // 原姿态 = 回到"悬空"的 109 / 110（空腔 105..108 之上）⇒ 沉降必须把它们带到地板顶面（实心到 104）。
    const CollapsePose pose { plan.units.front().centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };
    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);
    EXPECT_EQ(writeback.writtenVoxels, kSlabVoxels);
    EXPECT_EQ(writeback.settledVoxels, kSlabVoxels) << "每一个体素都从悬空处落到了支撑面上";
    EXPECT_EQ(writeback.removedFloatingVoxels, 0U)
        << "能降到支撑面的体素**不该被清除**（T51 只在「降不到支撑」时才删）";

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 0U) << "不该留在原来的悬空高度";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 0U);
    EXPECT_EQ(CountSolidAtHeight(after, 108), 0U) << "沉降不得在列里留下「实心轨迹」（本该是空的格不能变实心）";
    EXPECT_EQ(CountSolidAtHeight(after, 107), 0U);
    EXPECT_EQ(CountSolidAtHeight(after, 105), 8U * 8U) << "落到地板顶面";
    EXPECT_EQ(CountSolidAtHeight(after, 106), 8U * 8U);
    EXPECT_EQ(CountFloatingSolid(after), 0U) << "区域内不得有「下方为空」的实心体素（判据 ②）";
}

// ㉑ 刚性回写**不做沉降**：同一场景换成岩 ⇒ 体素留在原位（109 / 110）、`settledVoxels == 0`（形状不变）。
TEST(VolumeCollapseLanding, RigidWritebackKeepsVoxelsInPlace) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 2U);  // 岩 = 刚性

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    ASSERT_TRUE(plan.units.front().rigidDebris);

    const CollapsePose pose { plan.units.front().centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };
    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);
    EXPECT_EQ(writeback.writtenVoxels, kSlabVoxels);
    EXPECT_EQ(writeback.settledVoxels, 0U) << "刚性不做接地沉降";
    EXPECT_EQ(writeback.removedFloatingVoxels, 0U);

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 8U * 8U) << "刚性：体素留在原位（形状不变）";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 8U * 8U);
}

// ㉒ 保留残骸的**世界 AABB**（`UnitWorldAabb`）：用于"某体积块碰撞体重建后唤醒相交残骸"，
//    盒**随姿态旋转**。T48 起光球命中改由物理引擎回答（真实凸包表面）⇒
//    原先的手工 OBB 判据 `LocalAabbContainsPoint` **已下线**（ADR 0018 决策三）。
TEST(VolumeCollapseLanding, UnitWorldAabbFollowsPose) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);

    const CollapseSpec spec;
    const CollapsePlan plan = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                                spec, kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    const vx::CollapseUnit& unit = plan.units.front();

    // 石板 8×8×2 ⇒ 局部 AABB ≈ x ∈ [−4, 4]、y ∈ [−1, 1]、z ∈ [−4, 4]（薄轴是 y）。
    EXPECT_FLOAT_EQ(unit.hullMinLocal.y, -1.0F);
    EXPECT_FLOAT_EQ(unit.hullMaxLocal.y, 1.0F);
    EXPECT_FLOAT_EQ(unit.hullMaxLocal.x - unit.hullMinLocal.x, 8.0F);

    const vx::CollapsePose identity { unit.centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };

    // `UnitWorldAabb`：单位姿 ≡ 质心 + 局部 AABB；绕 Z 转 45° 后**半长按旋转后的包围盒增长**。
    glm::dvec3 low(0.0);
    glm::dvec3 high(0.0);
    vx::UnitWorldAabb(unit, identity, low, high);
    EXPECT_NEAR(low.x - unit.centroid.x, static_cast<double>(unit.hullMinLocal.x), 1.0e-4);
    EXPECT_NEAR(high.y - unit.centroid.y, static_cast<double>(unit.hullMaxLocal.y), 1.0e-4);

    const vx::CollapsePose diagonal { unit.centroid,
                                      glm::angleAxis(glm::radians(45.0F), glm::vec3(0.0F, 0.0F, 1.0F)) };
    vx::UnitWorldAabb(unit, diagonal, low, high);
    const double expectedHalfY = (static_cast<double>(unit.hullMaxLocal.x) +
                                  static_cast<double>(unit.hullMaxLocal.y)) *
                                 std::cos(glm::radians(45.0)) ;
    EXPECT_NEAR(high.y - unit.centroid.y, expectedHalfY, 1.0e-3)
        << "45° 下世界 AABB 半长 = |max.x|·cos45 + |max.y|·sin45";
}

// ---------------------------------------------------------------------------
// T49 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策一：
// 支撑求解 **连通域化**（scope = 以被改动采样为起点的实心连通域，而不是固定窗口）
// ---------------------------------------------------------------------------

// ㉓ 判据①（BUG1 的回归判据）：**30 格长条只由两端立柱撑着**，两次爆破各炸一侧立柱的**根部**
//    ⇒ 长条**整段**失去支撑 ⇒ 区域内不得再有"下方为空"的实心体素。
//    固定窗口（被改动采样 ± (悬挑 + 1) 格）下，距任何破坏点都超过窗口的**中段从不进入任何一次求解**
//    ⇒ 永远悬空（这正是实测的 BUG1）；连通域化后中段也在域内 ⇒ 必须一并落下。
//    判据②（确定性）在本用例内一并核对：同一串输入在另一个同样的场景上跑，域与整体逐位相同。
TEST(VolumeCollapseConnectedDomain, LongBarLosesBothPillarsAndFallsEntirely) {
    const MapPreset preset = FlatPreset();
    // 悬挑上限取 15：30 格长条（两端立柱的净跨度 26 格）**两端都有立柱时确实稳定** —— 两端各 15 步的
    // 悬挑把全段覆盖；否则"前提"就不成立，这个用例也证明不了"是这次破坏造成的"。
    // 破坏一侧后只剩一端撑着（另一侧那截必须落）；**第二次破坏后**整段都必须落。
    CollapseSpec spec;
    spec.maxCantileverBlocks = 15.0F;
    const auto build = [&preset]() {
        return SlabScene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                         TerrainMaterialTable::Default(), BuildTwoPillarBarScene());
    };

    // 前置：两端立柱都在 ⇒ 长条有支撑，一个整体都不该有（否则本用例证明不了"是这次破坏造成的"）。
    {
        SlabScene          pristine = build();
        const CollapsePlan none     = vx::ApplyCollapse(
            pristine.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec, kMaxUnitsInTest);
        EXPECT_TRUE(none.units.empty()) << "前提：两端立柱都在时长条稳定";
    }

    // 左端爆破 → 抽取；右端爆破 → 剩下的中段必须整段落下。
    std::vector<BlockCoord> dirty;
    vx::VoxelBounds         leftBounds;
    vx::VoxelBounds         rightBounds;

    SlabScene scene = build();
    vx::Clock clock;
    (void)clock.Tick();
    ASSERT_TRUE(scene.volumes.CarveSphere(glm::dvec3(2.0, 106.0, 11.0), 3.0F, dirty, &leftBounds))
        << "第一次爆破必须真的炸到左立柱根部";
    const CollapsePlan first = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed { leftBounds }, spec, kMaxUnitsInTest);
    const double        firstMs = clock.Tick() * 1000.0;
    EXPECT_TRUE(first.domainNarrowed) << "T49：按连通域收窄（不是回退固定窗口）";
    ASSERT_FALSE(first.units.empty()) << "左端支点被炸断 ⇒ 长条该失去支撑";

    ASSERT_TRUE(scene.volumes.CarveSphere(glm::dvec3(30.0, 106.0, 11.0), 3.0F, dirty, &rightBounds));
    const CollapsePlan second = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed { rightBounds }, spec, kMaxUnitsInTest);
    const double        secondMs = clock.Tick() * 1000.0;
    EXPECT_TRUE(second.domainNarrowed);
    ASSERT_FALSE(second.units.empty()) << "右端支点也被炸断 ⇒ 剩下的中段必须落（BUG1 的判据）";

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountFloatingSolid(after), 0U)
        << "两次爆破各炸一侧支点后，区域内**不得**再有'下方为空'的实心体素（悬空的中段 = BUG1）";
    std::printf("[T49 基准] 长条两次爆破（连通域收窄）：域 %zu / %zu 体素、窗口 %zu 采样；"
                "主线程上本次最贵一步 = 支撑求解，%.2f ms + %.2f ms（debug）\n",
                first.domainVoxels, second.domainVoxels, second.regionSamples, firstMs, secondMs);
    std::fflush(stdout);

    // 判据②：洪泛**确定性**（红线 7）—— 同一串输入在另一个同样构造的场景上跑，域与整体逐位相同。
    SlabScene               twin = build();
    std::vector<BlockCoord> twinDirty;
    vx::VoxelBounds         twinLeft;
    vx::VoxelBounds         twinRight;
    ASSERT_TRUE(twin.volumes.CarveSphere(glm::dvec3(2.0, 106.0, 11.0), 3.0F, twinDirty, &twinLeft));
    (void)vx::ApplyCollapse(twin.volumes, vx::CollapseSeed { twinLeft }, spec, kMaxUnitsInTest);
    ASSERT_TRUE(twin.volumes.CarveSphere(glm::dvec3(30.0, 106.0, 11.0), 3.0F, twinDirty, &twinRight));
    const CollapsePlan twinSecond = vx::ApplyCollapse(twin.volumes, vx::CollapseSeed { twinRight }, spec, kMaxUnitsInTest);
    EXPECT_EQ(twinSecond.domainVoxels, second.domainVoxels) << "同输入 ⇒ 同一连通域";
    ASSERT_EQ(twinSecond.units.size(), second.units.size());
    for (std::size_t i = 0; i < second.units.size(); ++i) {
        EXPECT_EQ(twinSecond.units[i].voxels.size(), second.units[i].voxels.size());
        EXPECT_TRUE(twinSecond.units[i].voxels.front() == second.units[i].voxels.front());
        EXPECT_TRUE(twinSecond.units[i].voxels.back() == second.units[i].voxels.back());
        EXPECT_NEAR(twinSecond.units[i].centroid.x, second.units[i].centroid.x, 1.0e-9);
    }
}

// ㉔ 判据③：连通域**超出上限** ⇒ **告警 + 保守回退固定窗口**（ADR 0018 决策一登记的显式例外）。
//    窗口的采样数 = `sizeX × sizeY × sizeZ`；扩张**只发生在触界的那些侧**（细长结构因此很便宜），
//    所以要让"超界"真的发生，就得让**两个方向**都很宽。这里用一条 512 格长的悬空长条 +
//    一个**刻意的宽 z 种子**（现实中对应"一次波及很宽的破坏"）把两个方向的宽度都做出来 ——
//    16 个块的场景即可走到这条例外，不必构造几百个块的地图。
//    可判定判据 = `plan.domainNarrowed == false`（日志里同时有一条 WARN）；耗时打进输出供人工核对**不冻结**。
TEST(VolumeCollapseConnectedDomain, OversizedDomainFallsBackToFixedWindow) {
    constexpr int      kBlocksX = 16;  // 16 × 32 = 512 格
    const MapPreset    preset   = FlatPreset();
    const CollapseSpec spec;

    TerrainWorld world(preset.seed, TerrainMaterialTable::Default());
    world.SetMapPreset(preset);
    world.LoadTile(0, 0);
    DigRegionTable regions = DigRegionTable::FromRegions(
        { MakeRegion(BlockCoord { 0, 3, 0 }, BlockCoord { kBlocksX - 1, 3, 0 }) });
    DigVolumeWorld volumes(world, regions);
    volumes.InitFromHeightField();
    volumes.WriteDensityRegion(BuildLongSprawlSlabScene(kBlocksX));

    // 种子 = "被改动采样"的 AABB：x 很窄、**z 很宽**（490 格）、y 覆盖地表块。
    vx::CollapseSeed seed;
    seed.bounds = vx::VoxelBounds { 0, 105, -230, 20, 112, 260 };

    vx::Clock clock;
    (void)clock.Tick();
    const CollapsePlan plan = vx::ApplyCollapse(volumes, seed, spec, kMaxUnitsInTest);
    const double       ms   = clock.Tick() * 1000.0;

    EXPECT_FALSE(plan.domainNarrowed)
        << "连通域超出上限 ⇒ 必须走'告警 + 保守回退固定窗口'（否则就是偷偷把上限放开了）";
    EXPECT_GT(plan.regionSamples, 0U);
    EXPECT_LE(plan.regionSamples, 8U * 1024U * 1024U) << "回退后的窗口仍必须在上限之内（不冻结画面的前提）";
    EXPECT_FALSE(plan.units.empty()) << "回退后仍按固定窗口抽出失去支撑的部分（'这次没倒'不该变成'什么都不做'）";
    std::printf("[T49 基准] 连通域超上限 ⇒ 回退固定窗口：窗口 %zu 采样、域 %zu 体素、整体 %zu 个；"
                "主线程上本次最贵一步 = 支撑求解，%.2f ms（debug）\n",
                plan.regionSamples, plan.domainVoxels, plan.units.size(), ms);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// T50 / ADR 0018 决策二：破坏时**不切换表示**（在碎块自身补丁上雕刻 + 同一份 Surface Nets 重网格）
// ---------------------------------------------------------------------------

// ㉕ 判据①（可证伪的不变量）：在补丁上雕刻后，**未被触及的区域顶点逐位不变** ——
//    两次网格化的顶点差集（两个方向）都必须落在球内（+ 一个 cell 的余量）。
//    这条把"岩石始终不变形"从"靠自觉"变成可测的判据。
TEST(VolumeCollapseCarve, CarveKeepsUntouchedVerticesBitIdentical) {
    const MapPreset            preset    = FlatPreset();
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    const CollapseSpec         spec;

    SlabScene scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    // **T52 起岩不可破坏**（`indestructible = true`）⇒ 在岩上雕刻是**零改动**（另有用例专门钉这条），
    // 故这里用**土**（可破坏）验证"雕刻 + 重网格"这条数学：任一可破坏材质的补丁口径完全相同。
    MarkSlabMaterial(scene.volumes, 1U);
    CollapsePlan plan = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                          spec, kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    vx::CollapseUnit& unit = plan.units.front();
    ASSERT_EQ(unit.voxels.size(), kSlabVoxels);
    ASSERT_FALSE(materials.Layer(1).indestructible) << "前提：土可破坏（不可破坏的材质根本挖不动）";

    const auto vertexSet = [](const vx::MeshData& mesh) {
        std::vector<std::array<float, 3>> out;
        out.reserve(mesh.vertices.size());
        for (const vx::MeshVertex& vertex : mesh.vertices) {
            out.push_back({ vertex.position[0], vertex.position[1], vertex.position[2] });
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    const vx::MeshData               before = vx::BuildCollapseUnitMesh(unit);
    const std::vector<std::array<float, 3>> beforeVerts = vertexSet(before);
    ASSERT_FALSE(beforeVerts.empty());

    // 球心放在石板的**一个上角**（世界坐标；网格顶点是"相对质心"的局部坐标，下面换算回来比较）。
    const double     radius = 1.0;
    const glm::dvec3 center(unit.centroid.x + 3.0, unit.centroid.y + 0.5, unit.centroid.z + 3.0);
    ASSERT_TRUE(vx::CarveCollapseUnitPatch(unit, center, static_cast<float>(radius), materials))
        << "前置：球必须真的碰到这块碎块（否则本用例证明不了什么）";

    const vx::MeshData                      after = vx::BuildCollapseUnitMesh(unit);
    const std::vector<std::array<float, 3>> afterVerts = vertexSet(after);
    ASSERT_FALSE(afterVerts.empty()) << "挖一个小角不该把整块挖光";

    std::vector<std::array<float, 3>> onlyAfter;
    std::vector<std::array<float, 3>> onlyBefore;
    std::set_difference(afterVerts.begin(), afterVerts.end(), beforeVerts.begin(), beforeVerts.end(),
                        std::back_inserter(onlyAfter));
    std::set_difference(beforeVerts.begin(), beforeVerts.end(), afterVerts.begin(), afterVerts.end(),
                        std::back_inserter(onlyBefore));

    // 顶点是"相对质心"的局部坐标 ⇒ 加回质心即世界坐标。
    const auto insideBall = [&](const std::array<float, 3>& local) {
        const glm::dvec3 world(unit.centroid.x + static_cast<double>(local[0]),
                               unit.centroid.y + static_cast<double>(local[1]),
                               unit.centroid.z + static_cast<double>(local[2]));
        // 球内 + 过渡带 + **一个 cell 的对角**（顶点属于 cell，cell 的任一采样被改就可能带动它）。
        return glm::distance(world, center) <= radius + vx::kCarveSdfBandBlocks + 2.0;
    };
    ASSERT_FALSE(onlyBefore.empty()) << "雕刻必须真的改动了网格（否则判据没有区分度）";
    for (const std::array<float, 3>& vertex : onlyAfter) {
        EXPECT_TRUE(insideBall(vertex)) << "新增顶点只允许出现在球附近";
    }
    for (const std::array<float, 3>& vertex : onlyBefore) {
        EXPECT_TRUE(insideBall(vertex))
            << "消失顶点只允许出现在球附近 ⇒ **未被触及的区域顶点逐位不变**（T50 的不变量）";
    }
}

// ㉖ 判据②：由**补丁**重算（`RefreshCollapseUnitFromPatch`）必须
//    ① **质心逐位不变**（局部坐标系不变 ⇒ 刚体与网格原地不动，碎块不会因为被雕刻而"跳一下"）；
//    ② 体素清单 = 补丁里"仍实心且属于本整体"的采样 —— 有减少，且被移除的只落在球附近；
//    ③ 质量随剩余体素减小（仍是逐体素累加）；凸包点集仍可用（≥ 4 点）。
TEST(VolumeCollapseCarve, RefreshKeepsCentroidAndShrinksVoxelsToTheCarvedPatch) {
    const MapPreset            preset    = FlatPreset();
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    const CollapseSpec         spec;

    SlabScene scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 1U);  // 土（T52 起岩不可破坏 ⇒ 雕刻用例改用可破坏材质；见上一个用例）
    CollapsePlan plan = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                          spec, kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    vx::CollapseUnit& unit = plan.units.front();

    const glm::dvec3   centroidBefore = unit.centroid;
    const float        massBefore     = unit.mass;
    const std::size_t  voxelsBefore   = unit.voxels.size();

    const double     radius = 1.0;
    const glm::dvec3 center(unit.centroid.x + 3.0, unit.centroid.y + 0.5, unit.centroid.z + 3.0);
    ASSERT_TRUE(vx::CarveCollapseUnitPatch(unit, center, static_cast<float>(radius), materials));
    ASSERT_TRUE(vx::RefreshCollapseUnitFromPatch(unit, materials));

    EXPECT_EQ(unit.centroid.x, centroidBefore.x) << "质心必须**逐位不变**（局部坐标系不变）";
    EXPECT_EQ(unit.centroid.y, centroidBefore.y);
    EXPECT_EQ(unit.centroid.z, centroidBefore.z);
    EXPECT_LT(unit.voxels.size(), voxelsBefore) << "确有体素被挖掉";
    EXPECT_LT(unit.mass, massBefore) << "质量随剩余体素减小（逐体素累加）";
    EXPECT_GE(unit.hullPoints.size() / 3U, 4U) << "剩余体素仍要能构成凸包（否则调用方会走删除兜底）";
    EXPECT_FALSE(unit.rigidDebris) << "土 ⇒ 仍是散体（落地后回写融合；本用例只验证雕刻数学，与落地口径无关）";

    // 被移除的体素只允许落在球附近（球半径 + 一个格 + 半个格的对角余量）。
    std::map<std::array<int, 3>, int> remaining;
    for (const vx::CollapseUnit::Voxel& voxel : unit.voxels) {
        remaining[{ voxel.x, voxel.y, voxel.z }] = 1;
    }
    for (int x = 4; x < 12; ++x) {
        for (int z = 4; z < 12; ++z) {
            for (int y = 109; y < 111; ++y) {
                if (remaining.count({ x, y, z }) != 0U) {
                    continue;
                }
                const glm::dvec3 voxelCenter(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5,
                                             static_cast<double>(z) + 0.5);
                EXPECT_LE(glm::distance(voxelCenter, center), radius + 2.0)
                    << "被挖掉的体素必须落在球附近（x=" << x << ", y=" << y << ", z=" << z << "）";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// T52（2026-09-28，所有者指定）：**岩 = 完全不可破坏**（但仍挡光球、仍会倒塌）
// ---------------------------------------------------------------------------

// ㉚ 判据③：**岩石碎块不再被雕刻** —— 岩现在 `indestructible = true`，在它的补丁上挖球必须**零改动**：
//    网格顶点（含顺序）**逐位相同**，且不走进"重算"那一步。于是"光球打中岩块"只会被挡住，
//    不会把岩石打碎/变形（这正是所有者要的"完全无法击毁"）；而**命中判定与物理碰撞完全不变**
//    （`CarveBody` 只会在"零改动"时返回 false ⇒ 光球按常规引爆，岩体纹丝不动）。
TEST(VolumeCollapseCarve, IndestructibleRockDebrisIsNotCarvedAtAll) {
    const MapPreset            preset    = FlatPreset();
    const TerrainMaterialTable materials = TerrainMaterialTable::Default();
    const CollapseSpec         spec;

    SlabScene scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true);
    MarkSlabMaterial(scene.volumes, 2U);  // 岩
    CollapsePlan plan = vx::ApplyCollapse(scene.volumes, vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax),
                                          spec, kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    vx::CollapseUnit& unit = plan.units.front();
    ASSERT_TRUE(materials.Layer(2).indestructible) << "前提：岩不可破坏（T52）";
    ASSERT_TRUE(unit.rigidDebris) << "前提：岩是刚性碎块（保留几何体）";

    const vx::MeshData before      = vx::BuildCollapseUnitMesh(unit);
    const std::size_t  voxelBefore = unit.voxels.size();
    const glm::dvec3   center(unit.centroid.x + 3.0, unit.centroid.y + 0.5, unit.centroid.z + 3.0);
    ASSERT_FALSE(before.vertices.empty());

    EXPECT_FALSE(vx::CarveCollapseUnitPatch(unit, center, 1.0F, materials))
        << "岩不可破坏 ⇒ 球心就在岩里也必须**零改动**";

    const vx::MeshData after = vx::BuildCollapseUnitMesh(unit);
    ASSERT_EQ(after.vertices.size(), before.vertices.size());
    for (std::size_t i = 0; i < before.vertices.size(); ++i) {
        EXPECT_EQ(after.vertices[i].position[0], before.vertices[i].position[0]) << "顶点必须逐位相同（第 " << i << " 个）";
        EXPECT_EQ(after.vertices[i].position[1], before.vertices[i].position[1]);
        EXPECT_EQ(after.vertices[i].position[2], before.vertices[i].position[2]);
    }
    EXPECT_EQ(unit.voxels.size(), voxelBefore) << "体素清单不得变化（没走进 Refresh）";
}

// ---------------------------------------------------------------------------
// T51（2026-09-28，所有者实测）：**降不到支撑的散体一律清除**
// ---------------------------------------------------------------------------

// ㉙ 判据：散体落在**下方 32 格内没有任何支撑**的地带（深井场景）⇒ 接地沉降走满上限也接不到地
//    ⇒ 必须走"仍悬空 ⇒ 清除"这条路。旧实现只计数 + 告警、体素仍留在空中（"静默的悬空泥土"）；
//    现在回写后区域内**不得**再有"下方为空"的实心体素（与 T46 的"泥土不能悬空"同一口径）。
TEST(VolumeCollapseLanding, FloatingGranularVoxelsAreRemovedInsteadOfBeingLeftInTheAir) {
    const MapPreset preset = FlatPreset();
    SlabScene       scene(preset, DigRegionTable::FromRegions({ MakeRegion(kSceneBlockMin, kSceneBlockMax) }), true,
                          TerrainMaterialTable::Default(), BuildDeepPitScene());
    MarkSlabMaterial(scene.volumes, 1U);  // 土 = 散体（只有散体才做接地沉降）

    const CollapseSpec spec;
    CollapsePlan       plan = vx::ApplyCollapse(scene.volumes,
                                                vx::CollapseSeed::FromBlocks(kSceneBlockMax, kSceneBlockMax), spec,
                                                kMaxUnitsInTest);
    ASSERT_EQ(plan.units.size(), 1U);
    ASSERT_FALSE(plan.units.front().rigidDebris) << "前提：土 = 散体";

    const CollapsePose pose { plan.units.front().centroid, glm::quat(1.0F, 0.0F, 0.0F, 0.0F) };
    const vx::CollapseWriteback writeback = vx::WritebackCollapseUnit(scene.volumes, plan.units.front(), pose);

    EXPECT_EQ(writeback.removedFloatingVoxels, kSlabVoxels)
        << "下方 32 格内没有支撑 ⇒ 每个体素都降不到地 ⇒ **全部清除**（不许留在空中）";
    EXPECT_EQ(writeback.writtenVoxels, 0U) << "被清除的体素不算写回成功";
    EXPECT_EQ(writeback.droppedVoxels, 0U) << "落点本身在体积块覆盖内 ⇒ 不是「丢弃」，是「悬空被清除」";

    const DensityRegion after = ReadScene(scene.volumes);
    EXPECT_EQ(CountSolidAtHeight(after, 109), 0U) << "原处的石板必须已被抽出";
    EXPECT_EQ(CountSolidAtHeight(after, 110), 0U);
    EXPECT_EQ(CountFloatingSolid(after), 0U)
        << "回写后区域内**不得**再有'下方为空'的实心体素（T51 的判据 = T46 的同一条不变量）";
}
