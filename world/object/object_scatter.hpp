#pragma once

#include "object/object_layer.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vx {

/// 散布点（V8）：**平面位置 + 朝向**。竖直方向（底面 Y）由调用方按**地表高度**求解（与 `[[placement]]` 同口径）。
struct ScatterPoint {
    float x = 0.0f;
    float z = 0.0f;
    float yawDegrees = 0.0f;
};

namespace object_scatter_detail {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSqrt2 = 1.41421356237309504880;

/// 抖动幅度（× 网格步长）：偏移 ∈ `±kScatterJitter * cell` ⇒ **最小间距 ≥ `0.5 * cell`**（可证明）。
constexpr double kScatterJitter = 0.25;

/// splitmix64：整数哈希（**确定性、无全局状态**；只用 `seed` 与格子索引决定抖动 / 朝向）。
[[nodiscard]] inline std::uint64_t Hash(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

/// 把哈希值映射到 `[0, 1)`（取高 53 位 ⇒ 精确落在 double 的有效位数内）。
[[nodiscard]] inline double UnitFrom(std::uint64_t hash) noexcept {
    return static_cast<double>(hash >> 11U) * (1.0 / 9007199254740992.0);
}

/// 格子键：`seed` 与格子索引 `(ix, iz)` 混合 ⇒ 每格抖动的 u / v 与朝向各自独立、且可复现。
[[nodiscard]] inline std::uint64_t CellKey(std::uint64_t seed, int ix, int iz) noexcept {
    const std::uint64_t a = Hash(seed ^ 0x9E3779B97F4A7C15ULL);
    const std::uint64_t b = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix) ^ 0xA5A5A5A5U);
    const std::uint64_t c = static_cast<std::uint64_t>(static_cast<std::uint32_t>(iz) ^ 0x5A5A5A5AU);
    return Hash(a ^ (b << 32U) ^ c);
}

/// 确定性 Fisher–Yates 洗牌（用 `seed` 派生每个下标的随机数）—— 用于"从抖网格里**均匀**取子集"。
inline void Shuffle(std::vector<ScatterPoint>& points, std::uint64_t seed) noexcept {
    for (std::size_t i = points.size(); i > 1U; --i) {
        const std::uint64_t draw = Hash(seed ^ static_cast<std::uint64_t>(i) ^ 0xD1B54A32D192ED03ULL);
        const std::size_t   j    = static_cast<std::size_t>(draw % static_cast<std::uint64_t>(i));
        std::swap(points[i - 1U], points[j]);
    }
}

}  // namespace object_scatter_detail

/// **程序化散布**（V8；**纯函数、确定性**）：在圆域内生成 `count` 个带朝向的点，**互不重叠**。
///
/// 算法 = **分块抖动网格**（Chunked Jittered Grid，红线 15）：
///   1. 网格步长 `cell` 先按「圆面积 / 个数」估；若落在圆内的格点**不足** `count` ⇒ 乘固定比例**加密重试**（确定性，最多 64 轮）；
///   2. **每格最多 1 个物件**，抖动 ∈ `±0.25 * cell`（**有界抖动**）⇒ **最小间距 ≥ `0.5 * cell`**（可证明、已单测）；
///   3. 只收**格心距 ≤ `radius − 0.25·√2·cell`** 的格子 ⇒ 抖动后**仍落在半径内**；
///   4. 格点**多于** `count` 时按确定性洗牌**均匀取前 `count` 个**（抖网格的任意子集都保持上述间距与均匀性）。
///
/// 确定性（红线 7）：仅由 `scatter` 的字段决定 —— 同入参 ⇒ **逐位相同**；无全局状态、无线程 / 时间依赖、无堆外副作用。
/// 非法入参（`radius ≤ 0` 或 `count ≤ 0`）返回**空表**（不抛：配置侧已由 `ObjectTable::LoadFromFile` 拦下）。
[[nodiscard]] inline std::vector<ScatterPoint> PlanObjectScatter(const ObjectScatter& scatter) {
    using namespace object_scatter_detail;

    std::vector<ScatterPoint> points;
    if (scatter.radius <= 0.0F || scatter.count <= 0) {
        return points;
    }

    const double radius = static_cast<double>(scatter.radius);
    const double wanted = static_cast<double>(scatter.count);
    const double jitterStep = kScatterJitter * kSqrt2;  // 抖动最大位移（× cell）

    double cell = std::sqrt(kPi * radius * radius / wanted);  // 面积估：期望格点数 = count
    for (int attempt = 0; attempt < 64; ++attempt) {
        points.clear();

        const int    span = static_cast<int>(std::ceil(radius / cell));  // 覆盖圆域外接正方形
        const double jitterMax = jitterStep * cell;

        for (int iz = -span; iz <= span; ++iz) {
            for (int ix = -span; ix <= span; ++ix) {
                const double cellCenterX = (static_cast<double>(ix) + 0.5) * cell;
                const double cellCenterZ = (static_cast<double>(iz) + 0.5) * cell;
                if (std::sqrt(cellCenterX * cellCenterX + cellCenterZ * cellCenterZ) > radius - jitterMax) {
                    continue;  // 抖动后可能越出半径 ⇒ 该格不收
                }

                const std::uint64_t key = CellKey(scatter.seed, ix, iz);
                const double        u   = UnitFrom(Hash(key));
                const double        v   = UnitFrom(Hash(key ^ 0xABCDEF0123456789ULL));
                const double        w   = UnitFrom(Hash(key ^ 0x1234567890ABCDEFULL));

                ScatterPoint point;
                point.x = static_cast<float>(static_cast<double>(scatter.centerX) + cellCenterX +
                                             (u - 0.5) * 2.0 * kScatterJitter * cell);
                point.z = static_cast<float>(static_cast<double>(scatter.centerZ) + cellCenterZ +
                                             (v - 0.5) * 2.0 * kScatterJitter * cell);
                point.yawDegrees = static_cast<float>(w * 360.0);
                points.push_back(point);
            }
        }

        if (static_cast<int>(points.size()) >= scatter.count) {
            break;  // 够数（多出来的下面均匀截断）
        }
        cell *= 0.85;  // 圆内格点不足 ⇒ 加密重试（确定性：cell 只由入参与轮次决定）
    }

    if (static_cast<int>(points.size()) > scatter.count) {
        Shuffle(points, scatter.seed);
        points.resize(static_cast<std::size_t>(scatter.count));
    }
    return points;
}

}  // namespace vx
