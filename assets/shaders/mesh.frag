#version 450

// 地表 / 体积网格的片元着色器（ADR 0009：权重逐像素算、外观用分层贴图）。
//
// 与旧实现（顶点权重 + 4 个占位纯色）的区别：
//   1. **权重逐像素重算**：按世界高度与坡度（坡度由插值法线求得）求各层权重，
//      过渡带是**窄带** smoothstep —— 带宽由这里的高度 / 坡度带参数决定，
//      不再受"顶点间距 1 格"限制（旧实现把整段变化摊在约 1 格宽的三角形里，形成宽带）。
//      公式与 CPU 侧 world/terrain/material_blender.cpp 的 ComputeBlendWeights 逐字镜像。
//   2. **外观用分层贴图**：albedo + 法线两张纹理数组（4 层），每层各自的 UV 尺度；
//      叠加高频细节噪声打散平铺重复；**法线贴图是棱角立体感的主要来源**。
//
// 世界坐标还原：顶点是相机相对坐标（红线 6），这里加上 uniform 的**渲染原点**得到世界坐标，
// 才能按世界高度算权重。
//
// 绑定约定（SDL3_gpu 的 SPIR-V 资源集）：
//   set 2 = 片元采样纹理（binding 0 = albedo 数组，binding 1 = 法线数组）
//   set 3 = 片元 uniform 块（binding 0），由 SDL_PushGPUFragmentUniformData(..., slot 0, ...) 填入。
// 这些数值全部来自 assets/config/materials.toml（经 BuildMaterialUniform 投影），**不在此另写一份**。

layout(location = 0) in vec3 v_relativePosition;
layout(location = 1) in vec3 v_normal;

layout(location = 0) out vec4 o_color;

/// 材质槽位数量：与材质表行数 / 纹理数组层数一致。
const int kMaterialLayerCount = 4;

/// 可采样的层数上限（当前 4 = 全部）。**这是性能的第一旋钮**：
/// 纹理带宽是逐像素混合的主要开销（每层 albedo + 法线 = 2 次带过滤的采样）；
/// 层数增长后应在此裁剪到权重最高的 3~4 层（ADR 0009 后果一节）。
const int kMaxSampledLayers = 4;

/// 近似零权重：低于此值直接跳过采样（不改变结果，只省带宽）。
const float kWeightEpsilon = 1.0 / 1000000.0;

/// 方向光：固定一束斜向下的日光（单位向量），只为让起伏与坡度可见（方案 §4.4 的方向光项）。
const vec3 kLightDirection = normalize(vec3(0.45, 0.80, 0.30));

/// 高频细节：扰动 UV，打散平铺重复与"机器般等距"的纹理边界。
const float kDetailFrequency = 0.35;
const float kDetailStrength  = 0.06;

layout(set = 2, binding = 0) uniform sampler2DArray u_albedo;
layout(set = 2, binding = 1) uniform sampler2DArray u_normal;

struct MaterialLayerParams {
    vec4 height;  // x = min, y = max, z = blend, w = 纹理数组层号
    vec4 slope;   // x = min, y = max, z = blend, w = 未用
    vec4 tintUv;  // rgb = 层色 tint, a = 每层 UV 尺度
};

layout(set = 3, binding = 0, std140) uniform MaterialBlock {
    vec4 renderOrigin;  // xyz = 渲染原点（世界坐标），片元用它把相机相对位置还原为世界坐标
    MaterialLayerParams layers[kMaterialLayerCount];
} material;

/// 一条「带」的隶属度：带内为 1，带外经 blend 宽的窄带平滑阶跃归零。
/// 逐字镜像 world/terrain/material_blender.cpp 的 MaterialBandFactor（两边改动必须同步）。
float bandFactor(float value, float lo, float hi, float blend) {
    if (blend <= 0.0) {
        return (value >= lo && value <= hi) ? 1.0 : 0.0;
    }
    return smoothstep(lo - blend, lo, value) * (1.0 - smoothstep(hi, hi + blend, value));
}

/// 全部槽位的归一化权重；镜像 CPU 侧 ComputeBlendWeights（含"无匹配 → 槽位 0"的退化）。
vec4 computeWeights(float height, float slope) {
    vec4  weights = vec4(0.0);
    float total   = 0.0;
    for (int i = 0; i < kMaterialLayerCount; ++i) {
        const float heightFactor =
            bandFactor(height, material.layers[i].height.x, material.layers[i].height.y, material.layers[i].height.z);
        const float slopeFactor =
            bandFactor(slope, material.layers[i].slope.x, material.layers[i].slope.y, material.layers[i].slope.z);
        const float weight = heightFactor * slopeFactor;
        weights[i] = weight;
        total += weight;
    }
    if (total <= kWeightEpsilon) {
        weights = vec4(0.0);
        weights[0] = 1.0;
        return weights;
    }
    return weights / total;
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

/// 便宜的 value noise（仅用于 UV 扰动，不需要高质量）。
float valueNoise(vec2 p) {
    const vec2  cell  = floor(p);
    const vec2  frac  = fract(p);
    const vec2  curve = frac * frac * (3.0 - 2.0 * frac);
    const float a = hash21(cell);
    const float b = hash21(cell + vec2(1.0, 0.0));
    const float c = hash21(cell + vec2(0.0, 1.0));
    const float d = hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, curve.x), mix(c, d, curve.x), curve.y);
}

void main() {
    const vec3  worldPosition   = v_relativePosition + material.renderOrigin.xyz;
    const vec3  geometricNormal = normalize(v_normal);
    const float slope           = clamp(1.0 - geometricNormal.y, 0.0, 1.0);
    const vec4  weights         = computeWeights(worldPosition.y, slope);

    const vec2 detail = vec2(valueNoise(worldPosition.xz * kDetailFrequency),
                             valueNoise(worldPosition.xz * kDetailFrequency + vec2(13.7, 71.3))) - 0.5;

    vec3 albedo        = vec3(0.0);
    vec3 tangentNormal = vec3(0.0);
    for (int i = 0; i < kMaxSampledLayers; ++i) {
        const float weight = weights[i];
        if (weight <= kWeightEpsilon) {
            continue;  // 近似零权重：跳过采样（省带宽；ADR 0009 的第一性能旋钮）
        }
        const MaterialLayerParams layer = material.layers[i];
        const vec2 uv = worldPosition.xz * layer.tintUv.a + detail * kDetailStrength;

        const vec3 layerAlbedo = texture(u_albedo, vec3(uv, layer.height.w)).rgb;
        const vec3 layerNormal = texture(u_normal, vec3(uv, layer.height.w)).rgb * 2.0 - 1.0;

        albedo += layerAlbedo * layer.tintUv.rgb * weight;
        tangentNormal += layerNormal * weight;
    }

    // 无自洽切线：由几何法线与参考轴构造正交 TBN（高度场足够，不需要逐顶点切线）。
    const float tangentLengthSq = dot(tangentNormal, tangentNormal);
    const vec3  safeTangentNormal =
        (tangentLengthSq > 1e-8) ? tangentNormal * inversesqrt(tangentLengthSq) : vec3(0.0, 0.0, 1.0);

    const vec3 reference = (abs(geometricNormal.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent   = normalize(cross(reference, geometricNormal));
    const vec3 bitangent = cross(geometricNormal, tangent);
    const vec3 mappedNormal = normalize(tangent * safeTangentNormal.x + bitangent * safeTangentNormal.y +
                                        geometricNormal * safeTangentNormal.z);

    // 环境项 + 漫反射项：避免背光面纯黑，同时保留坡度与法线细节的明暗。
    const float diffuse = max(dot(mappedNormal, kLightDirection), 0.0);
    o_color = vec4(albedo * (0.35 + 0.65 * diffuse), 1.0);
}
