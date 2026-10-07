#version 450

// **实例化**阴影通道顶点着色器（V0.7 H1 / ADR 0034）：`shadow.vert` 的实例化变体。
//
// 与 `shadow.vert` 的差别只有一处：逐实例 `modelToRender` 来自
// **set = 0 / binding = 1 的只读 storage buffer**（用 `gl_InstanceIndex` 索引），
// 而不是逐网格 uniform 推送 ⇒ 阴影通道与主通道**共用同一份实例缓冲**（阴影与几何不可能错位）。
//
// 光空间矩阵仍是 set = 0 / binding = 0（每级联一个缓冲，由渲染侧逐级绑定）。

layout(location = 0) in vec3 inPosition;

layout(set = 0, binding = 0, std430) readonly buffer LightMatrixBuffer {
    mat4 lightMatrix;
} light;

// 逐实例数据（与 `mesh_instanced.vert` 的 `InstanceRecord` **同布局**；阴影通道只用其中的变换矩阵，
// 围合体字段不参与深度绘制）。布局必须逐字节一致，否则同一缓冲会被解释错位。
struct InstanceRecord {
    mat4 modelToRender;
    vec4 enclosureA;
    vec4 enclosureB;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    InstanceRecord records[];
} instances;

layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 未使用（矩阵来自实例缓冲）
    vec4 meshParams;  // 与 mesh.vert 同布局（阴影通道不使用）
    vec4 meshTint;    // V0.10：与 mesh.vert 同布局（阴影通道不使用，仅为 uniform 块尺寸一致）
} meshTransform;

void main() {
    const mat4 modelToRender   = instances.records[gl_InstanceIndex].modelToRender;
    const vec3 relativePosition = (modelToRender * vec4(inPosition, 1.0)).xyz;
    gl_Position = light.lightMatrix * vec4(relativePosition, 1.0);
}
