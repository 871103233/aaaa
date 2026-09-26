#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

struct TerrainTile;

/// 地图编辑模式（T11）。
///
/// 三种模式都是**水平矩形**作用域（`[minX, maxX] × [minZ, maxZ]`，世界列坐标，闭区间）：
enum class MapEditMode : std::uint8_t {
    Flatten,  ///< 把区域内所有列**强制**为给定绝对高度（格）
    Raise,    ///< 在区域内所有列的当前高度上**叠加**给定增量
    Carve,    ///< 从区域内所有列的当前高度上**减去**给定增量
};

/// 一条水平矩形地形编辑。矩形按世界列坐标给出，与笔刷 / 高度场同口径。
struct MapEdit {
    std::string name;                  ///< 区域可读名（仅用于日志与排错，不参与计算）
    MapEditMode mode = MapEditMode::Flatten;
    int         minX = 0;              ///< 矩形最小世界列 X（含）
    int         maxX = -1;             ///< 矩形最大世界列 X（含）
    int         minZ = 0;              ///< 矩形最小世界列 Z（含）
    int         maxZ = -1;             ///< 矩形最大世界列 Z（含）
    int         heightUnits = 0;       ///< 定点高度（1/16 格）：Flatten=绝对高度，Raise/Carve=非负增量
};

/// 预设固定地图（T11）：种子 + 范围 + 出生点 + 一串地形编辑。
///
/// 语义：
///   - 生成顺序固定为「**噪声先行、编辑覆盖其上**」：tile 先由 `TerrainNoiseGenerator` 填充，
///     再按 `edits` 的**文件顺序**逐条应用；同一文件 + 同一种子 ⇒ 同一世界（红线 7：纯函数）。
///   - `tileRadiusX` / `tileRadiusZ` 决定地图范围：tile 坐标覆盖 `[-r, r]`（两轴各自），
///     即世界列覆盖 `[-r * 64, r * 64]`（含 tile 的共享边界列）。
///   - `spawnX` / `spawnZ` 为**世界列坐标（格）**；角色的竖直位置由游戏层按地表高度 + 余量求解。
///
/// 加载失败（文件缺失 / 语法错 / 字段缺失 / 取值非法）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值（与 `TerrainMaterialTable::LoadFromFile` 同一口径，见 ADR 0005）。
struct MapPreset {
    /// 当前文件格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    int                  schemaVersion = kSchemaVersion;
    std::string          name;                 ///< 显示名（日志用）
    std::uint64_t        seed = 0;             ///< 世界种子
    int                  tileRadiusX = 1;      ///< X 方向 tile 半径（含 0 号 tile）
    int                  tileRadiusZ = 1;      ///< Z 方向 tile 半径（含 0 号 tile）
    double               spawnX = 0.0;         ///< 出生列 X（格）
    double               spawnZ = 0.0;         ///< 出生列 Z（格）
    std::vector<MapEdit> edits;                ///< 地形编辑，按文件顺序应用

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    /// 前置条件：`path` 指向待加载的地图文件。
    [[nodiscard]] static MapPreset LoadFromFile(const std::filesystem::path& path);
};

/// 把预设编辑应用到**单个 tile**：对 tile 覆盖的每个世界列按矩形判定并修改高度，
/// 最后把结果钳制到世界垂直范围 `0 ~ 512` 格（ADR 0008）。
///
/// 为什么按世界列判定：共享边界列由世界列坐标唯一决定，因此相邻 tile 在同一列上得到
/// **完全相同**的结果，拼接处逐位相等、无裂缝（红线 12 的编辑层对应做法）。
///
/// 纯函数（只依赖 `edits` 与 tile 自身高度）；`edits` 为空时不做任何事。
/// 前置条件：`tile` 已由噪声生成填充（`GenerateTerrainTile`）。
void ApplyMapEditsToTile(const std::vector<MapEdit>& edits, TerrainTile& tile) noexcept;

}  // namespace vx
