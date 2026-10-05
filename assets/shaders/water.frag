#version 450

// W6 **水面**片元着色器（[ADR 0027](../../docs/adr/0027-water-representation.md)）：**flow 着色**。
//
// 做什么：按**世界坐标 + 时间**的两组滚动波扰动水面法线，叠加菲涅尔边缘与环境色，输出**半透明**水面。
// 明确**不做**：流体模拟 / 折射求解 / 泡沫 / 水下后处理（ADR 0027 已登记的"本阶段不做"）。
//
// 顶点阶段**复用 `mesh.vert`**（同一套顶点布局与逐网格变换）⇒ 输入与它逐字一致。
// uniform：`set = 3, binding = 0`（std140）。渲染侧在**水面通道之前**用
// `SDL_PushGPUFragmentUniformData(cmd, 0, ...)` 推送本块（槽 0 平时被材质占用，
// 而水面在**主通道最后**绘制 ⇒ 覆盖它不影响任何已有绘制）。

layout(location = 0) in vec3 v_relativePosition;
layout(location = 1) in vec3 v_normal;

layout(set = 3, binding = 0, std140) uniform WaterBlock {
    vec4 params;  // x = 时间（秒）；y = 流速（时间倍率）；z / w = 预留
    vec4 origin;  // 渲染原点（世界坐标的 float 近似；与 SetRenderOrigin 一致）
} water;

layout(location = 0) out vec4 o_color;

void main() {
    // 渲染相对坐标 + 渲染原点 ⇒ 世界坐标（与 mesh.frag 同一口径）。
    const vec3 world = v_relativePosition + water.origin.xyz;

    // ---- flow：两组方向不同的滚动波（叠加成"流动"的外观）----
    const vec2  dir1 = normalize(vec2(0.9, 0.4));
    const vec2  dir2 = normalize(vec2(-0.5, 0.8));
    const float f1   = 0.35;
    const float f2   = 0.52;
    const float t    = water.params.x * water.params.y;
    const float phase1 = dot(world.xz, dir1) * f1 + t * 1.7;
    const float phase2 = dot(world.xz, dir2) * f2 - t * 1.3;
    const float wave   = (sin(phase1) + sin(phase2)) * 0.5;

    // 法线扰动 = 两个正弦的相位梯度（沿世界 XZ）。
    const vec2 grad = dir1 * (f1 * cos(phase1)) + dir2 * (f2 * cos(phase2));
    const vec3 normal = normalize(v_normal + vec3(grad.x, 0.0, grad.y) * 0.9);

    // ---- 视角：渲染相对坐标下相机在原点附近（与水面体量相比足够近似）----
    const vec3  view    = normalize(-v_relativePosition);
    const float fresnel = pow(1.0 - clamp(dot(normal, view), 0.0, 1.0), 3.0);

    // ---- 颜色：深水 → 浅水，叠菲涅尔与环境色，再加方向光高光 ----
    const vec3 deep    = vec3(0.04, 0.14, 0.20);
    const vec3 shallow = vec3(0.14, 0.38, 0.46);
    vec3       color   = mix(deep, shallow, clamp(wave * 0.5 + 0.5, 0.0, 1.0));
    color              = mix(color, vec3(0.62, 0.80, 0.92), fresnel * 0.85);

    const vec3  sunDir = normalize(vec3(0.4, 0.8, 0.3));
    const float spec   = pow(max(dot(normal, normalize(sunDir + view)), 0.0), 96.0);
    color += vec3(1.0, 0.97, 0.90) * spec * 0.9;

    // 半透明：正视更透、掠射更实（菲涅尔）。
    o_color = vec4(color, mix(0.60, 0.90, fresnel));
}
