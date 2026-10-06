#pragma once

#include <cmath>
#include <functional>
#include <optional>

namespace vx {

/// 地表拾取结果（阶段 V0.5 的 E2：坐标拾取辅助）。
struct GroundPick {
    float x        = 0.0F;  ///< 命中处的世界列坐标（格）
    float z        = 0.0F;
    float surfaceY = 0.0F;  ///< 命中处的地表高度（格）
    float distance = 0.0F;  ///< 沿射线的命中距离（格）
};

/// 沿射线**步进**求"与地表的交点"（纯函数，便于单测）。
///
/// `heightAt(x, z, outHeight)` 返回 false 表示该列**无地表数据**（超出常驻窗口 / 尚未加载）——
/// 与 `TerrainWorld::QueryHeight` 同口径；这类采样**跳过、不计入命中**（绝不猜一个高度）。
/// 命中判据：射线上的点 `p.y <= 地表高度`（首步即命中，例如相机在地下，也返回该点）。
/// 返回 `std::nullopt` 表示在 `maxDistance` 内未命中。
///
/// `direction` 无需归一化（内部会归一化）；`maxDistance` / `stepDistance` 非正、或方向为零向量 ⇒ 返回 `nullopt`。
[[nodiscard]] inline std::optional<GroundPick> RaycastGround(
    float originX, float originY, float originZ, float dirX, float dirY, float dirZ, float maxDistance,
    float stepDistance, const std::function<bool(float, float, float&)>& heightAt) {
    if (!(maxDistance > 0.0F) || !(stepDistance > 0.0F)) {
        return std::nullopt;
    }
    const float dirLengthSq = dirX * dirX + dirY * dirY + dirZ * dirZ;
    if (!(dirLengthSq > 0.0F)) {
        return std::nullopt;
    }
    const float invLength = 1.0F / std::sqrt(dirLengthSq);
    dirX *= invLength;
    dirY *= invLength;
    dirZ *= invLength;

    for (float distance = 0.0F; distance <= maxDistance; distance += stepDistance) {
        const float x        = originX + dirX * distance;
        const float y        = originY + dirY * distance;
        const float z        = originZ + dirZ * distance;
        float       surfaceY = 0.0F;
        if (!heightAt(x, z, surfaceY)) {
            continue;  // 无地表数据 ⇒ 跳过该采样（不猜高度）
        }
        if (y <= surfaceY) {
            GroundPick hit;
            hit.x        = x;
            hit.z        = z;
            hit.surfaceY = surfaceY;
            hit.distance = distance;
            return hit;
        }
    }
    return std::nullopt;
}

}  // namespace vx
