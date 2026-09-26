#version 450

// 地表 / 体积网格的顶点着色器（真正的网格路径，非 PoC 冒烟三角形）。
//
// 顶点输入与 engine/render/mesh_renderer.hpp 的 MeshVertex 布局一一对应：
//   location 0 `vec3 position`        —— **相机相对坐标**：世界定位用整数 / double，
//                                        上传 GPU 前已完成相对偏移（红线 6），
//                                        因此这里不再出现任何大数值 world 坐标。
//   location 1 `vec3 normal`          —— 世界空间单位法线（由高度场梯度算出，禁止面法线近似）。
//   location 2 `vec4 materialWeights` —— splat 权重，x~w 依次对应 4 个材质槽位。
//
// 相机常量：SDL3_gpu 的 SPIR-V 约定 —— 顶点着色器 set 0 放只读资源（采样器 / 存储缓冲），
// set 1 放 uniform。渲染侧用 SDL_BindGPUVertexStorageBuffers(pass, 0, ...) 绑定该缓冲，
// 故这里声明为 set = 0、binding = 0 的只读 storage buffer，布局与 CameraUniform 一致
// （std430 下 mat4 即 4 个 vec4，无隐式填充；见 ADR 0002 的双格式管线）。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inMaterialWeights;

layout(set = 0, binding = 0, std430) readonly buffer CameraBuffer {
    mat4 viewProjection;
} camera;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_materialWeights;

void main() {
    gl_Position       = camera.viewProjection * vec4(inPosition, 1.0);
    v_normal          = inNormal;
    v_materialWeights = inMaterialWeights;
}
