#pragma once

#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace vx {

/// 传送门**交互提示半径**（格）：角色与门的距离 ≤ 该值 ⇒ 出"按 E 传送"提示（V3）。
///
/// 取值口径：略大于门的半宽（1 格）+ 角色半径，使"走到门口"即触发、"远看"不触发；
/// 与游戏内 1 格 ≈ 1 米的尺度一致。
inline constexpr double kPortalPromptRadius = 4.0;

/// 一个**传送门**（交互查询用的**最小数据**）：世界坐标（底面中心，格）+ 目标世界 id。
///
/// 数据源（V3）：物件层放置时，由 `ObjectPlacement`（含 `targetWorldId`）与求解出的落点构造
/// —— `ObjectInstance` 不带目标世界，故由 game 层在放置循环里一并收集。
struct PortalEntry {
    glm::dvec3  position { 0.0 };  ///< 门的世界位置（底面中心，格）
    std::string targetWorldId;      ///< 走到附近按 E 后要切到的世界 id
};

/// **最近门**查询（纯函数，确定性）：返回 `portals` 中**距离 `from` 最近且 ≤ `radius`** 的门；
/// 半径内没有门 ⇒ `nullptr`。
///
/// 确定性（红线 7）：按 `portals` 的**下标顺序**遍历、以**严格小于**比较 ⇒ 等距时取**先出现者**，
/// 同一输入恒返回同一元素（放置顺序 = 遍历顺序）。
/// 距离为**三维欧氏距离**（门与查询点都取各自位置；门坐落于地面、查询点取角色位置）。
[[nodiscard]] inline const PortalEntry* FindNearestPortal(const std::vector<PortalEntry>& portals,
                                                          const glm::dvec3& from, double radius) noexcept {
    const PortalEntry* nearest         = nullptr;
    double             nearestDistanceSq = 0.0;
    const double       radiusSq        = radius * radius;
    for (const PortalEntry& portal : portals) {
        const glm::dvec3 delta      = portal.position - from;
        const double     distanceSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distanceSq > radiusSq) {
            continue;  // 半径之外（边界取闭区间：恰好等于 radius ⇒ 命中）
        }
        if (nearest == nullptr || distanceSq < nearestDistanceSq) {
            nearest           = &portal;
            nearestDistanceSq = distanceSq;
        }
    }
    return nearest;
}

}  // namespace vx
