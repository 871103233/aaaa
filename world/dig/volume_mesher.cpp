#include "dig/volume_mesher.hpp"

#include <algorithm>
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

/// 一条网格棱的 4 个邻接 cell（`A` = 棱下端所属 cell；`B` = A−u、`C` = A−v、`D` = A−u−v）
/// 各自"**该棱**"在这个 cell 里的**局部棱编号**（`kCellEdges` 下标）。
///
/// 推导：同一根网格棱在各 cell 里都是由"只差一个二进制位的两个角"构成的棱 ——
/// 例如沿 Z 的棱，在 A 里是角 {0,4}（= 局部棱 8），在 B（沿 X 偏移 −1）里是角 {1,5}（= 9），
/// 在 C（沿 Y 偏移 −1）里是 {2,6}（= 10），在 D 里是 {3,7}（= 11）。
/// **用途（T55）**：四边形发射时要取每个 cell "属于这根棱那个分量"的子顶点。
constexpr int kQuadCellLocalEdge[3][4] = {
    { 0, 1, 2, 3 },    // 沿 X：A{0,1} B{2,3} C{4,5} D{6,7}
    { 4, 6, 5, 7 },    // 沿 Y：A{0,2} B{4,6} C{1,3} D{5,7}
    { 8, 9, 10, 11 },  // 沿 Z：A{0,4} B{1,5} C{2,6} D{3,7}
};

/// `(i, j, k)` → 扁平下标；索引可为 `-1`，故统一加 1 平移。extent 为**每轴**的步长（T42 起任意尺寸）。
[[nodiscard]] inline std::size_t FlatIndex(int i, int j, int k, int extentX, int extentY) noexcept {
    return static_cast<std::size_t>(i + 1) +
           static_cast<std::size_t>(extentX) *
               (static_cast<std::size_t>(j + 1) + static_cast<std::size_t>(extentY) * static_cast<std::size_t>(k + 1));
}

}  // namespace

MeshData BuildRegionMesh(const IVolumeSampler& sampler, int sizeX, int sizeY, int sizeZ) {
    MeshData mesh;
    if (sizeX < 1 || sizeY < 1 || sizeZ < 1) {
        return mesh;
    }

    // 采样索引范围 `[-1, size]` ⇒ 每轴 `size + 2` 个。
    const int sampleExtentX = sizeX + 2;
    const int sampleExtentY = sizeY + 2;
    const int sampleExtentZ = sizeZ + 2;

    // cell 索引范围 `[-1, size - 1]` ⇒ 每轴 `size + 1` 个。
    // 之所以往外多一圈：每条网格棱由"u/v 下侧"的 cell 发射，该 cell 可能落在本区域之外（围裙）。
    const int cellExtentX = sizeX + 1;
    const int cellExtentY = sizeY + 1;
    const int cellExtentZ = sizeZ + 1;

    const std::size_t sampleCount = static_cast<std::size_t>(sampleExtentX) *
                                    static_cast<std::size_t>(sampleExtentY) *
                                    static_cast<std::size_t>(sampleExtentZ);
    const std::size_t cellCount = static_cast<std::size_t>(cellExtentX) * static_cast<std::size_t>(cellExtentY) *
                                  static_cast<std::size_t>(cellExtentZ);

    // 采样缓存：多取一圈（索引 -1 与 size），使区域边界处的顶点与邻块逐位一致。
    std::vector<float> samples(sampleCount, 0.0F);
    for (int k = -1; k <= sizeZ; ++k) {
        for (int j = -1; j <= sizeY; ++j) {
            for (int i = -1; i <= sizeX; ++i) {
                samples[FlatIndex(i, j, k, sampleExtentX, sampleExtentY)] = sampler.Sample(i, j, k);
            }
        }
    }

    std::vector<std::int32_t> cellVertex(cellCount, -1);  ///< cell → 顶点下标（`-1` = 无顶点）
    std::vector<std::uint8_t> cellSign(cellCount, 0);     ///< cell → 角符号掩码（位 n 置 1 = 角 n 实心）
    /// cell 的**每条局部棱** → 该棱应使用的**子顶点序号**（歧义 cell 拆出的第几个顶点；单分量 cell 恒 0）。
    ///
    /// 为什么需要（T55）：**歧义 cell**（实体侧角分成 ≥ 2 个"共棱不连通"的分量，例：对角实心）
    /// 只放 1 个顶点时会把两片表面**捏合**成一点/一条线 ⇒ 相邻网格边被 3 / 4 个三角形共用
    /// （`CountBoundaryEdges > 0` = 非流形）⇒ 玩家看到"某个面透明 / 缺面"。
    /// 按实体侧**连通分量**各放一个顶点（球面等值面常用的 Manifold Dual Contouring 做法）后，
    /// 每条棱取"它自己那个分量"的顶点 ⇒ 曲面重新成为**流形**。
    /// **单分量 cell 偏移恒为 0 ⇒ 输出与引入本机制前逐位相同**（既有判据零回归）。
    std::vector<std::uint8_t> cellEdgeVertex(cellCount * 12U, 0U);

    // ---- 顶点：每个跨越表面的 cell **每个实体侧连通分量**各 1 个，位置 = 该分量所辖棱交点平均 ----
    for (int k = -1; k < sizeZ; ++k) {
        for (int j = -1; j < sizeY; ++j) {
            for (int i = -1; i < sizeX; ++i) {
                float        corner[8] = {};
                std::uint8_t signMask  = 0;
                for (int n = 0; n < 8; ++n) {
                    const float value =
                        samples[FlatIndex(i + kCornerOffsets[n][0], j + kCornerOffsets[n][1],
                                          k + kCornerOffsets[n][2], sampleExtentX, sampleExtentY)];
                    corner[n] = value;
                    if (value < 0.0F) {
                        signMask |= static_cast<std::uint8_t>(1U << n);
                    }
                }

                const std::size_t cellIndex = FlatIndex(i, j, k, cellExtentX, cellExtentY);
                cellSign[cellIndex]         = signMask;
                if (signMask == 0U || signMask == 0xFFU) {
                    continue;  // 全空 / 全实心：该 cell 不产生顶点
                }

                // ---- 实体侧连通分量（T55）：实体角之间按"**共棱**"（角下标 Hamming 距离 = 1）连通 ----
                // 歧义 cell（例：对角实心）会得到 ≥2 个分量 ⇒ 每个分量各放一个顶点；否则只有 1 个分量。
                int solidCorners[8]    = {};
                int cornerComponent[8] = {};  // 角下标 → 分量序号（空侧为 -1）
                int solidCount         = 0;
                for (int n = 0; n < 8; ++n) {
                    cornerComponent[n] = -1;
                    if (((signMask >> n) & 1U) != 0U) {
                        solidCorners[solidCount++] = n;
                    }
                }
                int parent[8] = {};
                for (int n = 0; n < solidCount; ++n) {
                    parent[n] = n;
                }
                for (int first = 0; first < solidCount; ++first) {
                    for (int second = first + 1; second < solidCount; ++second) {
                        const unsigned int difference =
                            static_cast<unsigned int>(solidCorners[first] ^ solidCorners[second]);
                        const bool shareEdge = difference != 0U && (difference & (difference - 1U)) == 0U;
                        if (!shareEdge) {
                            continue;
                        }
                        int rootFirst = first;
                        while (parent[rootFirst] != rootFirst) {
                            parent[rootFirst] = parent[parent[rootFirst]];
                            rootFirst        = parent[rootFirst];
                        }
                        int rootSecond = second;
                        while (parent[rootSecond] != rootSecond) {
                            parent[rootSecond] = parent[parent[rootSecond]];
                            rootSecond        = parent[rootSecond];
                        }
                        if (rootFirst != rootSecond) {
                            parent[rootFirst] = rootSecond;
                        }
                    }
                }
                int rootOrdinal[8] = {};
                for (int n = 0; n < 8; ++n) {
                    rootOrdinal[n] = -1;
                }
                int componentCount = 0;
                for (int n = 0; n < solidCount; ++n) {
                    int root = n;
                    while (parent[root] != root) {
                        parent[root] = parent[parent[root]];
                        root         = parent[root];
                    }
                    if (rootOrdinal[root] < 0) {
                        rootOrdinal[root] = componentCount++;
                    }
                    cornerComponent[solidCorners[n]] = rootOrdinal[root];
                }

                // 逐分量累加"该分量所辖棱"的交点：单分量时与旧实现**同序同值**（逐位不变）。
                float sumX[8]      = {};
                float sumY[8]      = {};
                float sumZ[8]      = {};
                int   crossings[8] = {};
                for (int e = 0; e < 12; ++e) {
                    const int  a        = kCellEdges[e][0];
                    const int  b        = kCellEdges[e][1];
                    const bool solidA   = ((signMask >> a) & 1U) != 0U;
                    const bool solidB   = ((signMask >> b) & 1U) != 0U;
                    if (solidA == solidB) {
                        continue;  // 该棱同侧：无交点
                    }
                    const int   component   = cornerComponent[solidA ? a : b];
                    const float denominator = corner[a] - corner[b];
                    const float t           = (denominator != 0.0F) ? (corner[a] / denominator) : 0.5F;
                    sumX[component] += static_cast<float>(kCornerOffsets[a][0]) +
                                       t * static_cast<float>(kCornerOffsets[b][0] - kCornerOffsets[a][0]);
                    sumY[component] += static_cast<float>(kCornerOffsets[a][1]) +
                                       t * static_cast<float>(kCornerOffsets[b][1] - kCornerOffsets[a][1]);
                    sumZ[component] += static_cast<float>(kCornerOffsets[a][2]) +
                                       t * static_cast<float>(kCornerOffsets[b][2] - kCornerOffsets[a][2]);
                    ++crossings[component];
                    // 该棱归哪个分量 ⇒ 四边形发射时就用那个子顶点（单分量恒 0 ⇒ 与旧行为逐位一致）
                    cellEdgeVertex[cellIndex * 12U + static_cast<std::size_t>(e)] =
                        static_cast<std::uint8_t>(component);
                }

                // 法线 = 密度场梯度（八角逐轴差分），指向密度增大的一侧 ⇒ **空侧**。
                // 这是 **cell 级**量（8 个角都参与），歧义 cell 拆出的各子顶点共用它 —— 它们在几何上相邻。
                const float gradientX = (corner[1] + corner[3] + corner[5] + corner[7]) -
                                        (corner[0] + corner[2] + corner[4] + corner[6]);
                const float gradientY = (corner[2] + corner[3] + corner[6] + corner[7]) -
                                        (corner[0] + corner[1] + corner[4] + corner[5]);
                const float gradientZ = (corner[4] + corner[5] + corner[6] + corner[7]) -
                                        (corner[0] + corner[1] + corner[2] + corner[3]);
                const float lengthSq = gradientX * gradientX + gradientY * gradientY + gradientZ * gradientZ;
                float       normalX  = 0.0F;  // 退化（理论上仅在零梯度处出现）：给一个确定值而非 NaN
                float       normalY  = 1.0F;
                float       normalZ  = 0.0F;
                if (lengthSq > 1.0e-12F) {
                    const float inverseLength = 1.0F / std::sqrt(lengthSq);
                    normalX                   = gradientX * inverseLength;
                    normalY                   = gradientY * inverseLength;
                    normalZ                   = gradientZ * inverseLength;
                }

                cellVertex[cellIndex] = static_cast<std::int32_t>(mesh.vertices.size());
                for (int component = 0; component < componentCount; ++component) {
                    const float inverse =
                        (crossings[component] > 0) ? (1.0F / static_cast<float>(crossings[component])) : 0.0F;

                    MeshVertex vertex;
                    vertex.position[0] = static_cast<float>(i) + sumX[component] * inverse;
                    vertex.position[1] = static_cast<float>(j) + sumY[component] * inverse;
                    vertex.position[2] = static_cast<float>(k) + sumZ[component] * inverse;
                    vertex.normal[0]   = normalX;
                    vertex.normal[1]   = normalY;
                    vertex.normal[2]   = normalZ;

                    // 材质槽位（ADR 0014）：取**本分量**实体侧各角材质的众数（同票取更先出现者）。
                    //
                    // 只统计实体侧：顶点落在等值面上，它属于"被切开的实体"；洞的内表面因此携带
                    // "被切开的材质"，片元直接用它选层（不再按高度/坡度，避免"地下草地"）。
                    // 取众数而非固定角：跨材质的 cell 只能产 1 个顶点，众数给出确定且占多数的答案。
                    std::uint8_t slots[8]  = {};
                    std::uint8_t counts[8] = {};
                    int          distinct  = 0;
                    for (int c = 0; c < 8; ++c) {
                        if (cornerComponent[c] != component) {
                            continue;  // 只统计本分量的实体角（空侧与其他分量不参与）
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

                    mesh.vertices.push_back(vertex);
                }
            }
        }
    }

    // ---- 四边形：只遍历本区域的 core cell；每条网格棱由"u/v 下侧"的那个 cell 发射一次 ----
    // 这样每条棱的四边形**恰好发射一次**，且跨区域的棱由拥有该 cell 的调用方负责 ⇒ 不会重复、不会漏。
    for (int k = 0; k < sizeZ; ++k) {
        for (int j = 0; j < sizeY; ++j) {
            for (int i = 0; i < sizeX; ++i) {
                const std::uint8_t signMask = cellSign[FlatIndex(i, j, k, cellExtentX, cellExtentY)];
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

                    // 该棱的 4 个邻接 cell 是 A(i,j,k) / B(−u) / C(−v) / D(−u−v)。
                    // 每个 cell 取"**它自己那条对应本棱的局部棱**"所属的**子顶点**
                    // （`kQuadCellLocalEdge[axis][slot]`）—— 单分量 cell 偏移恒 0 ⇒ 与旧行为逐位一致；
                    // 歧义 cell 则取到与这根棱同一分量的那个顶点（T55：不再把两片表面捏合）。
                    const std::size_t cellFlat[4] = {
                        FlatIndex(i, j, k, cellExtentX, cellExtentY),
                        FlatIndex(coordB[0], coordB[1], coordB[2], cellExtentX, cellExtentY),
                        FlatIndex(coordC[0], coordC[1], coordC[2], cellExtentX, cellExtentY),
                        FlatIndex(coordD[0], coordD[1], coordD[2], cellExtentX, cellExtentY),
                    };
                    std::uint32_t cornerIndex[4] = {};
                    bool          complete      = true;
                    for (int slot = 0; slot < 4; ++slot) {
                        const std::int32_t base = cellVertex[cellFlat[slot]];
                        if (base < 0) {
                            complete = false;  // 防御：该棱两端异侧时 4 个 cell 必然都有顶点，正常不可达
                            break;
                        }
                        const std::int32_t index =
                            base + static_cast<std::int32_t>(
                                       cellEdgeVertex[cellFlat[slot] * 12U +
                                                      static_cast<std::size_t>(kQuadCellLocalEdge[axis][slot])]);
                        cornerIndex[slot] = static_cast<std::uint32_t>(index);
                    }
                    if (!complete) {
                        continue;
                    }

                    const std::uint32_t a = cornerIndex[0];
                    const std::uint32_t b = cornerIndex[1];
                    const std::uint32_t c = cornerIndex[2];
                    const std::uint32_t d = cornerIndex[3];
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

MeshData BuildVolumeMesh(const IVolumeSampler& sampler) {
    return BuildRegionMesh(sampler, kVolumeBlockSize, kVolumeBlockSize, kVolumeBlockSize);
}

std::size_t CountBoundaryEdges(const MeshData& mesh) {
    // 把每个三角形的三条**无向**边（较小下标在前）编码成一个 64 位键，排序后数每种键出现几次。
    // 为什么不用 std::map：整体外观网格可达数万三角形，排序 + 线性扫描无节点分配、且结果与顺序无关
    // （红线 7：确定性）。
    std::vector<std::uint64_t> edges;
    edges.reserve((mesh.indices.size() / 3U) * 3U);
    for (std::size_t triangle = 0; triangle + 2U < mesh.indices.size(); triangle += 3U) {
        for (std::size_t corner = 0; corner < 3U; ++corner) {
            const std::uint32_t a  = mesh.indices[triangle + corner];
            const std::uint32_t b  = mesh.indices[triangle + (corner + 1U) % 3U];
            const std::uint32_t lo = a < b ? a : b;
            const std::uint32_t hi = a < b ? b : a;
            edges.push_back((static_cast<std::uint64_t>(lo) << 32U) | static_cast<std::uint64_t>(hi));
        }
    }
    std::sort(edges.begin(), edges.end());
    std::size_t boundary = 0;
    for (std::size_t index = 0; index < edges.size();) {
        std::size_t run = index + 1U;
        while (run < edges.size() && edges[run] == edges[index]) {
            ++run;
        }
        if (run - index != 2U) {
            ++boundary;
        }
        index = run;
    }
    return boundary;
}

std::size_t CountDegenerateTriangles(const MeshData& mesh) {
    std::size_t degenerate = 0;
    for (std::size_t triangle = 0; triangle + 2U < mesh.indices.size(); triangle += 3U) {
        const std::uint32_t ia = mesh.indices[triangle];
        const std::uint32_t ib = mesh.indices[triangle + 1U];
        const std::uint32_t ic = mesh.indices[triangle + 2U];
        if (ia == ib || ib == ic || ia == ic) {
            ++degenerate;  // 顶点索引重复 ⇒ 面积必然为 0
            continue;
        }
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) {
            continue;  // 索引越界（不该发生）：不计入本判据
        }
        // 叉积长度的平方 ≤ 1e-12（= 长度 ≤ 1e-6）视为退化 —— 与既有测试的断言同口径。
        const float* a = mesh.vertices[ia].position;
        const float* b = mesh.vertices[ib].position;
        const float* c = mesh.vertices[ic].position;
        const float  ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float  ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const float  cx    = ab[1] * ac[2] - ab[2] * ac[1];
        const float  cy    = ab[2] * ac[0] - ab[0] * ac[2];
        const float  cz    = ab[0] * ac[1] - ab[1] * ac[0];
        if (cx * cx + cy * cy + cz * cz <= 1.0e-12F) {
            ++degenerate;
        }
    }
    return degenerate;
}

}  // namespace vx
