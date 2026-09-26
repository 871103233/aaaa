#version 450

// 地表 / 体积网格的顶点着色器（真正的网格路径，非 PoC 冒烟三角形）。
//
// 顶点输入与 engine/render/mesh_renderer.hpp 的 MeshVertex 布局一一对应：
//   location 0 `vec3 position` —— **相机相对坐标**：世界定位用整数 / double，
//                                上传 GPU 前已完成相对偏移（红线 6），
//                                因此这里不再出现任何大数值 world 坐标。
//   location 1 `vec3 normal`   —— 世界空间单位法线（由高度场梯度算出，禁止面法线近似）。
//   location 2 `float material` —— **材质槽位覆盖**（ADR 0014）：
//                                 < 0 = 未指定（片元按世界高度 / 坡度逐像素算权重，地表走这条）；
//                                 >= 0 = 直接用该槽位（可挖体积的内表面走这条）。
//
// **不再**传递材质权重：ADR 0009 起权重由片元着色器逐像素按世界高度与坡度重算。
// 片元需要世界坐标才能算权重，故这里只传出相机相对位置；片元用 uniform 的渲染原点把它还原为世界坐标。
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

layout(location = 0) out vec3 v_relativePosition;
layout(location = 1) out vec3 v_normal;
// `flat`：材质槽位是**整面离散属性**，不做插值（插值会让三角形内出现"槽位 1.7"这种无意义的中间值）。
layout(location = 2) flat out float v_material;

void main() {
    gl_Position        = camera.viewProjection * vec4(inPosition, 1.0);
    v_relativePosition = inPosition;
    v_normal           = inNormal;
    v_material         = inMaterial;
}
