#version 450

// 地表 / 体积网格的片元着色器。
//
// V0.1 只做两件事：
//   1. 按 splat 权重把 4 个**占位颜色**混合起来（其顺序与 assets/config/materials.toml
//      的槽位一致：草 / 土 / 岩 / 沙），让"高度 + 坡度 → 材质过渡"肉眼可见；
//      真实纹理数组是后续任务（方案 §4.3），此处刻意不引入任何纹理采样。
//   2. 方向光漫反射（方案 §4.4：方向光 + 级联阴影；此处只做方向光项）。
//
// 法线来自顶点属性（由高度场梯度算出），**不**用面法线近似。

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_materialWeights;

layout(location = 0) out vec4 o_color;

// 占位颜色：槽位 0~3 依次为草 / 土 / 岩 / 沙。
const vec3 kSlotColors[4] = vec3[](
    vec3(0.31, 0.58, 0.24),  // 草
    vec3(0.44, 0.32, 0.20),  // 土
    vec3(0.52, 0.51, 0.50),  // 岩
    vec3(0.83, 0.74, 0.48)   // 沙
);

// 方向光：固定一束斜向下的日光（单位向量），只为让起伏与坡度可见。
const vec3 kLightDirection = normalize(vec3(0.45, 0.80, 0.30));

void main() {
    vec3 blended = vec3(0.0);
    for (int slot = 0; slot < 4; ++slot) {
        blended += kSlotColors[slot] * v_materialWeights[slot];
    }

    // 环境项 + 漫反射项：避免背光面纯黑，同时保留坡度明暗。
    const float diffuse = max(dot(normalize(v_normal), kLightDirection), 0.0);
    o_color = vec4(blended * (0.35 + 0.65 * diffuse), 1.0);
}
