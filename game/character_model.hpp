#pragma once

#include "render/mesh_renderer.hpp"  // SkinnedVertex / SkinnedMeshData
#include "render/model_loader.hpp"   // Model / ModelMesh / AnimationClip

#include <glm/common.hpp>  // glm::min / glm::max（vec3）
#include <glm/vec3.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace vx {

/// 由模型拼出的**单个**蒙皮网格 + 它在模型局部空间的"脚底中心"。
struct CharacterSkinnedMesh {
    SkinnedMeshData mesh;
    /// 绑定姿态包围盒的 `(中心 x, 最小 y, 中心 z)`。
    ///
    /// 用途：把**脚底中心**对齐到角色位置 —— 网格变换的原点应传
    /// `世界脚底位置 − localPivot`（见 `game/main.cpp`）。**不能**把这个偏移烘进顶点：
    /// 顶点要先进蒙皮（`Σ wᵢ·Mᵢ·v`），烘进去会被每个关节的矩阵各自搬运，角色会随动画整体漂移。
    glm::vec3 localPivot { 0.0F };
};

/// **纯函数**：把 `Model` 的**所有网格**拼成一个蒙皮网格，并算出绑定姿态的脚底中心。
///
/// 为什么合并成一个网格：占位阶段只需"看得见一个会动的角色"，而本项目**尚无角色材质系统**
/// （片元走地表材质路径，见 `mesh_skinned.vert`）⇒ 多网格只会多出 draw call 与材质槽，没有收益。
/// 合并后**顶点 / 索引数确定**，可直接单测（用 T68 的 `tests/fixtures/skinned_triangle.gltf`）。
///
/// 拓扑：每个源网格的索引整体偏移其顶点基数后追加（**不改绕序、不改法线**）。
/// 关节索引与权重原样搬运（`ModelVertex` 已保证权重归一化）。
[[nodiscard]] inline CharacterSkinnedMesh BuildSkinnedMeshFromModel(const Model& model) {
    CharacterSkinnedMesh out;
    for (const ModelMesh& source : model.meshes) {
        const std::uint32_t base = static_cast<std::uint32_t>(out.mesh.vertices.size());
        out.mesh.vertices.reserve(out.mesh.vertices.size() + source.vertices.size());
        for (const ModelVertex& vertex : source.vertices) {
            SkinnedVertex skinned;
            skinned.position[0] = vertex.position.x;
            skinned.position[1] = vertex.position.y;
            skinned.position[2] = vertex.position.z;
            skinned.normal[0] = vertex.normal.x;
            skinned.normal[1] = vertex.normal.y;
            skinned.normal[2] = vertex.normal.z;
            skinned.joints[0] = vertex.joints.x;
            skinned.joints[1] = vertex.joints.y;
            skinned.joints[2] = vertex.joints.z;
            skinned.joints[3] = vertex.joints.w;
            skinned.weights[0] = vertex.weights.x;
            skinned.weights[1] = vertex.weights.y;
            skinned.weights[2] = vertex.weights.z;
            skinned.weights[3] = vertex.weights.w;
            out.mesh.vertices.push_back(skinned);
        }
        out.mesh.indices.reserve(out.mesh.indices.size() + source.indices.size());
        for (const std::uint32_t index : source.indices) {
            out.mesh.indices.push_back(base + index);
        }
    }

    // 绑定姿态包围盒 ⇒ 脚底中心。空网格时 `localPivot` 保持 `(0,0,0)`（调用方不会上传空网格）。
    if (!out.mesh.vertices.empty()) {
        glm::vec3 minimum = glm::vec3(out.mesh.vertices.front().position[0], out.mesh.vertices.front().position[1],
                                      out.mesh.vertices.front().position[2]);
        glm::vec3 maximum = minimum;
        for (const SkinnedVertex& vertex : out.mesh.vertices) {
            const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
            minimum = glm::min(minimum, position);
            maximum = glm::max(maximum, position);
        }
        out.localPivot = glm::vec3((minimum.x + maximum.x) * 0.5F, minimum.y, (minimum.z + maximum.z) * 0.5F);
    }
    return out;
}

/// **纯函数**：按名字查动画 clip（**区分大小写**，与 glTF 里的名字一致）；找不到返回 `nullptr`。
[[nodiscard]] inline const AnimationClip* FindAnimationClip(const Model& model, std::string_view name) noexcept {
    for (const AnimationClip& clip : model.animations) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

}  // namespace vx
