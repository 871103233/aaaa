#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vx {

/// **放置吸附与对齐**（V0.11 / [ADR 0038](../../docs/adr/0038-construction-editor-and-runtime-separation.md) 决策四的"吸附"）
/// —— **纯函数、header-only**：编辑器与运行时"游戏内建造"**共用**同一套口径（与 UI / GPU 无关）。
///
/// 口径（**可判定**）：
///   - **平移吸附**：把落点世界坐标吸附到 `translateBlocks` 的**整数倍**（网格吸附）；
///   - **旋转吸附**：把朝向（度）吸附到 `yawDegrees` 的**整数倍**，并归一化到 `[0, 360)`；
///   - **关闭语义**：`step <= 0` ⇒ **不吸附**（平移原样返回、朝向仅做既有归一化）—— 用于 A/B 对照与"引入前逐位一致"；
///   - **非法输入**（`NaN`）⇒ 不产生 `NaN`（平移原样返回、朝向按 0 处理）。
///
/// 业界参照（点名）：UE5 **Grid / Socket / Vertex snapping**、Unity **ProGrids / ProBuilder**、Valheim 建造吸附
/// —— 共同口径 = "把落点 / 朝向量化到模数"。
struct PlacementSnapSettings {
    double translateBlocks      = 0.25;  ///< 世界网格吸附步长（格，**缺省 0.25**）；`<= 0` = 关闭
    double yawDegrees           = 1.0;   ///< 旋转吸附步长（度）；`<= 0` = 关闭。**V0.11（2026-10-08）：缺省 15 → 1**（与旋转离散步一致，支持 1° 精细调整）
    double neighborRadiusBlocks = 2.0;   ///< **邻居优先吸附**的搜索半径（格）；`<= 0` = 关闭（恒走世界网格）
};

/// 把 `value` 吸附到 `step` 的整数倍（**就近取整**）。`step <= 0` 或 `value` 为 `NaN` ⇒ **原样返回**。
[[nodiscard]] inline double SnapToStep(double value, double step) noexcept {
    if (!(step > 0.0) || std::isnan(value)) {
        return value;
    }
    return std::round(value / step) * step;
}

/// 把朝向（度）吸附到 `stepDegrees` 的整数倍，并归一化到 `[0, 360)`。
/// `stepDegrees <= 0` ⇒ 不吸附（**仅归一化**，与 `game/main.cpp` 既有的 `fmod(v + 360, 360)` 口径一致）。
[[nodiscard]] inline double SnapYawDegrees(double yawDegrees, double stepDegrees) noexcept {
    double yaw = std::isnan(yawDegrees) ? 0.0 : yawDegrees;
    if (stepDegrees > 0.0) {
        yaw = std::round(yaw / stepDegrees) * stepDegrees;
    }
    yaw = std::fmod(yaw, 360.0);
    if (yaw < 0.0) {
        yaw += 360.0;
    }
    return yaw;
}

/// 一个**邻居的底面足迹**（用于"邻居优先吸附"）：中心 `(x, z)`、半尺寸、绕 Y 的朝向。
/// 调用方负责把"可吸附的物件 / 建筑"投影成足迹（物件用 `ObjectType::halfExtentX/Z` 与 `yawDegrees`；
/// 建筑用其 footprint 的轴对齐包围盒、`yawDegrees = 0`）。
struct PlacementFootprint {
    double x          = 0.0;
    double z          = 0.0;
    double halfX      = 0.0;
    double halfZ      = 0.0;
    double yawDegrees = 0.0;
};

/// **邻居优先吸附**（V0.11 / I1c）：在 `searchRadiusBlocks` 内找**最近**的邻居（按到其足迹的距离），
/// 把落点**贴到该邻居最近的那一面**（在**邻居的局部坐标系**内：贴面 + 另一轴按 `gridStep` 量化），
/// 写出吸附后的世界坐标并返回 `true`；**找不到**（半径 ≤ 0 / 列表空 / 都在半径外）⇒ 返回 `false`
/// —— 调用方据此**退回世界网格**吸附。
///
/// 简化（如实登记）：`selfHalfX/selfHalfZ` 按**与邻居同朝向**理解（朝向由调用方另行决定）；**本函数只改位置、不改朝向**。
[[nodiscard]] inline bool SnapPlacementToNeighbor(double pointX, double pointZ, double selfHalfX,
                                                  double selfHalfZ,
                                                  const std::vector<PlacementFootprint>& neighbors,
                                                  double searchRadiusBlocks, double gridStep, double& outX,
                                                  double& outZ) noexcept {
    if (!(searchRadiusBlocks > 0.0) || neighbors.empty()) {
        return false;
    }
    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const auto       quantize          = [](double value, double step) noexcept {
        return (step > 0.0) ? std::round(value / step) * step : value;
    };

    // ① 找最近邻居：点到其足迹（有向矩形）的距离（局部夹到盒内后取长度；在盒内 ⇒ 0）。
    std::size_t bestIndex = neighbors.size();
    double      bestDist  = searchRadiusBlocks;
    for (std::size_t i = 0; i < neighbors.size(); ++i) {
        const PlacementFootprint& n = neighbors[i];
        const double c  = std::cos(n.yawDegrees * kDegreesToRadians);
        const double s  = std::sin(n.yawDegrees * kDegreesToRadians);
        const double dx = pointX - n.x;
        const double dz = pointZ - n.z;
        const double lx = c * dx - s * dz;  // 世界 → 邻居局部（与 `ObjectSupportProbes` 的绕 Y 约定一致）
        const double lz = s * dx + c * dz;
        const double ox = std::max(std::fabs(lx) - n.halfX, 0.0);
        const double oz = std::max(std::fabs(lz) - n.halfZ, 0.0);
        const double dist = std::hypot(ox, oz);
        if (dist < bestDist) {
            bestDist  = dist;
            bestIndex = i;
        }
    }
    if (bestIndex >= neighbors.size()) {
        return false;  // 半径内没有邻居 ⇒ 退回世界网格
    }

    // ② 贴到"最近的那一面"（邻居局部坐标系）：贴面轴固定、另一轴按网格量化。
    const PlacementFootprint& n  = neighbors[bestIndex];
    const double              c  = std::cos(n.yawDegrees * kDegreesToRadians);
    const double              s  = std::sin(n.yawDegrees * kDegreesToRadians);
    const double              dx = pointX - n.x;
    const double              dz = pointZ - n.z;
    const double              lx = c * dx - s * dz;
    const double              lz = s * dx + c * dz;
    const double              qx = quantize(lx, gridStep);
    const double              qz = quantize(lz, gridStep);
    const double candidates[4][2] = {
        { n.halfX + selfHalfX, qz },    // +X 面
        { -(n.halfX + selfHalfX), qz },  // −X 面
        { qx, n.halfZ + selfHalfZ },    // +Z 面
        { qx, -(n.halfZ + selfHalfZ) },  // −Z 面
    };
    // 选面：优先取"点**外侧得更多**"的那条轴的面（这才是"从哪边靠近"的直觉：从 +X 靠近就贴 +X 面）；
    // 点在盒内（或恰在边上）⇒ 取最近的贴面候选（确定性：并列取先者）。
    const double overshootX = std::fabs(lx) - n.halfX;
    const double overshootZ = std::fabs(lz) - n.halfZ;
    std::size_t  face       = 0;
    if (overshootX > overshootZ && overshootX > 0.0) {
        face = (lx >= 0.0) ? 0U : 1U;  // +X / −X 面
    } else if (overshootZ > 0.0) {
        face = (lz >= 0.0) ? 2U : 3U;  // +Z / −Z 面
    } else {
        std::size_t nearest     = 0;
        double      nearestDist = -1.0;
        for (std::size_t f = 0; f < 4; ++f) {
            const double wx   = c * candidates[f][0] + s * candidates[f][1];  // 局部 → 世界（R(θ)）
            const double wz   = -s * candidates[f][0] + c * candidates[f][1];
            const double dist = std::hypot(wx - dx, wz - dz);
            if (nearestDist < 0.0 || dist < nearestDist) {
                nearestDist = dist;
                nearest     = f;
            }
        }
        face = nearest;
    }
    outX = n.x + (c * candidates[face][0] + s * candidates[face][1]);
    outZ = n.z + (-s * candidates[face][0] + c * candidates[face][1]);
    return true;
}

}  // namespace vx
