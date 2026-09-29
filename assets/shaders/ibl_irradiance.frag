#version 450

// IBL 烘焙之一：**漫反射 irradiance**（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
//
// 输出 = 32×16 的等距柱状贴图，每个 texel 存该方向上的**余弦卷积辐照度**：
//     E(N) = (1/π) · ∫_Ω L(ω) · max(N·ω, 0) dω
// 前面那个 1/π 是刻意保留的：本项目的光照口径把 1/π 折进强度常量（见 mesh.frag 头部），
// 因此"常数天空 L"经本表得到 `E = L`，与旧版半球天空光的 `skyRadiance = color × intensity` **亮度可比**（不引入 π 倍偏差）。
//
// 采样方式：**余弦加权**重要性采样（pdf = cos/π）⇒ E 的无偏估计就是 `(1/N) Σ L(ω_i)`（1/π 与 pdf 相消）。
// 采样序固定（Hammersley + 固定样本数）⇒ 同一张 HDRI 每次烘焙得到**逐位相同**的结果（红线 7）。
//
// 顶点阶段复用 `tonemap.vert`；等距柱状映射与 sky.frag **逐字一致**（改一处必须同步另一处）。

const float kPi = 3.14159265358979323846;
const uint  kSampleCount = 64u * 32u;  // 2048 个方向：对 32×16 的输出足够（漫反射是极低频）

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_source;

/// 等距柱状的逆映射：UV → 方向（v = 0 在天顶，与 sky.frag 一致）。
vec3 EquirectToDirection(vec2 uv) {
    const float phi   = (uv.x - 0.5) * 2.0 * kPi;   // 方位角
    const float theta = uv.y * kPi;                 // 极角（0 = 天顶）
    const float sinTheta = sin(theta);
    return vec3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

float RadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint count) {
    return vec2(float(i) / float(count), RadicalInverse(i));
}

void main() {
    const vec3 normal = EquirectToDirection(v_uv);

    // 以 normal 为 z 轴构造正交基（与重要性采样配合即可，不要求与纹理 UV 对齐）。
    const vec3 up = (abs(normal.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent = normalize(cross(up, normal));
    const vec3 bitangent = cross(normal, tangent);

    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < kSampleCount; ++i) {
        const vec2 xi = Hammersley(i, kSampleCount);
        // 余弦加权半球采样：r = sqrt(xi.x)、phi = 2π·xi.y。
        const float r = sqrt(xi.x);
        const float phi = 2.0 * kPi * xi.y;
        const vec3 localSample = vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - xi.x, 0.0)));
        const vec3 direction = tangent * localSample.x + bitangent * localSample.y + normal * localSample.z;

        const float u = atan(direction.z, direction.x) / (2.0 * kPi) + 0.5;
        const float v = acos(clamp(direction.y, -1.0, 1.0)) / kPi;
        sum += texture(u_source, vec2(u, v)).rgb;
    }

    o_color = vec4(sum / float(kSampleCount), 1.0);
}
