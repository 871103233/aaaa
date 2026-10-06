#include "render/software_occlusion.hpp"

#include <glm/vec4.hpp>

#include <cstddef>
#include <limits>

namespace vx {
namespace {

/// NDC x → 列号（0..width-1）。单调递增；结果为 grid 内的合法列。
[[nodiscard]] int CellColumn(float ndcX, int width) noexcept {
    if (width <= 1) {
        return 0;
    }
    const float t    = (ndcX * 0.5F + 0.5F) * static_cast<float>(width - 1) + 0.5F;
    int         cell = static_cast<int>(t);
    if (cell < 0) {
        cell = 0;
    }
    if (cell >= width) {
        cell = width - 1;
    }
    return cell;
}

/// NDC y → 行号（0..height-1）。NDC 的 +Y 向上、图像行向下 ⇒ **单调递减**。
[[nodiscard]] int CellRow(float ndcY, int height) noexcept {
    if (height <= 1) {
        return 0;
    }
    const float t    = (1.0F - (ndcY * 0.5F + 0.5F)) * static_cast<float>(height - 1) + 0.5F;
    int         cell = static_cast<int>(t);
    if (cell < 0) {
        cell = 0;
    }
    if (cell >= height) {
        cell = height - 1;
    }
    return cell;
}

/// 点（渲染相对）→ NDC。返回 false 表示点在相机所在平面之后或退化（此时调用方须保守处理）。
[[nodiscard]] bool ProjectToNdc(const glm::mat4& viewProjectionRelative, const glm::vec3& pointRelative,
                                glm::vec3& outNdc) noexcept {
    const glm::vec4 clip = viewProjectionRelative * glm::vec4(pointRelative, 1.0F);
    if (!(clip.w > 1.0e-6F)) {
        return false;  // 透视：w = 视深；≤ 0 表示不在前方
    }
    outNdc = glm::vec3(clip) / clip.w;
    return true;
}

[[nodiscard]] bool GridUsable(const OcclusionDepthGrid& grid) noexcept {
    return grid.width > 0 && grid.height > 0 &&
           grid.depth.size() == static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height);
}

}  // namespace

void ResetOcclusionDepthGrid(OcclusionDepthGrid& grid, int width, int height) {
    const int clampedWidth  = (width > 0) ? width : 0;
    const int clampedHeight = (height > 0) ? height : 0;
    const std::size_t cells =
        static_cast<std::size_t>(clampedWidth) * static_cast<std::size_t>(clampedHeight);
    if (grid.depth.size() != cells) {
        grid.depth.assign(cells, kNoOccluderDepth);
    } else {
        for (float& value : grid.depth) {
            value = kNoOccluderDepth;
        }
    }
    grid.width  = clampedWidth;
    grid.height = clampedHeight;
}

void SplatOccluderPoint(OcclusionDepthGrid& grid, const glm::mat4& viewProjectionRelative,
                        const glm::vec3& pointRelative) noexcept {
    if (!GridUsable(grid)) {
        return;
    }
    glm::vec3 ndc {};
    if (!ProjectToNdc(viewProjectionRelative, pointRelative, ndc)) {
        return;
    }
    if (ndc.x < -1.0F || ndc.x > 1.0F || ndc.y < -1.0F || ndc.y > 1.0F || ndc.z < 0.0F || ndc.z > 1.0F) {
        return;  // 出屏：不写（保守——不制造遮挡面）
    }
    const int   column = CellColumn(ndc.x, grid.width);
    const int   row    = CellRow(ndc.y, grid.height);
    float&      cell   = grid.depth[static_cast<std::size_t>(row) * static_cast<std::size_t>(grid.width) +
                          static_cast<std::size_t>(column)];
    if (ndc.z > cell) {
        cell = ndc.z;  // max 聚合（哨兵 -1 时首写即生效）
    }
}

bool IsAabbOccluded(const OcclusionDepthGrid& grid, const glm::mat4& viewProjectionRelative,
                    const glm::vec3& minimumRelative, const glm::vec3& maximumRelative, float depthBias,
                    int maxCoveredCells) noexcept {
    if (!GridUsable(grid) || maxCoveredCells <= 0) {
        return false;
    }

    float ndcMinimumX = std::numeric_limits<float>::max();
    float ndcMaximumX = std::numeric_limits<float>::lowest();
    float ndcMinimumY = std::numeric_limits<float>::max();
    float ndcMaximumY = std::numeric_limits<float>::lowest();
    float ndcNearestZ = std::numeric_limits<float>::max();  // 候选**最近**点（透视 NDC z 越大越远）
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point(((corner & 1) != 0) ? maximumRelative.x : minimumRelative.x,
                              ((corner & 2) != 0) ? maximumRelative.y : minimumRelative.y,
                              ((corner & 4) != 0) ? maximumRelative.z : minimumRelative.z);
        glm::vec3       ndc {};
        if (!ProjectToNdc(viewProjectionRelative, point, ndc)) {
            return false;
        }
        // 任一角点不在 NDC 盒内（含近 / 远裁剪）⇒ 保守：不剔。
        if (ndc.x < -1.0F || ndc.x > 1.0F || ndc.y < -1.0F || ndc.y > 1.0F || ndc.z < 0.0F || ndc.z > 1.0F) {
            return false;
        }
        ndcMinimumX = (ndc.x < ndcMinimumX) ? ndc.x : ndcMinimumX;
        ndcMaximumX = (ndc.x > ndcMaximumX) ? ndc.x : ndcMaximumX;
        ndcMinimumY = (ndc.y < ndcMinimumY) ? ndc.y : ndcMinimumY;
        ndcMaximumY = (ndc.y > ndcMaximumY) ? ndc.y : ndcMaximumY;
        ndcNearestZ = (ndc.z < ndcNearestZ) ? ndc.z : ndcNearestZ;
    }

    const int       columnBegin = CellColumn(ndcMinimumX, grid.width);
    const int       columnEnd   = CellColumn(ndcMaximumX, grid.width);
    const int       rowBegin    = CellRow(ndcMaximumY, grid.height);  // NDC +Y 向上 ⇒ 行的起点取上边
    const int       rowEnd      = CellRow(ndcMinimumY, grid.height);
    const long long covered     = static_cast<long long>(columnEnd - columnBegin + 1) *
                              static_cast<long long>(rowEnd - rowBegin + 1);
    if (covered <= 0 || covered > static_cast<long long>(maxCoveredCells)) {
        return false;  // 覆盖过大（近处大网格）⇒ 按保守处理，不做遮挡判定
    }

    for (int row = rowBegin; row <= rowEnd; ++row) {
        for (int column = columnBegin; column <= columnEnd; ++column) {
            const float occluder = grid.depth[static_cast<std::size_t>(row) * static_cast<std::size_t>(grid.width) +
                                              static_cast<std::size_t>(column)];
            if (occluder <= kNoOccluderDepth) {
                return false;  // 该格没有遮挡面
            }
            if (ndcNearestZ <= occluder + depthBias) {
                return false;  // 候选不比该格遮挡面更远 ⇒ 可能可见
            }
        }
    }
    return true;
}

}  // namespace vx
