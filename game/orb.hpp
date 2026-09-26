#pragma once

#include "dig/projectile_table.hpp"
#include "render/mesh_renderer.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// 弹道与命中检测所需的世界查询契约（T27）。
///
/// 只问两件事，使玩法侧的弹道逻辑**不依赖世界层的具体类型**（`TerrainWorld` / `DigVolumeWorld`），
/// 从而可以用"一块平地 + 一个实心球"的假实现在单测里钉死弹道与命中语义。
class IOrbWorldQuery {
public:
    virtual ~IOrbWorldQuery() = default;

    IOrbWorldQuery(const IOrbWorldQuery&) = delete;
    IOrbWorldQuery& operator=(const IOrbWorldQuery&) = delete;
    IOrbWorldQuery(IOrbWorldQuery&&) = delete;
    IOrbWorldQuery& operator=(IOrbWorldQuery&&) = delete;

    /// 该点是否为**实心**（地形内部；或可挖体积内尚未挖掉的实心处）。
    [[nodiscard]] virtual bool IsSolid(double x, double y, double z) const = 0;

protected:
    IOrbWorldQuery() = default;
};

/// 一枚弹丸的弹道状态。位置用 `double`（红线 6：世界定位不用 `float`）。
///
/// `previousPosition` 是**上一个固定步**的位置，只用于渲染插值（红线 11：插值绝不回写模拟状态）。
struct Orb {
    glm::dvec3 position { 0.0 };
    glm::dvec3 previousPosition { 0.0 };
    glm::vec3  velocity { 0.0F };
    float      age      = 0.0F;  ///< 已存活时间（秒）
    float      lifetime = 0.0F;  ///< 存活上限（秒）；由 `Fire` 按规格写入，超时即失效
    bool       active   = false;
};

/// 命中结果：`point` 为爆炸中心（世界坐标，格）。
struct OrbHit {
    bool       hit = false;
    glm::dvec3 point { 0.0 };
};

/// 线段命中检测（纯函数）：沿 `from → to` 以固定步长采样，返回**首次进入实心**的位置。
///
/// 为什么用定步长采样而不是解析求交：地形是"高度场 + 有界 SDF 体积"的混合，没有统一解析式。
/// 步长取 0.25 格（与相机避障同一量级），远小于弹丸半径，正常速度下不会穿过去；
/// 命中点取"**最后一个安全点与首个实心点的中点**"，使爆炸中心落在表面附近而不是深埋进岩体
/// （这正是"从山侧面射入 → 在山体上掏出洞"的关键：爆炸中心贴着山壁，而不是打进山里）。
[[nodiscard]] inline OrbHit MarchRay(const IOrbWorldQuery& world, const glm::dvec3& from, const glm::dvec3& to,
                                     double stepBlocks = 0.25) {
    OrbHit result;

    const glm::dvec3 delta = to - from;
    const double     length =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    const double step  = (stepBlocks > 0.0) ? stepBlocks : 0.25;
    const int    steps = std::max(1, static_cast<int>(std::ceil(length / step)));

    glm::dvec3 lastSafe = from;
    for (int index = 0; index <= steps; ++index) {
        const double     t     = static_cast<double>(index) / static_cast<double>(steps);
        const glm::dvec3 point = from + delta * t;
        if (world.IsSolid(point.x, point.y, point.z)) {
            result.hit   = true;
            result.point = (lastSafe + point) * 0.5;
            return result;
        }
        lastSafe = point;
    }
    return result;
}

/// 推进一枚弹丸一个**固定逻辑步**：重力 → 位移 → 命中检测 → 超时判定。
///
/// 前置条件：`dt` 为固定步长（红线 11：不得用可变帧间隔驱动物理，否则弹道随帧率漂移）。
/// 返回 true 表示本步命中（`outHit` 有效），弹丸已被置为不活动；
/// 返回 false 且弹丸变为不活动表示**超时消失**（`orb.lifetime` 到期，不产生爆炸）。
[[nodiscard]] inline bool StepOrb(Orb& orb, const IOrbWorldQuery& world, float gravity, float gravityScale, float dt,
                                  OrbHit& outHit) {
    if (!orb.active) {
        return false;
    }

    const glm::dvec3 previous = orb.position;
    orb.previousPosition = previous;  // 供渲染插值（不参与模拟）
    orb.velocity.y -= gravity * gravityScale * dt;
    orb.position += glm::dvec3(orb.velocity) * static_cast<double>(dt);
    orb.age += dt;

    const OrbHit hit = MarchRay(world, previous, orb.position);
    if (hit.hit) {
        outHit      = hit;
        orb.active  = false;
        return true;
    }
    if (orb.age >= orb.lifetime) {
        orb.active = false;  // 超时未命中：消失
    }
    return false;
}

/// 固定容量的光球池：`Fire` 复用失效槽位，稳态**零堆分配**（容量来自 `projectiles.toml` 的 `max_active`）。
class OrbPool {
public:
    explicit OrbPool(std::size_t capacity) : m_orbs(capacity) {}

    [[nodiscard]] std::size_t Capacity() const noexcept { return m_orbs.size(); }

    /// 全部槽位（含未激活项）；调用方按 `active` 过滤。渲染槽位与它一一对应，故顺序必须稳定。
    [[nodiscard]] std::vector<Orb>& Orbs() noexcept { return m_orbs; }
    [[nodiscard]] const std::vector<Orb>& Orbs() const noexcept { return m_orbs; }

    [[nodiscard]] std::size_t ActiveCount() const noexcept {
        std::size_t count = 0;
        for (const Orb& orb : m_orbs) {
            if (orb.active) {
                ++count;
            }
        }
        return count;
    }

    /// 从 `origin` 沿 `direction`（内部归一化，零向量视为失败）以 `spec.speed` 发射。
    /// 池满（没有失效槽位）返回 false，**不**淘汰已有弹丸（确定、可预期）。
    bool Fire(const glm::dvec3& origin, const glm::vec3& direction, const ProjectileSpec& spec) {
        const float lengthSq = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
        if (!(lengthSq > 0.0F)) {
            return false;
        }
        const float inverseLength = 1.0F / std::sqrt(lengthSq);

        for (Orb& orb : m_orbs) {
            if (orb.active) {
                continue;
            }
            orb.position         = origin;
            orb.previousPosition = origin;  // 新弹丸：插值起点与当前位置一致（不拖影）
            orb.velocity = direction * (inverseLength * spec.speed);
            orb.age      = 0.0F;
            orb.lifetime = spec.lifetimeSeconds;
            orb.active   = true;
            return true;
        }
        return false;
    }

private:
    std::vector<Orb> m_orbs;
};

/// 程序化球体网格（光球外观）：**局部坐标以球心为原点**，半径 `radius`（格）。
///
/// 与 `BuildCapsuleMesh` 同约定：法线朝外且为单位向量，三角形从外部看为逆时针
/// （与 `MeshRenderer` 的 `FRONTFACE_COUNTER_CLOCKWISE` + 背面剔除一致），
/// 顶点只含位置与法线（自发光由片元 uniform 槽 3 给出，不占顶点属性）。
[[nodiscard]] inline MeshData BuildOrbMesh(float radius, int radialSegments = 16, int rings = 10) {
    MeshData mesh;
    if (!(radius > 0.0F)) {
        return mesh;
    }

    const int   radial = std::max(radialSegments, 3);
    const int   stacks = std::max(rings, 2);
    const float kPi    = 3.14159265358979323846F;
    const float kTwoPi = 2.0F * kPi;

    /// 一圈顶点：θ = 2π 处复制一份，闭合四边形不必对索引取模。
    const std::size_t ringVertexCount = static_cast<std::size_t>(radial) + 1;
    /// 环数：phi 从 0（北极）到 π（南极）。
    const std::size_t ringCount = static_cast<std::size_t>(stacks) + 1;

    mesh.vertices.reserve(ringCount * ringVertexCount);
    for (int ring = 0; ring <= stacks; ++ring) {
        const float phi    = kPi * static_cast<float>(ring) / static_cast<float>(stacks);
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int segment = 0; segment <= radial; ++segment) {
            const float theta = kTwoPi * static_cast<float>(segment) / static_cast<float>(radial);
            const float nx    = sinPhi * std::cos(theta);
            const float ny    = cosPhi;
            const float nz    = sinPhi * std::sin(theta);

            MeshVertex vertex;
            vertex.position[0] = radius * nx;
            vertex.position[1] = radius * ny;
            vertex.position[2] = radius * nz;
            vertex.normal[0]   = nx;
            vertex.normal[1]   = ny;
            vertex.normal[2]   = nz;
            mesh.vertices.push_back(vertex);
        }
    }

    mesh.indices.reserve((ringCount - 1) * static_cast<std::size_t>(radial) * 6U);
    for (std::size_t ring = 0; ring + 1 < ringCount; ++ring) {
        const std::size_t row0 = ring * ringVertexCount;
        const std::size_t row1 = (ring + 1) * ringVertexCount;
        for (int segment = 0; segment < radial; ++segment) {
            const std::uint32_t p00 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(segment));
            const std::uint32_t p01 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(segment) + 1U);
            const std::uint32_t p10 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(segment));
            const std::uint32_t p11 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(segment) + 1U);

            mesh.indices.push_back(p00);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p10);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p11);
            mesh.indices.push_back(p10);
        }
    }

    return mesh;
}

}  // namespace vx
