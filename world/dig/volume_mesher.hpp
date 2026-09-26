#pragma once

#include "render/mesh_renderer.hpp"

#include <cstdint>

namespace vx {

/// 体积块每边的**体素**数（ADR 0008：32³ 体素）。
inline constexpr int kVolumeBlockSize = 32;

/// 体积块每边的**采样**数：多一层共享边界采样（与地表 tile 的 64 列 / 65 采样同构）。
///
/// 相邻体积块在同一世界采样面上取到**同一份数据**（见 `IVolumeSampler` 的越界约定），
/// 因此块间接缝处顶点逐位相等（ADR 0004 硬约束 3：体积按固定网格对齐、共享边界采样）。
inline constexpr int kVolumeSampleCount = kVolumeBlockSize + 1;

/// 密度定点口径（ADR 0008「`int8` 距离场，±1 格范围近似」的落地实现）：
/// 1 格 = `kDensityUnitsPerBlock` 个单位，值域钳制到 `[-127, 127]` ⇒ 距表面超过 1 格处**饱和**。
/// 语义：`d < 0` 实心、`d > 0` 空、`d == 0` 为表面（ADR 0007）。
inline constexpr float kDensityUnitsPerBlock = 127.0F;
inline constexpr int   kDensityMin           = -127;
inline constexpr int   kDensityMax           = 127;

/// 体积块的整数坐标（单位：块；块边长 = `kVolumeBlockSize` 格）。
/// 世界定位用整数承担（红线 6）：块内顶点只存**局部**坐标。
struct BlockCoord {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] friend constexpr bool operator==(const BlockCoord& lhs, const BlockCoord& rhs) noexcept {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }

    [[nodiscard]] friend constexpr bool operator<(const BlockCoord& lhs, const BlockCoord& rhs) noexcept {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        if (lhs.y != rhs.y) {
            return lhs.y < rhs.y;
        }
        return lhs.z < rhs.z;
    }
};

/// 块索引 → 该块原点对应的世界坐标（格）。块是**固定网格对齐**的，故这是纯整数乘法。
[[nodiscard]] constexpr int BlockOriginBlocks(int blockIndex) noexcept {
    return blockIndex * kVolumeBlockSize;
}

/// 材质槽位的"未指定"值（ADR 0014）：与 `MeshVertex::material` 的 `kNoMaterialOverride` 对应。
inline constexpr std::uint8_t kNoMaterialSlot = 0xFFU;

/// 密度采样器：给定**块内采样索引**返回密度（`int8` 语义的浮点表示）。
///
/// 索引**可以越界**（`-1` 或 `kVolumeBlockSize`）：实现必须给出与相邻块 / 区域外回退一致的同一份
/// 密度值——网格化只需多取一层外围采样，就能在块边界处生成与邻块**逐位相同**的顶点。
/// 这是"块间无接缝"的唯一机制，不得用"边界处截断"代替。
class IVolumeSampler {
public:
    virtual ~IVolumeSampler() = default;

    IVolumeSampler(const IVolumeSampler&) = delete;
    IVolumeSampler& operator=(const IVolumeSampler&) = delete;
    IVolumeSampler(IVolumeSampler&&) = delete;
    IVolumeSampler& operator=(IVolumeSampler&&) = delete;

    [[nodiscard]] virtual float Sample(int i, int j, int k) const = 0;

    /// 该采样点的**材质槽位**（ADR 0014）；`kNoMaterialSlot` = 未指定（片元按高度 / 坡度算）。
    ///
    /// 越界约定与 `Sample` 相同（必须给出与邻块一致的材质）。默认返回 `kNoMaterialSlot` ——
    /// 只关心密度、不关心材质的调用方（测试桩、离线工具）因此无需改动。
    [[nodiscard]] virtual std::uint8_t SampleMaterial(int i, int j, int k) const {
        static_cast<void>(i);
        static_cast<void>(j);
        static_cast<void>(k);
        return kNoMaterialSlot;
    }

protected:
    IVolumeSampler() = default;
};

/// Naive Surface Nets 等值面提取（ADR 0007）：把一个体积块网格化为平滑曲面。
///
/// 为什么是 Surface Nets：**每个 cell 至多产生 1 个顶点**，顶点位置由该 cell 内各棱交点**平均**得到、
/// 随密度值**连续变化** ⇒ 挖除时表面不会突然跳变（"平滑手感"的直接来源），且实现最简、
/// 块间共享边界采样即天然无接缝。
///
/// 输出约定：
/// - 顶点位置是**块内局部坐标**（格，范围约 `-1 ~ 32`：为块边界棱的四边形额外算了一格"围裙"），
///   世界定位由块坐标承担（红线 6）；
/// - 法线由**密度场梯度**给出（八角逐轴差分，比面法线平滑），指向**空侧**（密度增大的方向）；
/// - 三角形绕序使**正面朝向空侧**（与 `MeshRenderer` 的 `FRONTFACE_COUNTER_CLOCKWISE` + 背面剔除一致）；
/// - 每条网格棱只发射一次四边形（由该棱"u/v 下侧"的 cell 发射），故**不会跨块重复**——
///   块边界外的 cell 只被用来取顶点，不发射四边形。
///
/// 前置条件：`sampler` 的生命周期覆盖本调用；越界采样索引必须给出与邻块一致的密度。
[[nodiscard]] MeshData BuildVolumeMesh(const IVolumeSampler& sampler);

}  // namespace vx
