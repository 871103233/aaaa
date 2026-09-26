#version 450

// 色调映射片元着色器（T20 / ADR 0010 P0）：HDR 线性颜色 → 曝光 → ACES 近似 → sRGB 编码。
//
// 目的：主通道改渲到 `R16G16B16A16_FLOAT` 的离屏 HDR 目标后，亮度可以超过 1.0；
// 本通道先用曝光缩放，再经 ACES 曲线压到 [0,1)，最后按 sRGB 编码写交换链
// （交换链是 SDR 的 8 位目标，若不编码则整体偏暗）。
//
// 绑定约定（SDL3_gpu 的 SPIR-V 资源集，与 mesh.frag 一致）：
//   set = 2、binding = 0：采样纹理（主通道的 HDR 颜色目标）
//   set = 3、binding = 0：片元 uniform 块，由 SDL_PushGPUFragmentUniformData(..., slot 0, ...) 填入。
//
// 曝光值来自 settings.toml（经 engine/platform/settings.hpp 读入 → MeshRenderer::SetExposure），
// **不在此另写一份**（ADR 0010：参数进配置，改值不需重编 Shader）。

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_hdr;

layout(set = 3, binding = 0, std140) uniform TonemapBlock {
    vec4 exposureParams;  // x = 曝光系数（其余未用；std140 下用 vec4 以满足 16 字节对齐）
} tonemap;

/// ACES 电影曲线近似（Narkowicz 2015）：把 [0, ∞) 的线性 HDR 压到 [0, 1]。
vec3 acesApprox(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

/// 线性 → sRGB 编码（**分段精确曲线**，非 pow(1/2.2) 近似）：
/// 暗部线性段（c ≤ 0.0031308 用 12.92·c），亮部幂函数段（1.055·c^(1/2.4) − 0.055）。
vec3 linearToSrgb(vec3 c) {
    const vec3 low  = c * 12.92;
    const vec3 high = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), c));
}

void main() {
    const vec3 hdr    = texture(u_hdr, v_uv).rgb;
    const vec3 mapped = acesApprox(hdr * tonemap.exposureParams.x);
    o_color = vec4(linearToSrgb(mapped), 1.0);
}
