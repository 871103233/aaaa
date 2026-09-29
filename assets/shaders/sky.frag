#version 450

// 天空通道的片元着色器（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
//
// 做什么：全屏三角形的每个像素 → 用**逆视图投影**把 NDC 变成**渲染相对空间**的一条视线方向 →
// 按**等距柱状（equirectangular）**投影采样 HDRI → 写入 HDR 目标（随后由 tonemap.frag 做曝光 + 色调映射）。
//
// 为什么可以省略相机位置：本项目的顶点与矩阵都在**渲染相对坐标系**里（顶点减去渲染原点，红线 6），
// 相机在该坐标系中**恒在原点**（见 mesh.vert 的约定与 T41）⇒ 视线方向 = 反投影得到的远平面点方向，无需相机位置。
//
// 投影口径（与 `world/terrain/...` 无关，纯几何约定；**必须与烘焙通道的一致**，见 ibl_*.frag）：
//   u = atan2(dir.z, dir.x) / 2π + 0.5   （方位角：+X 为 0.5，逆时针）
//   v = acos(clamp(dir.y, -1, 1)) / π    （极角：**v = 0 在天顶**、v = 1 在天底 —— 与 HDRI 文件的常规朝向一致）
//
// 绑定约定（与既有通道一致，见 ADR 0002 的双格式管线）：
//   set 2 = binding 0：等距柱状 HDRI（采样器：线性过滤 + 重复 U / 钳制 V）
//   set 3 = binding 0：片元 uniform（单个 mat4 = 逆视图投影）
// 顶点阶段复用 `tonemap.vert`（全屏三角形、无顶点缓冲；两者的 v_uv 约定必须一致）。

const float kPi = 3.14159265358979323846;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_sky;

layout(set = 3, binding = 0, std140) uniform SkyBlock {
    mat4 inverseViewProjection;  // 相机视投影矩阵的逆（作用在**渲染相对**坐标上）
} sky;

void main() {
    // NDC：x 右、y 上（SDL_gpu 的裁剪空间，见 tonemap.vert 的 V 翻转说明）。
    const vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0);

    // 远平面点（w = 1）→ 方向。相机在渲染相对空间的原点，故方向 = 归一化后的远点坐标。
    const vec4 farPoint = sky.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    const vec3 direction = normalize(farPoint.xyz / max(farPoint.w, 1e-6));

    const float u = atan(direction.z, direction.x) / (2.0 * kPi) + 0.5;
    const float v = acos(clamp(direction.y, -1.0, 1.0)) / kPi;

    // 线性光：HDRI 已是线性辐照度（`LoadImageHdr` 的输出口径），**不做 sRGB 转换**。
    o_color = vec4(texture(u_sky, vec2(u, v)).rgb, 1.0);
}
