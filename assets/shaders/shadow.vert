#version 450

// 阴影通道顶点着色器（T21b / ADR 0010 P1 第二步）：纯深度写入通道。
// 配套的 `shadow.frag` 只是空入口（SDL_gpu 不允许管线片元着色器为 null），无任何计算。
//
// 顶点输入与 engine/render/mesh_renderer.hpp 的 MeshVertex 布局一致：
//   location 0 `vec3 position` —— **相机相对坐标**（红线 6：世界定位用整数 / double，
//                                上传 GPU 前已完成相对偏移）。这里只消费位置，法线不参与深度写入。
//
// 光空间矩阵的传递机制与 mesh.vert **同类**：都用 SDL3_gpu 的**顶点只读 storage buffer**
// （渲染侧 `SDL_BindGPUVertexStorageBuffers(pass, 0, ...)` 绑定，故声明为 set = 0、binding = 0）。
// 差别只在于 mesh.vert 绑的是相机视图投影矩阵、本通道绑的是**该级联的光空间矩阵**；
// 每级渲染通道绑定各自的矩阵缓冲（SDL_gpu 的 storage buffer 绑定不带偏移，见 SDL_gpu.h），
// 因此这里只声明**一个** mat4 —— 每个渲染通道看到的都是"当前级联"的矩阵。
// **不引入第二种传递机制**（不用 push uniform），与相机矩阵的路径保持一致。
//
// 矩阵的坐标系：与顶点同为**渲染原点相对**（构造方式见 engine/render/shadow_cascade.hpp 的
// `BuildCascadeLightMatrix` 说明），因此这里直接 `矩阵 × vec4(position, 1)`，
// **不做任何额外翻转 / 平移补偿**；多余的补偿会让阴影整体错位。
//
// 输出深度：正交投影产出的 NDC z ∈ [0, 1]（SDL_gpu 的深度约定，近平面 = 0），
// 直接写入 D32_FLOAT 深度数组的对应层。

layout(location = 0) in vec3 inPosition;

layout(set = 0, binding = 0, std430) readonly buffer LightMatrixBuffer {
    mat4 lightMatrix;
} light;

void main() {
    gl_Position = light.lightMatrix * vec4(inPosition, 1.0);
}
