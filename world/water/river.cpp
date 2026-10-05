// 河流（W6 / [ADR 0027]）：确定性下坡河道 + 下切场 + 水面网格。
//
// 本文件只做**纯函数**几何：不碰渲染器、不碰物理（河床碰撞由地表壳的三角网自动承担，见 `world/shell/`）。

#include "water/river.hpp"

#include "generation/terrain_noise.hpp"
#include "terrain/terrain_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vx {
namespace {

/// 计算下坡方向时的差分探针距离（格）。
constexpr float kGradientProbeBlocks = 2.0F;

/// 区域最高点的**粗扫步长**（格）：河道起点只需"大致最高"，不必逐列。
constexpr int kSourceScanStep = 16;

/// 河道**候选源点**的个数上限（取高度最高的前若干个，再选"最长的那条河"）。
constexpr std::size_t kMaxSourceCandidates = 16;

/// 候选源点距区域边界的**最小边距**（格）：贴着边界的源点会"刚生成就出界"。
constexpr int kSourceMarginBlocks = 32;

[[nodiscard]] float HeightAt(const TerrainNoiseGenerator& noise, float x, float z) noexcept {
    return HeightToBlocks(noise.HeightUnits(static_cast<std::int64_t>(std::llround(x)),
                                            static_cast<std::int64_t>(std::llround(z))));
}

/// 河道下切的横向**剖面系数**（`[0,1]`）：`d ≤ coreHalfWidth` 保持 1，随后在 `bankWidth` 内线性衰减到 0
/// （河岸连续）。实际下切深度 = 剖面系数 × 该处的 `carveDepth`。
[[nodiscard]] float CarveRamp(float distance, float coreHalfWidth, float bankWidth) noexcept {
    if (distance <= coreHalfWidth) {
        return 1.0F;
    }
    if (!(bankWidth > 0.0F) || distance >= coreHalfWidth + bankWidth) {
        return 0.0F;
    }
    return 1.0F - (distance - coreHalfWidth) / bankWidth;
}

/// 点到线段的最短距离（XZ 平面）。
[[nodiscard]] float DistanceToSegment(float px, float pz, float ax, float az, float bx, float bz, float* outT) noexcept {
    const float dx   = bx - ax;
    const float dz   = bz - az;
    const float len2 = dx * dx + dz * dz;
    float       t    = (len2 > 1.0e-6F) ? (((px - ax) * dx + (pz - az) * dz) / len2) : 0.0F;
    t                = std::clamp(t, 0.0F, 1.0F);
    if (outT != nullptr) {
        *outT = t;
    }
    const float cx = ax + t * dx;
    const float cz = az + t * dz;
    const float ox = px - cx;
    const float oz = pz - cz;
    return std::sqrt(ox * ox + oz * oz);
}

/// 从 `(startX, startZ)` 起沿**最陡下降**方向走一条河道（纯函数）；语义见 `GenerateRiverPath`。
[[nodiscard]] RiverPath WalkRiver(const TerrainNoiseGenerator& noise, const TerrainRiverParams& params, float startX,
                                  float startZ, int minColumnX, int minColumnZ, int maxColumnX, int maxColumnZ) {
    RiverPath path;
    float      x = startX;
    float      z = startZ;

    // `baseDir` = 下坡方向（仅在梯度可用时更新；平坦 / 洼地保持上一次）——**抖动只作用于它**，
    // 不写回 ⇒ 前进方向不会随步数累积旋转（否则在平地上会原地打转成螺旋）。
    float baseDirX      = 1.0F;
    float baseDirZ      = 0.0F;
    float dirX          = 1.0F;
    float dirZ          = 0.0F;
    float previousLevel = 1.0e30F;

    for (int node = 0; node < params.maxNodes; ++node) {
        // 自交终止：河道**不得缠绕成环**（走到已访问过的位置附近就结束）——否则在盆地里会一直打转。
        // 与"上一节点"的距离恒 ≈ stepBlocks，故比较时跳过紧邻的前一个节点。
        {
            const float closeSquared = params.stepBlocks * params.stepBlocks * 0.25F;
            for (std::size_t older = 0; older + 1U < path.nodes.size(); ++older) {
                const float dx = path.nodes[older].x - x;
                const float dz = path.nodes[older].z - z;
                if (dx * dx + dz * dz < closeSquared) {
                    return path;
                }
            }
        }

        const float height = HeightAt(noise, x, z);

        // 水位 = `min(上游水位, 河床底 + 水深)` ⇒ **单调不升**（水往低处流），且**不高于当地地表**。
        const float level = std::min(height - params.channelDepthBlocks + params.waterDepthBlocks, previousLevel);
        previousLevel     = level;

        // 下切深度由水位**反推** ⇒ 河床恒在水位之下 `waterDepth`（地形回升 ⇒ 河道更深，形成峡谷，
        // 而不是"水落到河床之下"）。由 `level ≤ height − channelDepth + waterDepth` ⇒ `carveDepth ≥ channelDepth`。
        const float carveDepth = height - level + params.waterDepthBlocks;

        // 水面半宽 = 下切系数降到"河床恰好等于水位"处的距离 ⇒ 水面恰好填满河道（不悬在岸上）。
        const float halfWidth = params.channelHalfWidthBlocks +
                                params.bankHalfWidthBlocks * std::min(params.waterDepthBlocks / carveDepth, 1.0F);

        RiverNode out;
        out.x                = x;
        out.z                = z;
        out.waterLevelBlocks = level;
        out.halfWidthBlocks  = std::max(halfWidth, 0.5F);
        out.carveDepthBlocks = carveDepth;
        path.nodes.push_back(out);

        // 下坡方向 = -∇height（中心差分）。
        const float gradientX =
            (HeightAt(noise, x + kGradientProbeBlocks, z) - HeightAt(noise, x - kGradientProbeBlocks, z)) /
            (2.0F * kGradientProbeBlocks);
        const float gradientZ =
            (HeightAt(noise, x, z + kGradientProbeBlocks) - HeightAt(noise, x, z - kGradientProbeBlocks)) /
            (2.0F * kGradientProbeBlocks);
        float       nextX  = -gradientX;
        float       nextZ  = -gradientZ;
        const float length = std::sqrt(nextX * nextX + nextZ * nextZ);
        if (length > 1.0e-4F) {
            baseDirX = nextX / length;
            baseDirZ = nextZ / length;
        }  // 平坦 / 洼地 ⇒ 保持上一次的下坡方向（不累积抖动）

        // 确定性域扭曲：把**下坡方向**旋转一个由噪声给出的偏角（河道蜿蜒）；结果不写回 `baseDir`。
        const float angle = noise.RiverJitterAt(x, z) * params.jitterRadians;
        const float cosA  = std::cos(angle);
        const float sinA  = std::sin(angle);
        dirX              = baseDirX * cosA - baseDirZ * sinA;
        dirZ              = baseDirX * sinA + baseDirZ * cosA;

        x += dirX * params.stepBlocks;
        z += dirZ * params.stepBlocks;
        if (x < static_cast<float>(minColumnX) || x >= static_cast<float>(maxColumnX) ||
            z < static_cast<float>(minColumnZ) || z >= static_cast<float>(maxColumnZ)) {
            break;  // 走出区域 ⇒ 止（全图铺开属 W7 流式）
        }
    }
    return path;
}

}  // namespace

RiverPath GenerateRiverPath(const TerrainNoiseGenerator& noise, const TerrainRiverParams& params, int minColumnX,
                            int minColumnZ, int maxColumnX, int maxColumnZ) {
    RiverPath best;
    if (!params.enabled || maxColumnX <= minColumnX || maxColumnZ <= minColumnZ || !(params.stepBlocks > 0.0F) ||
        params.maxNodes <= 0) {
        return best;
    }

    // 候选源点 = 区域内**离边界至少 `kSourceMarginBlocks`** 的列里最高的若干列（粗扫）。
    // 为什么不用单一起点：贴着边界的源点 ⇒ 河道刚生成就出界；改用多候选再取"**最长的**一条"
    // （确定性：按高度降序稳定排序 ⇒ 同高按扫描序）。
    const int margin     = kSourceMarginBlocks;
    const int scanBeginX = minColumnX + margin;
    const int scanEndX   = maxColumnX - margin;
    const int scanBeginZ = minColumnZ + margin;
    const int scanEndZ   = maxColumnZ - margin;
    if (scanBeginX >= scanEndX || scanBeginZ >= scanEndZ) {
        return best;  // 区域太小（连边距都不够）⇒ 不生成河道
    }

    struct Source {
        float x      = 0.0F;
        float z      = 0.0F;
        float height = 0.0F;
    };
    std::vector<Source> sources;
    for (int scanZ = scanBeginZ; scanZ < scanEndZ; scanZ += kSourceScanStep) {
        for (int scanX = scanBeginX; scanX < scanEndX; scanX += kSourceScanStep) {
            Source source;
            source.x      = static_cast<float>(scanX);
            source.z      = static_cast<float>(scanZ);
            source.height = HeightAt(noise, source.x, source.z);
            sources.push_back(source);
        }
    }
    std::stable_sort(sources.begin(), sources.end(),
                     [](const Source& a, const Source& b) { return a.height > b.height; });
    if (sources.size() > kMaxSourceCandidates) {
        sources.resize(kMaxSourceCandidates);
    }

    for (const Source& source : sources) {
        RiverPath candidate =
            WalkRiver(noise, params, source.x, source.z, minColumnX, minColumnZ, maxColumnX, maxColumnZ);
        if (candidate.nodes.size() > best.nodes.size()) {
            best = std::move(candidate);
        }
    }
    return best;
}

RiverCarveField::RiverCarveField(const RiverPath& path, const TerrainRiverParams& params, int minColumnX,
                                 int minColumnZ, int sizeX, int sizeZ)
    : m_minColumnX(minColumnX), m_minColumnZ(minColumnZ), m_sizeX(sizeX), m_sizeZ(sizeZ) {
    if (path.Empty() || sizeX <= 0 || sizeZ <= 0) {
        return;
    }
    m_values.assign(static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeZ), 0.0F);

    const std::size_t nodeCount = path.nodes.size();
    for (int iz = 0; iz < sizeZ; ++iz) {
        const float worldZ = static_cast<float>(minColumnZ + iz);
        for (int ix = 0; ix < sizeX; ++ix) {
            const float worldX = static_cast<float>(minColumnX + ix);

            float distance   = 1.0e30F;
            float carveDepth = params.channelDepthBlocks;
            for (std::size_t n = 0; n + 1 < nodeCount; ++n) {
                const RiverNode& a = path.nodes[n];
                const RiverNode& b = path.nodes[n + 1];
                float            t = 0.0F;
                const float      d = DistanceToSegment(worldX, worldZ, a.x, a.z, b.x, b.z, &t);
                if (d < distance) {
                    distance   = d;
                    // 下切深度沿最近的线段线性插值 ⇒ 河道从"正常河床"平滑过渡到"峡谷"。
                    carveDepth = a.carveDepthBlocks + t * (b.carveDepthBlocks - a.carveDepthBlocks);
                }
            }
            if (nodeCount == 1) {
                const RiverNode& a = path.nodes[0];
                distance            = DistanceToSegment(worldX, worldZ, a.x, a.z, a.x, a.z, nullptr);
                carveDepth          = a.carveDepthBlocks;
            }
            m_values[static_cast<std::size_t>(iz) * static_cast<std::size_t>(sizeX) + static_cast<std::size_t>(ix)] =
                CarveRamp(distance, params.channelHalfWidthBlocks, params.bankHalfWidthBlocks) * carveDepth;
        }
    }
}

float RiverCarveField::CarveAt(float worldX, float worldZ) const noexcept {
    if (m_values.empty()) {
        return 0.0F;
    }
    // 双线性采样（栅格 = 整数世界列）⇒ 河岸在格间连续过渡。
    const float localX = std::clamp(worldX - static_cast<float>(m_minColumnX), 0.0F,
                                    static_cast<float>(m_sizeX - 1));
    const float localZ = std::clamp(worldZ - static_cast<float>(m_minColumnZ), 0.0F,
                                    static_cast<float>(m_sizeZ - 1));
    const int   x0 = static_cast<int>(localX);
    const int   z0 = static_cast<int>(localZ);
    const int   x1 = std::min(x0 + 1, m_sizeX - 1);
    const int   z1 = std::min(z0 + 1, m_sizeZ - 1);
    const float fx = localX - static_cast<float>(x0);
    const float fz = localZ - static_cast<float>(z0);

    const auto at = [&](int ix, int iz) noexcept {
        return m_values[static_cast<std::size_t>(iz) * static_cast<std::size_t>(m_sizeX) +
                        static_cast<std::size_t>(ix)];
    };
    const float v00 = at(x0, z0);
    const float v10 = at(x1, z0);
    const float v01 = at(x0, z1);
    const float v11 = at(x1, z1);
    const float top = v00 + (v10 - v00) * fx;
    const float bottom = v01 + (v11 - v01) * fx;
    return top + (bottom - top) * fz;
}

MeshData BuildRiverWaterMesh(const RiverPath& path, int minColumnX, int minColumnZ) {
    MeshData mesh;
    if (path.Empty()) {
        return mesh;
    }
    const std::size_t nodeCount = path.nodes.size();
    mesh.vertices.reserve(nodeCount * 2U);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const RiverNode& point = path.nodes[i];

        float tangentX = 1.0F;
        float tangentZ = 0.0F;
        if (i == 0) {
            tangentX = path.nodes[1].x - point.x;
            tangentZ = path.nodes[1].z - point.z;
        } else if (i + 1U == nodeCount) {
            tangentX = point.x - path.nodes[i - 1U].x;
            tangentZ = point.z - path.nodes[i - 1U].z;
        } else {
            tangentX = path.nodes[i + 1U].x - path.nodes[i - 1U].x;
            tangentZ = path.nodes[i + 1U].z - path.nodes[i - 1U].z;
        }
        const float length = std::sqrt(tangentX * tangentX + tangentZ * tangentZ);
        if (length > 1.0e-5F) {
            tangentX /= length;
            tangentZ /= length;
        } else {
            tangentX = 1.0F;
            tangentZ = 0.0F;
        }

        // XZ 平面内的横向（法向）：`(-tz, tx)`。
        const float normalX = -tangentZ;
        const float normalZ = tangentX;
        const float halfWidth = std::max(point.halfWidthBlocks, 0.5F);

        MeshVertex left;
        MeshVertex right;
        left.position[0]  = point.x + normalX * halfWidth - static_cast<float>(minColumnX);
        left.position[1]  = point.waterLevelBlocks;
        left.position[2]  = point.z + normalZ * halfWidth - static_cast<float>(minColumnZ);
        right.position[0] = point.x - normalX * halfWidth - static_cast<float>(minColumnX);
        right.position[1] = point.waterLevelBlocks;
        right.position[2] = point.z - normalZ * halfWidth - static_cast<float>(minColumnZ);
        left.normal[0]    = 0.0F;
        left.normal[1]    = 1.0F;
        left.normal[2]    = 0.0F;
        right.normal[0]   = 0.0F;
        right.normal[1]   = 1.0F;
        right.normal[2]   = 0.0F;

        mesh.vertices.push_back(left);
        mesh.vertices.push_back(right);
    }

    for (std::size_t i = 0; i + 1U < nodeCount; ++i) {
        const std::uint32_t left0  = static_cast<std::uint32_t>(i * 2U);
        const std::uint32_t right0 = left0 + 1U;
        const std::uint32_t left1  = static_cast<std::uint32_t>((i + 1U) * 2U);
        const std::uint32_t right1 = left1 + 1U;
        // 绕序不作要求：水面管线**关闭背面剔除**（ribbon 从上下看都要可见）。
        mesh.indices.insert(mesh.indices.end(), { left0, right0, left1, right0, right1, left1 });
    }
    return mesh;
}

}  // namespace vx
