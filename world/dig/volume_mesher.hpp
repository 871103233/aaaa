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

/// 同一套 Surface Nets（ADR 0007）在**任意尺寸区域**上的入口（T42）：`size*` 是该区域的**体素数**
/// （采样数 = `size* + 1`，两侧各多取一圈用于围裙）。输出约定与 `BuildVolumeMesh` **逐字相同**
/// （顶点为区域局部坐标、法线 = 密度梯度、材质 = 实体侧众数）。
///
/// 为什么需要它：倒塌整体原先用"逐体素方块面 + 面法线"自造网格，与地形 / 洞的等值面口径不同 ⇒
/// 掉落中棱角明显、落地后又变回平滑（项目所有者实测）。改用本入口后，切下来的那一块**就是它原本的
/// 那一片等值面**（顶点与原地形网格在重叠处逐位一致），三态（静止 / 运动中 / 落定）外观连续。
[[nodiscard]] MeshData BuildRegionMesh(const IVolumeSampler& sampler, int sizeX, int sizeY, int sizeZ);

/// **闭合自检**（T47 / [ADR 0018](../../docs/adr/0018-structural-support-and-representation-preserving-destruction.md) 决策五）：
/// 统计网格里"**不是恰被 2 个三角形共用**"的无向边条数。
///
/// 语义：闭合（水密）曲面里每条无向边恰被 2 个三角形共用 ⇒ 返回 0；有洞时洞的边界边只被用到 1 次 ⇒ > 0
/// （被 3 个及以上共用 = 非流形退化，同样计入，便于发现坏输出）。
/// 用途是**取证**：整体倒塌的**外观网格**若返回非 0，玩家就会看到"某个面没有颜色、直接透明"
/// （片元 `o_color.a` 恒为 1 ⇒ 只可能是缺面）。纯函数、无 GPU 依赖 ⇒ 生产代码与单测**共用同一实现**，
/// 口径不会漂移。
[[nodiscard]] std::size_t CountBoundaryEdges(const MeshData& mesh);

/// 统计 `mesh` 的**退化三角形**数（零面积 / 含重复顶点）。
///
/// 为什么需要（T55）：退化三角形面积为零 ⇒ 光栅化**不产生任何片元** ⇒ 在屏幕上表现为
/// **这一小片"透明"、看得见后面**（"爆炸破坏处偶发透明面"的候选成因之一）。
/// 它与 `CountBoundaryEdges` 互补：后者数"面缺了留下的洞"，前者数"画不出来的面"。
/// 判据与 `volume_mesher_test.cpp` 的既有断言同口径（叉积长度 ≤ 1e-6 视为退化）。
[[nodiscard]] std::size_t CountDegenerateTriangles(const MeshData& mesh);

}  // namespace vx
