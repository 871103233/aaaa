#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstring>

namespace vx {

/// 一个**实例**的世界位姿（V0.7 H1 / [ADR 0034](../../docs/adr/0034-object-instancing-and-hlod.md)）。
///
/// 与逐网格路径同口径：世界原点用 **`double`**（红线 6），旋转用四元数；
/// 顶点仍是**原型网格的局部坐标**，绘制时由引擎把它变成**渲染原点相对**坐标。
///
/// V0.8 追加**围合体**字段（[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策四）：
/// 供片元着色器判定"该片元是否处在室内"（⇒ 调暗环境项）。室外物件保持缺省（`enclosureEnabled = false`）。
struct InstancePose {
    glm::dvec3 origin { 0.0, 0.0, 0.0 };
    glm::quat  rotation { 1.0F, 0.0F, 0.0F, 0.0F };

    /// 该实例所属建筑的**围合体代理**（世界坐标）；`false` ⇒ 不参与室内变暗（室外物件）。
    bool  enclosureEnabled  = false;
    float enclosureCenterX  = 0.0F;
    float enclosureCenterZ  = 0.0F;
    float enclosureHalfX    = 0.0F;
    float enclosureHalfZ    = 0.0F;
    float enclosureCeilingY = 0.0F;  ///< 屋檐下沿的**绝对世界高度**（格）
};

/// 单个实例在 GPU 实例缓冲中的**字节数**：`mat4`(64) + `vec4`(16) + `vec4`(16) = 96。
///
/// 必须与 `assets/shaders/mesh_instanced.vert` / `shadow_instanced.vert` 的 **std430 结构体**逐字节一致
/// （`mat4` 无隐式填充、两个 `vec4` 紧随其后 ⇒ 数组步长 96）。
inline constexpr std::uint32_t kInstancePoseBytes = 96U;

/// 纯函数（红线 7）：实例位姿 + 渲染原点 → `modelToRender`（局部坐标 → **渲染原点相对**坐标）。
///
/// - 平移量在 **`double`** 下相减后才落回 `float` ⇒ 大坐标不丢精度（与 `MeshResources::origin` 同口径）；
/// - 矩阵**无缩放** ⇒ `mat3(modelToRender)` 是纯旋转，法线直接乘即可（无需逆转置）。
[[nodiscard]] inline glm::mat4 BuildInstanceModelToRender(const InstancePose& pose,
                                                          const glm::dvec3&   renderOrigin) noexcept {
    glm::mat4 model = glm::mat4_cast(pose.rotation);
    model[3]        = glm::vec4(static_cast<float>(pose.origin.x - renderOrigin.x),
                                static_cast<float>(pose.origin.y - renderOrigin.y),
                                static_cast<float>(pose.origin.z - renderOrigin.z), 1.0F);
    return model;
}

/// 纯函数（红线 7）：把**一批**实例位姿打包成 GPU 实例缓冲内容
/// （每个实例 = `kInstancePoseBytes` 字节：`mat4` 渲染相对变换 + 围合体 `vec4` ×2）。
///
/// - 返回**实际写入**的实例数 = `min(count, capacity)` —— 超容量时**截断**（调用方据返回值判断是否 WARN，
///   见 `MeshRenderer::UploadInstances`；SKILL 硬规则「超容量不静默」）；
/// - `poses` / `outInstances` 为空或 `capacity == 0` ⇒ 返回 0（不写任何字节）；
/// - 围合体字段按 `enclosureEnabled` → `w`（1/0）写出，其余分量原样透传（不做隐式默认）。
[[nodiscard]] inline std::uint32_t PackInstanceTransforms(const InstancePose* poses, std::uint32_t count,
                                                          std::uint32_t capacity, const glm::dvec3& renderOrigin,
                                                          float* outInstances) noexcept {
    if (poses == nullptr || outInstances == nullptr || capacity == 0U) {
        return 0U;
    }
    constexpr std::size_t kFloatsPerInstance = kInstancePoseBytes / sizeof(float);
    const std::uint32_t   used               = (count < capacity) ? count : capacity;
    for (std::uint32_t index = 0; index < used; ++index) {
        const InstancePose& pose  = poses[index];
        const glm::mat4     model = BuildInstanceModelToRender(pose, renderOrigin);
        float*              slot  = outInstances + static_cast<std::size_t>(index) * kFloatsPerInstance;
        std::memcpy(slot, &model[0][0], sizeof(float) * 16U);
        slot[16] = pose.enclosureCenterX;
        slot[17] = pose.enclosureCenterZ;
        slot[18] = pose.enclosureHalfX;
        slot[19] = pose.enclosureHalfZ;
        slot[20] = pose.enclosureCeilingY;
        slot[21] = 0.0F;  // 预留
        slot[22] = 0.0F;  // 预留
        slot[23] = pose.enclosureEnabled ? 1.0F : 0.0F;
    }
    return used;
}

}  // namespace vx
