#version 450

// 蒙皮网格顶点着色器（T69）：与 mesh.vert 同源，但顶点先经**骨骼矩阵**变形。
//
// 与 mesh.vert 的差别只有两处：
//   ① 顶点输入多了**关节索引与权重**（location 2 / 3）；
//   ② 顶点资源多一个 **set = 0 / binding = 1 的骨骼矩阵数组**（与 binding 0 的相机并列）。
// 片元阶段**复用 mesh.frag**（同一批采样器与 uniform 槽），因此这里输出的 varyings 与 mesh.vert 完全一致。
//
// 顶点输入（与 engine/render/mesh_renderer.hpp 的 `SkinnedVertex` 逐字段对应）：
//   location 0 `vec3 position` —— **绑定姿态**下的网格局部坐标（脚底为原点，与台座胶囊同一约定）
//   location 1 `vec3 normal`   —— 局部空间单位法线
//   location 2 `uvec4 joints`  —— 4 路关节索引（指向骨骼矩阵数组）
//   location 3 `vec4 weights`  —— 4 路权重（**CPU 侧已归一化**，见 model_loader 的取前 4 路重归一）
//
// 骨骼矩阵 = `Global(joint) × inverseBind(joint)`（CPU 由 `ComputeSkinningMatrices` 算好、整块上传），
// 故这里只做加权求和 —— **线性混合蒙皮（LBS）**，与 UE5 / Unity 的 GPU skinning 同形态。
//
// 坐标口径与 mesh.vert 一致：顶点是**网格局部**坐标，`modelToRender`（set 1）再把它补成
// **渲染原点相对**坐标；片元的材质权重按世界高度 / 坡度算，需要这个相对坐标 + uniform 渲染原点。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in uvec4 inJoints;
layout(location = 3) in vec4 inWeights;

layout(set = 0, binding = 0, std430) readonly buffer CameraBuffer {
    mat4 viewProjection;
} camera;

layout(set = 0, binding = 1, std430) readonly buffer BoneBuffer {
    mat4 boneMatrices[];
} bones;

layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 局部坐标 → 渲染原点相对坐标（std140：mat4 = 4 个 vec4）
} meshTransform;

layout(location = 0) out vec3 v_relativePosition;
layout(location = 1) out vec3 v_normal;
// `flat`：与 mesh.frag 的 `flat in float v_material` 对应（整面离散属性，不插值）。
layout(location = 2) flat out float v_material;

void main() {
    // 线性混合蒙皮：权重已在 CPU 侧归一化，直接加权求和。
    const mat4 skin = bones.boneMatrices[inJoints.x] * inWeights.x +
                      bones.boneMatrices[inJoints.y] * inWeights.y +
                      bones.boneMatrices[inJoints.z] * inWeights.z +
                      bones.boneMatrices[inJoints.w] * inWeights.w;
    const vec3 skinnedPosition = (skin * vec4(inPosition, 1.0)).xyz;
    const vec3 skinnedNormal   = mat3(skin) * inNormal;

    const vec3 relativePosition = (meshTransform.modelToRender * vec4(skinnedPosition, 1.0)).xyz;
    gl_Position        = camera.viewProjection * vec4(relativePosition, 1.0);
    v_relativePosition = relativePosition;
    v_normal           = mat3(meshTransform.modelToRender) * skinnedNormal;
    // **已知取舍（T69 占位阶段）**：占位模型不带自己的 PBR 贴图，走**地表材质路径**
    // （`kNoMaterialOverride` = -1 ⇒ 片元按世界高度 / 坡度逐像素算权重）。
    // 见 docs/plans/v0.3.md §1.3「明确不做」与 §3 的登记。
    v_material         = -1.0;
}
