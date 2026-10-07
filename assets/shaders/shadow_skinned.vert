#version 450

// 蒙皮网格的**阴影通道**顶点着色器（T69）：与 shadow.vert 同源，但顶点先经骨骼矩阵变形。
//
// 为什么必须补这一条：换成模型之前，主角胶囊是普通 `MeshVertex` 网格、**会正常投影**；
// 若蒙皮网格在阴影通道缺席，主角就会"影子消失" —— 那是**可见回退**。故阴影通道也要蒙皮版本。
//
// 与 shadow.vert 的差别：多声明 **set = 0 / binding = 1 的骨骼矩阵数组**（binding 0 仍是本级的
// 光空间矩阵），并把顶点输入换成 `SkinnedVertex` 的 layout 0 / 2 / 3（只消费位置，法线不参与深度写入）。
// 片元阶段复用空入口的 `shadow.frag`。

layout(location = 0) in vec3 inPosition;
layout(location = 2) in uvec4 inJoints;
layout(location = 3) in vec4 inWeights;

layout(set = 0, binding = 0, std430) readonly buffer LightMatrixBuffer {
    mat4 lightMatrix;
} light;

layout(set = 0, binding = 1, std430) readonly buffer BoneBuffer {
    mat4 boneMatrices[];
} bones;

layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 局部坐标 → 渲染原点相对坐标
    vec4 meshParams;  // W6e：与 mesh_skinned.vert 同布局（x = 逐网格不透明度；阴影通道不使用）
    vec4 meshTint;    // V0.10：与 mesh.vert 同布局（阴影通道不使用，仅为 uniform 块尺寸一致）
} meshTransform;

void main() {
    const mat4 skin = bones.boneMatrices[inJoints.x] * inWeights.x +
                      bones.boneMatrices[inJoints.y] * inWeights.y +
                      bones.boneMatrices[inJoints.z] * inWeights.z +
                      bones.boneMatrices[inJoints.w] * inWeights.w;
    const vec3 skinnedPosition  = (skin * vec4(inPosition, 1.0)).xyz;
    const vec3 relativePosition = (meshTransform.modelToRender * vec4(skinnedPosition, 1.0)).xyz;
    gl_Position = light.lightMatrix * vec4(relativePosition, 1.0);
}
