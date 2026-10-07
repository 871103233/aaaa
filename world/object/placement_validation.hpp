#pragma once

#include "object/placement_snap.hpp"  // PlacementFootprint

#include <cmath>

namespace vx {

/// **放置合法性：2D（XZ）足迹重叠检测**（V0.11 / I2；[ADR 0038](../../docs/adr/0038-construction-editor-and-runtime-separation.md) 决策四的"合法性"）
/// —— **纯函数、header-only**：编辑器与运行时"游戏内建造"**共用**（与 UI / GPU 无关）。
///
/// 口径（**可判定**）：两个**有向矩形**（中心 + 半尺寸 + 绕 Y 朝向）在 XZ 平面上是否**重叠**。
/// 用**分离轴测试（SAT）**：取两矩形各自 2 条轴共 4 条辅助轴，任一轴上投影"分离" ⇒ **不重叠**；
/// 4 条轴上都重叠 ⇒ **重叠**。**边界接触（间隙 = 0）不算重叠**（须 `穿透 > epsilon`）。
///
/// 业界参照（点名）：UE5 *Place Actors* 的"**碰撞即红**"、Cities: Skylines 的"**不可建造地块**"判据、Valheim 建造的占用校验
/// —— 共同口径 = **重叠 ⇒ 拒绝并给可见提示**。
[[nodiscard]] inline bool FootprintsOverlap2D(const PlacementFootprint& a, const PlacementFootprint& b,
                                              double epsilon = 1e-6) noexcept {
    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const double     ca = std::cos(a.yawDegrees * kDegreesToRadians);
    const double     sa = std::sin(a.yawDegrees * kDegreesToRadians);
    const double     cb = std::cos(b.yawDegrees * kDegreesToRadians);
    const double     sb = std::sin(b.yawDegrees * kDegreesToRadians);

    // 局部轴 → 世界（与 `ObjectSupportProbes` / `SnapPlacementToNeighbor` 同一"绕 +Y"约定）。
    const double axes[4][2] = {
        { ca, -sa }, { sa, ca },  // a 的 X 轴 / Z 轴
        { cb, -sb }, { sb, cb },  // b 的 X 轴 / Z 轴
    };
    const double dx = b.x - a.x;
    const double dz = b.z - a.z;

    for (const auto& u : axes) {
        const double ux = u[0];
        const double uz = u[1];
        const double ra = std::fabs(a.halfX * (ca * ux - sa * uz)) + std::fabs(a.halfZ * (sa * ux + ca * uz));
        const double rb = std::fabs(b.halfX * (cb * ux - sb * uz)) + std::fabs(b.halfZ * (sb * ux + cb * uz));
        const double centerDistance = std::fabs(dx * ux + dz * uz);
        if (centerDistance >= ra + rb - epsilon) {
            return false;  // 该轴上分离（含相切）⇒ 不重叠
        }
    }
    return true;
}

}  // namespace vx
