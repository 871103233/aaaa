#include "dig/volume_mesher.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {
namespace {

/// 单元 8 个角的偏移：位 0 = X、位 1 = Y、位 2 = Z（角下标即"该位为 1 则偏移 1"）。
constexpr int kCornerOffsets[8][3] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
    { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 },
};

/// 单元 12 条棱（角下标对；两端只差一个位）。
constexpr int kCellEdges[12][2] = {
    { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },  // 沿 X
    { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },  // 沿 Y
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },  // 沿 Z
};

/// 三轴的 (u, v) 置换：满足右手关系 `u × v = axis`（发射四边形时按 (u, v) 平面定向绕序）。
constexpr int kAxisU[3] = { 1, 2, 0 };
constexpr int kAxisV[3] = { 2, 0, 1 };

/// 采样索引范围 `[-1, kVolumeBlockSize]` ⇒ 每轴 `kVolumeBlockSize + 2` 个。
constexpr int kSampleExtent = kVolumeBlockSize + 2;  // 34

/// cell 索引范围 `[-1, kVolumeBlockSize - 1]` ⇒ 每轴 `kVolumeBlockSize + 1` 个。
/// 之所以往外多一圈：每条网格棱由"u/v 下侧"的 cell 发射，该 cell 可能落在本块之外（围裙）。
constexpr int kCellExtent = kVolumeBlockSize + 1;  // 33

/// `(i, j, k)` → 扁平下标；索引可为 `-1`，故统一加 1 平移。
[[nodiscard]] inline std::size_t FlatIndex(int i, int j, int k, int extent) noexcept {
    const std::size_t extentSize = static_cast<std::size_t>(extent);
    return static_cast<std::size_t>(i + 1) +
           extentSize * (static_cast<std::size_t>(j + 1) + extentSize * static_cast<std::size_t>(k + 1));
}

}  // namespace

MeshData BuildVolumeMesh(const IVolumeSampler& sampler) {
    const std::size_t sampleCount = static_cast<std::size_t>(kSampleExtent) *
                                    static_cast<std::size_t>(kSampleExtent) * static_cast<std::size_t>(kSampleExtent);
    const std::size_t cellCount =
        static_cast<std::size_t>(kCellExtent) * static_cast<std::size_t>(kCellExtent) * static_cast<std::size_t>(kCellExtent);

    // 采样缓存：多取一圈（索引 -1 与 kVolumeBlockSize），使块边界处的顶点与邻块逐位一致。
    std::vector<float> samples(sampleCount, 0.0F);
    for (int k = -1; k <= kVolumeBlockSize; ++k) {
        for (int j = -1; j <= kVolumeBlockSize; ++j) {
            for (int i = -1; i <= kVolumeBlockSize; ++i) {
                samples[FlatIndex(i, j, k, kSampleExtent)] = sampler.Sample(i, j, k);
            }
        }
    }

    MeshData mesh;
    std::vector<std::int32_t> cellVertex(cellCount, -1);  ///< cell → 顶点下标（`-1` = 无顶点）
    std::vector<std::uint8_t> cellSign(cellCount, 0);     ///< cell → 角符号掩码（位 n 置 1 = 角 n 实心）

    // ---- 顶点：每个跨越表面的 cell 至多 1 个，位置 = 各棱交点平均 ----
    for (int k = -1; k < kVolumeBlockSize; ++k) {
        for (int j = -1; j < kVolumeBlockSize; ++j) {
            for (int i = -1; i < kVolumeBlockSize; ++i) {
                float        corner[8] = {};
                std::uint8_t signMask  = 0;
                for (int n = 0; n < 8; ++n) {
                    const float value = samples[FlatIndex(i + kCornerOffsets[n][0], j + kCornerOffsets[n][1],
                                                         k + kCornerOffsets[n][2], kSampleExtent)];
                    corner[n]         = value;
                    if (value < 0.0F) {
                        signMask |= static_cast<std::uint8_t>(1U << n);
                    }
                }

                const std::size_t cellIndex = FlatIndex(i, j, k, kCellExtent);
                cellSign[cellIndex]         = signMask;
                if (signMask == 0U || signMask == 0xFFU) {
                    continue;  // 全空 / 全实心：该 cell 不产生顶点
                }

                float sumX = 0.0F;
                float sumY = 0.0F;
                float sumZ = 0.0F;
                int   crossings = 0;
                for (int e = 0; e < 12; ++e) {
                    const int  a        = kCellEdges[e][0];
                    const int  b        = kCellEdges[e][1];
                    const bool solidA   = ((signMask >> a) & 1U) != 0U;
                    const bool solidB   = ((signMask >> b) & 1U) != 0U;
                    if (solidA == solidB) {
                        continue;  // 该棱同侧：无交点
                    }
                    const float denominator = corner[a] - corner[b];
                    const float t           = (denominator != 0.0F) ? (corner[a] / denominator) : 0.5F;
                    sumX += static_cast<float>(kCornerOffsets[a][0]) +
                            t * static_cast<float>(kCornerOffsets[b][0] - kCornerOffsets[a][0]);
                    sumY += static_cast<float>(kCornerOffsets[a][1]) +
                            t * static_cast<float>(kCornerOffsets[b][1] - kCornerOffsets[a][1]);
                    sumZ += static_cast<float>(kCornerOffsets[a][2]) +
                            t * static_cast<float>(kCornerOffsets[b][2] - kCornerOffsets[a][2]);
                    ++crossings;
                }

                const float inverse = (crossings > 0) ? (1.0F / static_cast<float>(crossings)) : 0.0F;

                MeshVertex vertex;
                vertex.position[0] = static_cast<float>(i) + sumX * inverse;
                vertex.position[1] = static_cast<float>(j) + sumY * inverse;
                vertex.position[2] = static_cast<float>(k) + sumZ * inverse;

                // 法线 = 密度场梯度（八角逐轴差分），指向密度增大的一侧 ⇒ **空侧**。
                const float gradientX = (corner[1] + corner[3] + corner[5] + corner[7]) -
                                        (corner[0] + corner[2] + corner[4] + corner[6]);
                const float gradientY = (corner[2] + corner[3] + corner[6] + corner[7]) -
                                        (corner[0] + corner[1] + corner[4] + corner[5]);
                const float gradientZ = (corner[4] + corner[5] + corner[6] + corner[7]) -
                                        (corner[0] + corner[1] + corner[2] + corner[3]);
                const float lengthSq = gradientX * gradientX + gradientY * gradientY + gradientZ * gradientZ;
                if (lengthSq > 1.0e-12F) {
                    const float inverseLength = 1.0F / std::sqrt(lengthSq);
                    vertex.normal[0]          = gradientX * inverseLength;
                    vertex.normal[1]          = gradientY * inverseLength;
                    vertex.normal[2]          = gradientZ * inverseLength;
                } else {
                    vertex.normal[0] = 0.0F;  // 退化（理论上仅在零梯度处出现）：给一个确定值而非 NaN
                    vertex.normal[1] = 1.0F;
                    vertex.normal[2] = 0.0F;
                }

                // 材质槽位（ADR 0014）：取该 cell 内**实体侧**（密度 < 0）各角材质的众数。
                //
                // 只统计实体侧：顶点落在等值面上，它属于"被切开的实体"；洞的内表面因此携带
                // "被切开的材质"，片元直接用它选层（不再按高度/坡度，避免"地下草地"）。
                // 取众数而非固定角：跨材质的 cell 只能产 1 个顶点，众数给出确定且占多数的答案。
                {
                    std::uint8_t slots[8]  = {};
                    std::uint8_t counts[8] = {};
                    int          distinct  = 0;
                    for (int c = 0; c < 8; ++c) {
                        if (corner[c] >= 0.0F) {
                            continue;  // 空侧不参与
                        }
                        const std::uint8_t slot =
                            sampler.SampleMaterial(i + kCornerOffsets[c][0], j + kCornerOffsets[c][1],
                                                   k + kCornerOffsets[c][2]);
                        int found = -1;
                        for (int d = 0; d < distinct; ++d) {
                            if (slots[d] == slot) {
                                found = d;
                                break;
                            }
                        }
                        if (found < 0) {
                            slots[distinct]  = slot;
                            counts[distinct] = 1;
                            ++distinct;
                        } else {
                            ++counts[found];
                        }
                    }
                    if (distinct > 0) {
                        int best = 0;
                        for (int d = 1; d < distinct; ++d) {
                            if (counts[d] > counts[best]) {
                                best = d;
                            }
                        }
                        vertex.material = (slots[best] == kNoMaterialSlot)
                                              ? kNoMaterialOverride
                                              : static_cast<float>(slots[best]);
                    }
                }

                cellVertex[cellIndex] = static_cast<std::int32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
            }
        }
    }

    // ---- 四边形：只遍历本块的 core cell；每条网格棱由"u/v 下侧"的那个 cell 发射一次 ----
    // 这样每条棱的四边形**恰好发射一次**，且跨块的棱由拥有该 cell 的块负责 ⇒ 不会重复、不会漏。
    for (int k = 0; k < kVolumeBlockSize; ++k) {
        for (int j = 0; j < kVolumeBlockSize; ++j) {
            for (int i = 0; i < kVolumeBlockSize; ++i) {
                const std::uint8_t signMask = cellSign[FlatIndex(i, j, k, kCellExtent)];
                if (signMask == 0U || signMask == 0xFFU) {
                    continue;
                }

                for (int axis = 0; axis < 3; ++axis) {
                    // 该 cell 的"下侧"棱：局部 (u = 0, v = 0)，沿 axis 从 0 到 1。
                    // 棱的两个端点即角 0 与角 `1 << axis`（角的位编号 = 该轴的偏移）。
                    const std::uint8_t cornerAxisBit = static_cast<std::uint8_t>(1U << axis);
                    const bool         solidAtZero   = (signMask & 1U) != 0U;
                    const bool         solidAtAxis   = ((signMask >> cornerAxisBit) & 1U) != 0U;
                    if (solidAtZero == solidAtAxis) {
                        continue;  // 该棱两端同侧：无表面
                    }

                    const int u = kAxisU[axis];
                    const int v = kAxisV[axis];

                    int coordB[3] = { i, j, k };
                    coordB[u] -= 1;
                    int coordC[3] = { i, j, k };
                    coordC[v] -= 1;
                    int coordD[3] = { i, j, k };
                    coordD[u] -= 1;
                    coordD[v] -= 1;

                    const std::int32_t indexA = cellVertex[FlatIndex(i, j, k, kCellExtent)];
                    const std::int32_t indexB = cellVertex[FlatIndex(coordB[0], coordB[1], coordB[2], kCellExtent)];
                    const std::int32_t indexC = cellVertex[FlatIndex(coordC[0], coordC[1], coordC[2], kCellExtent)];
                    const std::int32_t indexD = cellVertex[FlatIndex(coordD[0], coordD[1], coordD[2], kCellExtent)];
                    if (indexA < 0 || indexB < 0 || indexC < 0 || indexD < 0) {
                        continue;  // 防御：该棱两端异侧时 4 个 cell 必然都有顶点，正常不可达
                    }

                    const std::uint32_t a = static_cast<std::uint32_t>(indexA);
                    const std::uint32_t b = static_cast<std::uint32_t>(indexB);
                    const std::uint32_t c = static_cast<std::uint32_t>(indexC);
                    const std::uint32_t d = static_cast<std::uint32_t>(indexD);
                    // 绕序：实心在 axis=0 侧 ⇒ 正面（逆时针）朝向 +axis ⇒ (A, B, D, C)；
                    //       反之正面朝向 -axis ⇒ 反向为 (A, C, D, B)。
                    if (solidAtZero) {
                        mesh.indices.insert(mesh.indices.end(), { a, b, d, a, d, c });
                    } else {
                        mesh.indices.insert(mesh.indices.end(), { a, c, d, a, d, b });
                    }
                }
            }
        }
    }

    return mesh;
}

}  // namespace vx
