#version 450

// IBL 烘焙之二：**预过滤高光**（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
//
// 按 **GGX 重要性采样**把环境贴图卷积成"按粗糙度分级"的 mip 链：mip i ⇒ roughness = i / (mipCount − 1)。
// 运行期配合 BRDF LUT 使用（split-sum）：`环境高光 = prefiltered(R, roughness) · (F0·A + B)`。
//
// 与标准实现（Karis / UE4 "Real Shading in UE4"）的差异（**已登记**）：
//   本实现**不做** mip 选择的 pdf 近似（源贴图只有第 0 级）⇒ 低粗糙度 + 大输出时可能有轻微噪声；
//   代价是省掉源贴图的 mip 链与其带宽。切换条件：目视发现高光有可见噪点 ⇒ 给源贴图生成 mip 并按 pdf 选级。
//
// 采样序固定（Hammersley + 固定样本数）⇒ 同一张 HDRI 每次烘焙得到**逐位相同**的结果（红线 7）。
// 顶点阶段复用 `tonemap.vert`；等距柱状映射与 sky.frag **逐字一致**。

const float kPi = 3.14159265358979323846;
const uint  kSampleCount = 256u;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_source;

layout(set = 3, binding = 0, std140) uniform PrefilterBlock {
    vec4 params;  // x = 本 mip 对应的 roughness（0 = 镜面）
} prefilter;

vec3 EquirectToDirection(vec2 uv) {
    const float phi   = (uv.x - 0.5) * 2.0 * kPi;
    const float theta = uv.y * kPi;
    const float sinTheta = sin(theta);
    return vec3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

vec2 DirectionToEquirect(vec3 direction) {
    return vec2(atan(direction.z, direction.x) / (2.0 * kPi) + 0.5,
                acos(clamp(direction.y, -1.0, 1.0)) / kPi);
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

/// 重要性采样 GGX 半向量（Karis 式：`cosθ = sqrt((1−u)/(1+(α²−1)u))`）。
vec3 ImportanceSampleGgx(vec2 xi, float roughness, vec3 normal) {
    const float alpha = roughness * roughness;
    const float alphaSq = alpha * alpha;

    const float phi = 2.0 * kPi * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alphaSq - 1.0) * xi.y));
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    const vec3 halfLocal = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    const vec3 up = (abs(normal.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent = normalize(cross(up, normal));
    const vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * halfLocal.x + bitangent * halfLocal.y + normal * halfLocal.z);
}

void main() {
    const vec3 normal = EquirectToDirection(v_uv);
    // split-sum 的第一项按 **N = V = R** 的假设预过滤（Karis）⇒ 采样时只需法线。
    const vec3 viewDirection = normal;

    const float roughness = clamp(prefilter.params.x, 0.0, 1.0);

    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < kSampleCount; ++i) {
        const vec2 xi = Hammersley(i, kSampleCount);
        const vec3 halfVector = ImportanceSampleGgx(xi, roughness, normal);
        const vec3 lightDirection = reflect(-viewDirection, halfVector);

        const float nDotL = dot(normal, lightDirection);
        if (nDotL > 0.0) {
            sum += texture(u_source, DirectionToEquirect(lightDirection)).rgb * nDotL;
            weight += nDotL;
        }
    }

    o_color = vec4((weight > 0.0) ? sum / weight : texture(u_source, v_uv).rgb, 1.0);
}
