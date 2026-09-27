#version 450

// 地表 / 体积网格的顶点着色器（真正的网格路径，非 PoC 冒烟三角形）。
//
// 顶点输入与 engine/render/mesh_renderer.hpp 的 MeshVertex 布局一一对应：
//   location 0 `vec3 position` —— **网格局部坐标**（T41）：地表 tile 内 0..64 格、可挖体积块内 0..32 格。
//                                世界定位用整数 / double（红线 6），一个网格到底是"哪一块"由下面的
//                                `meshOffset`（网格世界原点 − 渲染原点）在顶点阶段补上，
//                                故这里的顶点数值恒为小量，float 的有效位不会被大坐标吃掉。
//   location 1 `vec3 normal`   —— 世界空间单位法线（由高度场梯度算出，禁止面法线近似）。
//   location 2 `float material` —— **材质槽位覆盖**（ADR 0014）：
//                                 < 0 = 未指定（片元按世界高度 / 坡度逐像素算权重，地表走这条）；
//                                 >= 0 = 直接用该槽位（可挖体积的内表面走这条）。
//
// **不再**传递材质权重：ADR 0009 起权重由片元着色器逐像素按世界高度与坡度重算。
// 片元需要世界坐标才能算权重，故这里只传出**渲染原点相对**位置；片元用 uniform 的渲染原点把它还原为世界坐标。
//
// 相机常量：SDL3_gpu 的 SPIR-V 约定 —— 顶点着色器 set 0 放只读资源（采样器 / 存储缓冲），
// set 1 放 uniform。渲染侧用 SDL_BindGPUVertexStorageBuffers(pass, 0, ...) 绑定该缓冲，
// 故这里声明为 set = 0、binding = 0 的只读 storage buffer，布局与 CameraUniform 一致
// （std430 下 mat4 即 4 个 vec4，无隐式填充；见 ADR 0002 的双格式管线）。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inMaterial;

layout(set = 0, binding = 0, std430) readonly buffer CameraBuffer {
    mat4 viewProjection;
} camera;

// 逐网格**模型变换**（T41 起；T33 由 `vec4` 偏移泛化为 `mat4`）：
// `modelToRender` = 平移(网格世界原点 − 渲染原点) × 旋转，把**网格局部坐标**变成**渲染原点相对坐标**。
// 由渲染侧 `SDL_PushGPUVertexUniformData(cmd, 0, ...)` **逐网格**推送（顶点 set 1 = uniform，
// 与片元 set 3 同一套 SDL3_gpu 约定，个数须与创建 Shader 时声明的 `num_uniform_buffers` 一致）。
// 两个好处：① 渲染原点重定基只需改这一块，**不再重传整世界的 137 个网格**（T41）；
// ② 倒塌中的刚体（位置与姿态都在变）也只需改这一块，CPU **不必**重烘焙上万顶点（T33）。
layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 局部坐标 → 渲染原点相对坐标（std140：mat4 = 4 个 vec4）
} meshTransform;

layout(location = 0) out vec3 v_relativePosition;
layout(location = 1) out vec3 v_normal;
// `flat`：材质槽位是**整面离散属性**，不做插值（插值会让三角形内出现"槽位 1.7"这种无意义的中间值）。
layout(location = 2) flat out float v_material;

void main() {
    // 网格局部坐标 → 渲染原点相对坐标（片元据此 + 渲染原点还原世界坐标）。
    const vec3 relativePosition = (meshTransform.modelToRender * vec4(inPosition, 1.0)).xyz;
    gl_Position        = camera.viewProjection * vec4(relativePosition, 1.0);
    v_relativePosition = relativePosition;
    // 法线必须跟着**同一个旋转**走（T33：倒塌中的刚体会翻滚）——矩阵无缩放，故 mat3 仍是纯旋转、法线保持单位长。
    v_normal           = mat3(meshTransform.modelToRender) * inNormal;
    v_material         = inMaterial;
}
