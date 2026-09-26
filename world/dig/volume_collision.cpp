#include "dig/volume_collision.hpp"

#include <algorithm>
#include <cstdint>

namespace vx {

VolumeCollision::VolumeCollision(PhysicsWorld& physics) : m_physics(physics) {
    m_scratch.reserve(3 * 1024);  // 单块 Surface Nets 的顶点量级（32³ cell 至多 1024 个顶点）
}

VolumeCollision::~VolumeCollision() {
    RemoveAll();
}

bool VolumeCollision::SyncBlock(const DigVolumeWorld& volumes, const BlockCoord& coord) {
    const MeshData* mesh = volumes.FindMesh(coord);

    // 块不存在 / 无等值面（全实心或全空）⇒ 不需要碰撞体。全实心的块也不会被走到（四面都是实心）。
    if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.size() < 3) {
        RemoveBlock(coord);
        return false;
    }

    // 顶点是**块内局部坐标**，世界定位交给静态刚体的位置（红线 6：世界定位用 `double`）。
    const std::size_t vertexCount = mesh->vertices.size();
    m_scratch.resize(vertexCount * 3);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        m_scratch[i * 3 + 0] = mesh->vertices[i].position[0];
        m_scratch[i * 3 + 1] = mesh->vertices[i].position[1];
        m_scratch[i * 3 + 2] = mesh->vertices[i].position[2];
    }

    PhysicsWorld::MeshDesc desc;
    desc.positions     = m_scratch.data();
    desc.vertexCount   = vertexCount;
    desc.indices       = mesh->indices.data();
    desc.triangleCount = mesh->indices.size() / 3;
    desc.originX       = static_cast<double>(BlockOriginBlocks(coord.x));
    desc.originY       = static_cast<double>(BlockOriginBlocks(coord.y));
    desc.originZ       = static_cast<double>(BlockOriginBlocks(coord.z));

    const auto found = m_bodies.find(coord);
    if (found != m_bodies.end()) {
        return m_physics.UpdateMesh(found->second, desc);
    }

    const PhysicsWorld::BodyHandle handle = m_physics.AddMesh(desc);
    if (handle == 0) {
        return false;
    }
    m_bodies.emplace(coord, handle);
    return true;
}

std::size_t VolumeCollision::SyncBlocks(const DigVolumeWorld& volumes, const std::vector<BlockCoord>& blocks) {
    std::vector<BlockCoord> unique = blocks;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    std::size_t bodies = 0;
    for (const BlockCoord& coord : unique) {
        if (SyncBlock(volumes, coord)) {
            ++bodies;
        }
    }
    return bodies;
}

void VolumeCollision::RemoveBlock(const BlockCoord& coord) noexcept {
    const auto found = m_bodies.find(coord);
    if (found == m_bodies.end()) {
        return;
    }
    m_physics.RemoveBody(found->second);
    m_bodies.erase(found);
}

void VolumeCollision::RemoveAll() noexcept {
    for (const auto& entry : m_bodies) {
        m_physics.RemoveBody(entry.second);
    }
    m_bodies.clear();
}

bool VolumeCollision::HasBlock(const BlockCoord& coord) const noexcept {
    return m_bodies.find(coord) != m_bodies.end();
}

}  // namespace vx
