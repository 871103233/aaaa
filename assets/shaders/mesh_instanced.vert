#version 450

// **实例化**网格顶点着色器（V0.7 H1 / ADR 0034）：`mesh.vert` 的实例化变体。
//
// 与 `mesh.vert` 的唯一差别：逐实例的 `modelToRender` **不来自 uniform 块**，而来自
// **set = 0 / binding = 1 的只读 storage buffer**（`mat4[]`，用 `gl_InstanceIndex` 索引）。
// 这使"同一原型画 N 次"变成**一次绘制**（`numInstances = N`），draw call 与物件数解耦。
//
// 顶点输入 / 输出与 `mesh.vert` **逐字段一致**（片元阶段复用 `mesh.frag`）：
//   location 0 `vec3 position`（原型网格局部坐标）、1 `vec3 normal`、2 `float material`、3 `float morph`。
// `morph`（CDLOD 顶点过渡）**仅供地表使用**：物件不 morph ⇒ 本着色器**不消费**它（保持与 mesh.vert 的顶点布局一致）。
//
// uniform 块（set 1 / binding 0）仍保留：`meshParams.x` = 逐批不透明度（Bayer 抖动淡出）；
// 其 `modelToRender` 字段**未被本着色器读取**（矩阵来自实例缓冲）——保留是为了与 `MeshTransformUniform`
// 的布局 / 推送路径一致（CPU 侧推一次即可，避免第二套块布局）。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inMaterial;
layout(location = 3) in float inMorph;

layout(set = 0, binding = 0, std430) readonly buffer CameraBuffer {
    mat4 viewProjection;
    vec4 lodOrigin;  // 仅为与 mesh.vert 的布局一致而声明（本着色器不读）
} camera;

// V0.7 H1：逐实例数据（局部坐标 → 渲染原点相对坐标 + 围合体代理）。CPU 侧每帧**一次**整块上传。
// V0.8：`enclosureA.xy` = 围合体中心 XZ、`enclosureA.zw` = 半尺寸 XZ；`enclosureB.x` = 屋檐下沿绝对高度、
//       `enclosureB.w` = 启用位（1/0）。口径与 `engine/render/instance_batch.hpp` 的 `kInstancePoseBytes` 逐字节一致。
struct InstanceRecord {
    mat4 modelToRender;
    vec4 enclosureA;
    vec4 enclosureB;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    InstanceRecord records[];
} instances;

layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 未使用（矩阵来自实例缓冲）；保留以复用同一块布局与推送路径
    vec4 meshParams;     // x = 逐批不透明度（1 = 不透明；< 1 = 片元 Bayer 抖动淡出），yzw 预留
} meshTransform;

layout(location = 0) out vec3 v_relativePosition;
layout(location = 1) out vec3 v_normal;
layout(location = 2) flat out float v_material;
layout(location = 3) out float v_fade;
// V0.8 室内变暗：逐实例围合体代理（`flat`：整实例同值，不做插值）。片元据此判定"是否室内"。
layout(location = 4) flat out vec4 v_enclosureA;
layout(location = 5) flat out vec4 v_enclosureB;

void main() {
    const InstanceRecord record = instances.records[gl_InstanceIndex];
    const mat4 modelToRender = record.modelToRender;

    const vec3 relativePosition = (modelToRender * vec4(inPosition, 1.0)).xyz;
    gl_Position         = camera.viewProjection * vec4(relativePosition, 1.0);
    v_relativePosition  = relativePosition;
    v_normal            = mat3(modelToRender) * inNormal;
    v_material          = inMaterial;
    v_fade              = meshTransform.meshParams.x;
    v_enclosureA        = record.enclosureA;
    v_enclosureB        = record.enclosureB;
}
