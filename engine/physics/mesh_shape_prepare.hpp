#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vx {

struct MeshData;  // render/mesh_renderer.hpp

/// **预构建的三角网形状**（不透明 —— 公共头**不泄漏** Jolt 类型）。
///
/// 用途（W7-S3b① / [ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)）：`JPH::MeshShape` 的 BVH 构建
/// **实测 ≈ 16 ms/块**（见 `physics_world.cpp` 的 `FavorBuildSpeed` 注释）；逐块在主线程建会让帧时间爆表。
/// 把构建**下沉 worker**（本接口），主线程只做廉价的"加体"（`PhysicsWorld::AddMesh(prepared, ...)`）。
class PreparedMeshShape;

/// **worker 侧**：由三角形网构建网格形状（纯计算：不触碰 `PhysicsWorld`、不加锁、无 IO）。
///
/// `positions` = `3 * vertexCount` 个 `float`（块局部或世界坐标都可，定位由加体时给的原点承担）；
/// `indices` = `3 * triangleCount` 个 `uint32`。
/// 失败（空输入 / 索引越界 / Jolt 报错）返回 `nullptr`（不静默产出半成品）。
///
/// 返回 `shared_ptr`（而非 `unique_ptr`）：`PreparedMeshShape` 是**不透明类型**，调用方（如 worker 的
/// 结果结构体）在头文件里只能看到前置声明 —— `unique_ptr` 的销毁需要完整类型，`shared_ptr` 不需要。
[[nodiscard]] std::shared_ptr<PreparedMeshShape> PrepareMeshShape(const float* positions, std::size_t vertexCount,
                                                                  const std::uint32_t* indices,
                                                                  std::size_t triangleCount);

/// **worker 侧**：`MeshData` 的便捷重载（内部摊平 `MeshVertex` 的位置缓冲）。
[[nodiscard]] std::shared_ptr<PreparedMeshShape> PrepareMeshShape(const MeshData& mesh);

}  // namespace vx
