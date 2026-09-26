#version 450

// 地表 / 体积网格的片元着色器。
//
// 演进：ADR 0009（权重逐像素算 + 分层 albedo/法线，T19）→ ADR 0010 P1（方向光 + CSM + 半球天空光 + 雾，T21）
//       → ADR 0010 P2（PBR Cook-Torrance + 材质四件套 + 宏观变化，T22）→ **C 项（陡壁三平面混合投影）**。
//
// 本版做什么：
//   1. **权重逐像素重算**（不变）：按世界高度与坡度求各层权重，过渡带是**窄带** smoothstep。
//      公式与 CPU 侧 world/terrain/material_blender.cpp 的 ComputeBlendWeights 逐字镜像。
//   2. **材质四件套**：albedo / normal / roughness / AO 四张纹理数组（各 4 层），每层各自的 UV 尺度。
//      **只对权重最高的 3~4 层采样**，且四件套的采样**全部落在同一个 `weight > kWeightEpsilon` 守卫内**
//      —— 近似零权重直接 `continue`，不为低权重层付出采样带宽（ADR 0009 的第一性能旋钮）。
//   3. **宏观变化**：按每层的 `macro_uv_scale` 采样**单层** macro 图，用 `macro_strength`
//      调制 albedo 与 roughness，打破 10 格量级的平铺重复感。
//   4. **PBR（Cook-Torrance）**：GGX 法线分布 D + Smith 几何项 G + Schlick 菲涅尔 F，电介质 `F0 = 0.04`。
//   5. **多频细节法线**（T23 / ADR 0010 P3）：在既有 normal 贴图之上再叠**第二频段**——同一张 normal 图
//      取**另一个（更高频的）UV 尺度**；合成方式为 RNM（见下）。**只对权重最高的层**做这一次额外采样，
//      不为每层都加采样（带宽硬约束：材质采样是最大的一笔带宽，见 tech-plan §7.2.2）。
//   6. **三平面混合投影（C 项）**：陡壁上的平面（+Y 轴）投影会把纹理拉长。这里对**权重最高的那一层**、
//      按**逐像素由世界空间几何法线**算出的混合权重，在三个轴投影之间混合（见 triplanarWeight / 采样段）。
//      平地路径（混合权重 ≈ 0）**早退**为单次平面投影，采样次数与旧版完全相同；参数全部来自材质表的
//      `[triplanar]` 段（经 BuildMaterialUniform 投影），此处不留第二份常量。
//
// PBR 公式（每片元；`N` 为几何 + 法线贴图后的世界法线，`L` 由地表指向太阳，`V` 由地表指向相机，
// `H = normalize(L + V)`，`α = roughness²`）：
//   D（GGX / Trowbridge-Reitz）  = α² / (π · ( (N·H)²·(α²−1) + 1 )²)
//   G（Smith，Schlick-GGX 近似）= G₁(N·L) · G₁(N·V)，G₁(x) = x / (x·(1−k) + k)，k = (r+1)² / 8
//   F（Schlick）                 = F0 + (1 − F0) · (1 − V·H)⁵
//   镜面 BRDF                    = D · G · F / (4 · (N·L) · (N·V))
//   漫反射 BRDF                  = kD · albedo，kD = 1 − F（无金属，metallic = 0）
//   出射 = (漫反射 + 镜面) · 入射辐照度 · (N·L)，再加环境项（见下）
//
// F0 = 0.04 的依据：常见电介质（塑料 / 石 / 土 / 草 / 沙）在可见光波段的垂直入射反射率约 4%，
// 是实时 PBR 的通行占位值；**地表无金属**，故不引入 metallic 也不使用金属的 F0 = albedo 形式。
//
// 漫反射的 1/π 约定：本项目把 1/π 折进光照强度常量（`lighting.toml` 的 `intensity` 即为该口径），
// 与 P1 的 Lambert（`albedo · sunColor · intensity · (N·L)`）**亮度对齐**，因此本版落地**不需要**
// 重新校准 `lighting.toml`；镜面项仍为标准 Cook-Torrance 形式。
//
// ---- 中值色调推算（本组配置：太阳强度 1.30 / 天空强度 1.00 / 曝光 1.0 / 阴影 = 0）----
// 本表是**估算**：取贴图的中间灰度纹素（草 0.86 / 沙 0.89 / 岩 0.66）、宏观乘子与粗糙度乘子均取 1.0，
// 经 `tonemap.frag` 的曝光 + ACES 近似 + sRGB 编码后的屏上落点（0 = 黑、1 = 白）：
//
//   场景 A —— **受光面（N = +Y，N·L = 0.83）+ 掠射视角（N·V = 0.20，V·H ≈ 0.60 使 F ≈ 0.050）**：
//     草地 ≈ (0.31, 0.63, 0.23)    沙地 ≈ (0.84, 0.80, 0.60)    岩石 ≈ (0.45, 0.45, 0.48)
//     各通道最高 0.84（沙地红），**不出现整屏死白**；植被 / 沙地粗糙度 0.90 / 0.95 ⇒ 高光很宽、近乎哑光。
//   场景 B —— **岩石在镜面峰值方向（H = N）**：
//     无高光 ≈ (0.44, 0.44, 0.48)  →  含高光 ≈ (0.69, 0.68, 0.67)
//     粗糙度 0.40（三档中最低）使岩石的 GGX 瓣最窄，只有岩石在镜面方向出现**可分辨的方向性高光**：
//     红通道从 0.44 抬到 0.69（+0.25 屏上亮度），远未到 1.0，故**不是死白**。
//   结论：草地 / 沙地保持哑光，**岩石出现可分辨的方向性高光且不出现死白**（T22 验收判据）。
//   若调高 / 调低 `lighting.toml` 的强度或 `materials.toml` 的 roughness / tint，必须同步复核本表。
//
// 世界坐标还原：顶点是相机相对坐标（红线 6），这里加上 uniform 的**渲染原点**得到世界坐标。
//
// 绑定约定（SDL3_gpu 的 SPIR-V 资源集；每阶段采样器上限 16，见 SDL_gpu.h，故 6 个采样器无需打包）：
//   set 2 = 片元采样纹理：
//            binding 0 = albedo    （4 层数组）
//            binding 1 = normal    （4 层数组）
//            binding 2 = roughness （4 层数组）
//            binding 3 = ao        （4 层数组）
//            binding 4 = macro     （**1 层**数组）
//            binding 5 = shadow    （阴影深度数组，层 i = 级联 i）
//   set 3 = 片元 uniform 块（binding 0 = 材质 / 槽 0；binding 1 = 光照与雾 / 槽 1；binding 2 = 阴影 / 槽 2）。
// 材质数值来自 assets/config/materials.toml（经 BuildMaterialUniform 投影）；
// 光照 / 雾 / 阴影数值来自 assets/config/lighting.toml（经 BuildLightingUniform / BuildShadowUniform 投影）。
// **三处都不在此另写一份**。

layout(location = 0) in vec3 v_relativePosition;
layout(location = 1) in vec3 v_normal;

layout(location = 0) out vec4 o_color;

/// 材质槽位数量：与材质表行数 / 纹理数组层数一致。
const int kMaterialLayerCount = 4;

/// 可采样的层数上限（当前 4 = 全部）。**这是性能的第一旋钮**：
/// 纹理带宽是逐像素混合的主要开销（每层 albedo / 法线 / 粗糙度 / AO / 宏观 = 5 次带过滤的采样）；
/// 层数增长后应在此裁剪到权重最高的 3~4 层（ADR 0009 后果一节）。
const int kMaxSampledLayers = 4;

/// 近似零权重：低于此值直接跳过采样（不改变结果，只省带宽）。**四件套与宏观都受同一守卫保护。**
const float kWeightEpsilon = 1.0 / 1000000.0;

/// 三平面混合权重的"视为平地"阈值（C 项）：低于它**不进入三平面分支**，只做单次平面采样。
/// 与 CPU 侧口径一致（平地路径零额外开销）。
const float kTriWeightEpsilon = 1.0 / 1000.0;

/// 级联数上限：与 engine/render/shadow_cascade.hpp 的 `kMaxShadowCascades` 一致。
const int kMaxShadowCascades = 4;

// ---- PBR 常量（ADR 0010 P2）：粗糙度下限与电介质 F0 ----
//
// kMinRoughness：GGX 的 α = roughness²，roughness → 0 时 α → 0、D 的分母出现 0/0；钳一个合理下限
//   （0.045 ⇒ α ≈ 0.002，D 峰值仍有界）即可避免除零，同时保留几乎镜面的观感。
// kF0Dielectric：电介质垂直入射反射率约 4%（见文件头"F0 依据"）。
const float kMinRoughness   = 0.045;
const float kF0Dielectric   = 0.04;
const float kPi             = 3.14159265358979323846;

// ---- 细节与宏观调制（数值全部来自 uniform 或确定性的世界坐标，不留第二份配置常量）----

/// 高频细节：扰动 UV，打散平铺重复与"机器般等距"的纹理边界。
const float kDetailFrequency = 0.35;
const float kDetailStrength  = 0.06;

/// 多频细节法线（T23）：第二频段相对基础 UV 尺度的**倍数**。
/// 取 4.0 ⇒ 第二频段波长为第一频段的 1/4（明显更高频），近看不再"单一频率的平感"。
/// 第二频段复用**同一张** normal 贴图（不新增采样器 / 纹理），故不增加显存，只多一次采样。
const float kDetailNormalUvRatio = 4.0;

/// 粗糙度贴图的折算区间：贴图值 ∈ [0,1] → 乘子 ∈ [0.8, 1.2]（±20%），基准值来自材质表 `roughness`。
const float kRoughnessTexLow  = 0.8;
const float kRoughnessTexHigh = 1.2;

layout(set = 2, binding = 0) uniform sampler2DArray u_albedo;
layout(set = 2, binding = 1) uniform sampler2DArray u_normal;
layout(set = 2, binding = 2) uniform sampler2DArray u_roughness;
layout(set = 2, binding = 3) uniform sampler2DArray u_ao;
layout(set = 2, binding = 4) uniform sampler2DArray u_macro;
// 阴影深度数组（T21b）：层 i = 级联 i。采样器为 clamp 寻址 + 最近邻（见 mesh_renderer.cpp 的说明）。
layout(set = 2, binding = 5) uniform sampler2DArray u_shadow;

struct MaterialLayerParams {
    vec4 height;   // x = min, y = max, z = blend, w = 纹理数组层号
    vec4 slope;    // x = min, y = max, z = blend, w = PBR 粗糙度基准
    vec4 tintUv;   // rgb = 层色 tint, a = 每层 UV 尺度
    vec4 macroAo;  // x = 宏观 UV 尺度, y = 宏观调制强度, z = AO 系数, w = 未用（整块唯一填充位）
};

layout(set = 3, binding = 0, std140) uniform MaterialBlock {
    vec4 renderOrigin;  // xyz = 渲染原点（世界坐标），片元用它把相机相对位置还原为世界坐标
    vec4 triplanar;     // C 项：x = 启用(1/0), y = slope_min, z = slope_max, w = sharpness（来自材质表 [triplanar]）
    MaterialLayerParams layers[kMaterialLayerCount];
} material;

/// 光照与雾 uniform 块：字段排布与 CPU 侧 engine/render/lighting_table.hpp 的 `LightingUniform`
/// **逐字对应**（8 个 vec4 = 128 字节）。所有颜色在此**已是线性光**——它们是常量，由 CPU 侧
/// 一次性 `pow(c, 2.2)` 转换；而材质 albedo 来自纹理、无法预转，故仍在 main() 内逐像素转线性。
layout(set = 3, binding = 1, std140) uniform LightingBlock {
    vec4 sunDirectionIntensity;  // xyz = 由地表指向太阳的单位方向（世界空间）, w = 强度
    vec4 sunColorLinear;         // rgb = 太阳颜色（线性光）, a = 未用
    vec4 skyZenithIntensity;     // rgb = 天顶色（线性光）, a = 天空光强度
    vec4 skyHorizonLinear;       // rgb = 地平色（线性光）, a = 未用
    vec4 skyGroundLinear;        // rgb = 地面反弹色（线性光）, a = 未用
    vec4 cameraPositionWorld;    // xyz = 相机世界位置, a = 未用（指数高度雾按视距插值需要）
    vec4 fogColorDensity;        // rgb = 雾色（线性光）, a = 密度（1 / 格）
    vec4 fogParams;              // x = 启用(1/0), y = 高度衰减（1 / 格）
} lighting;

/// 级联阴影 uniform 块（T21b / ADR 0010 P1）：字段排布与 CPU 侧 engine/render/shadow_cascade.hpp 的
/// `ShadowUniform` **逐字对应**（4×mat4 + 4 个 vec4 = 320 字节）。矩阵由 game/ 每帧按相机参数构建。
layout(set = 3, binding = 2, std140) uniform ShadowBlock {
    mat4 lightMatrices[4];      // 各级光空间矩阵（**渲染原点相对坐标系**，与顶点同为相机相对坐标）
    vec4 splitDistances;        // x..w = 各级远平面（相机视距，格）
    vec4 shadowParams;          // x = 级数, y = 1/resolution（texel 尺寸，UV 单位）, z = depth_bias, w = normal_offset(格)
    vec4 cameraForwardEnabled;  // xyz = 相机世界前向（单位向量）, w = 启用(1/0)
    vec4 cascadeBlendParams;    // x = cascade_blend（级联过渡带宽度比例）, y/z/w = 未用（填充位）
} shadow;

/// 一条「带」的隶属度：带内为 1，带外经 blend 宽的窄带平滑阶跃归零。
/// 逐字镜像 world/terrain/material_blender.cpp 的 MaterialBandFactor（两边改动必须同步）。
float bandFactor(float value, float lo, float hi, float blend) {
    if (blend <= 0.0) {
        return (value >= lo && value <= hi) ? 1.0 : 0.0;
    }
    return smoothstep(lo - blend, lo, value) * (1.0 - smoothstep(hi, hi + blend, value));
}

/// 全部槽位的归一化权重；镜像 CPU 侧 ComputeBlendWeights（含"无匹配 → 槽位 0"的退化）。
///
/// ⚠ 警示：下面的兜底分支（total ≈ 0 → 槽位 0 权重 1）是**给异常输入的兜底**，不应在正常地形上触发
///   （ADR 0009 / 缺陷 2）。它被触发意味着 materials.toml 的带出现**覆盖空洞**，整片区域会被强制涂成
///   槽位 0（草）的颜色。这里与 CPU 侧逐字镜像；改动必须同步（见 world/terrain/material_blender.cpp）。
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

/// 平面 ↔ 三平面的**自动混合权重**（C 项）；逐字镜像 world/terrain/material_blender.cpp 的 TriplanarBlendWeight。
///
/// 输入 `slope = 1 - |N.y|`（由**世界空间几何法线**逐像素算出：0 = 水平面、1 = 竖直面）。
/// 返回 smoothstep(slope_min, slope_max, slope)，并受 uniform 的启用位门控（false → 0）。
///   ≈ 0 → 纯平面（平地路径，单次采样）；= 1 → 完全三平面；中间为平滑过渡。
///
/// **自动切换的根据**：地形会被笔刷挖 / 堆，法线一变这里就变，**无需任何 CPU 侧预烘焙 / 重建网格**。
float triplanarWeight(float slope) {
    if (material.triplanar.x < 0.5) {
        return 0.0;  // 关闭：整段走平面路径
    }
    return smoothstep(material.triplanar.y, material.triplanar.z, clamp(slope, 0.0, 1.0));
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

/// 级联阴影采样（T21b / 缺陷 B8）：返回**遮蔽量** ∈ [0, 1]（0 = 全亮、1 = 全暗）。
///
/// 级联选择规则：按**沿相机视轴的线性深度** `viewDepth = dot(worldPos - cameraWorld, cameraForward)`
/// 与 `shadow.splitDistances[i]` 比较，取第一个"深度 ≤ 该级远平面"的级联 i。
/// 为什么不用欧氏距离 `length(worldPos - cameraWorld)`：分割距离是沿视轴量的，
/// 视锥边缘的片元欧氏距离会大于其轴向深度，导致它提前跳到更大的级联、而该级联的包围球
/// 可能并不包含它（采样到级联外 → 阴影错误）。轴向深度与分割口径完全一致，不会出现这种越界。
///
/// **超出最远级联 → clamp 到最远级联**（缺陷 B8 要求 3）：此前返回"受光"，会使影子尾端
/// 随视角出现 / 消失；最远级联已覆盖到 `max_distance`，再远处由雾掩盖，故 clamp 后行为更稳定。
///
/// **级联边界混合**（缺陷 B8 要求 2）：在 `cascade_blend × 该级远平面` 宽的过渡带内同时采样
/// 相邻两级，权重用 `smoothstep`（与 CPU 侧 `vx::CascadeBlendWeight` 逐字镜像），**两级权重和恒为 1**；
/// `cascade_blend = 0` 时逐字退回"单级采样"（旧行为）。
///
/// 深度比较公式（正交投影，NDC z ∈ [0, 1]，近平面 = 0，与 SDL_gpu 的深度约定一致）：
///   `lightDepth = projected.z`，`closest = texture(u_shadow, ...).r`；
///   受光条件 = `lightDepth - depthBias <= closest`。3×3 PCF 内平均得到受光比例。
///
/// 偏移量量纲与抗瑕疵：
///   - `shadowParams.w` = normal_offset，**世界单位（格）**：采样点沿几何法线外移，
///     使掠射表面离开自身写入的深度 —— 这是抗 **acne（自阴影条纹）** 的主要手段。
///   - `shadowParams.z` = depth_bias，**阴影图 [0,1] 深度单位**：给比较留一点常数余量。
///
/// **V 必须翻转**：SDL_gpu 的 NDC 是"左下角 (-1,-1)、+Y 向上"，而纹理坐标是"左上角 (0,0)、+Y 向下"
/// （SDL_gpu.h §Coordinate System；后端差异由 SDL 自动转换）。因此把光空间 NDC 转纹理 UV 必须写
/// `uv = vec2(x*0.5 + 0.5, 0.5 - y*0.5)`；照直写 `y*0.5 + 0.5` 会让阴影图相对几何**上下颠倒**
/// （与 tonemap.vert 的 V 翻转同源，属于本项目缺陷 B5 的同类陷阱）。

/// 采样**单个**级联，返回遮蔽量 ∈ [0, 1]。级联外（含深度越界）→ 视为受光（0）。
float sampleCascadeShadow(int cascade, vec3 shadowPosition) {
    const vec4 lightClip = shadow.lightMatrices[cascade] * vec4(shadowPosition, 1.0);
    const vec3 projected = lightClip.xyz / lightClip.w;  // 正交投影：w 恒为 1

    if (any(lessThan(projected, vec3(-1.0))) || any(greaterThan(projected, vec3(1.0)))) {
        return 0.0;  // 级联外 → 视为受光，避免 clamp 到边缘 texel 产生假阴影
    }

    const vec2  uv    = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);  // 见上方 V 翻转说明
    const float depth = projected.z;
    const float bias  = shadow.shadowParams.z;
    const vec2  texel = vec2(shadow.shadowParams.y);

    // 3×3 PCF：取邻域 9 个深度各自比较，再平均，得到软化的阴影边缘。
    float litSamples = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float closest = texture(u_shadow, vec3(uv + vec2(float(dx), float(dy)) * texel,
                                                         float(cascade))).r;
            litSamples += (depth - bias <= closest) ? 1.0 : 0.0;
        }
    }
    return 1.0 - litSamples * (1.0 / 9.0);
}

/// 级联过渡带的**近级权重** ∈ [0, 1]（缺陷 B8）；与 CPU 侧 `vx::CascadeBlendWeight` 逐字镜像。
/// 远级权重 = `1 - 本值`，故两侧权重和**恒为 1**；`blendBand <= 0` 时恒为 1（退回单级采样）。
float cascadeBlendWeight(float viewDepth, float splitInner, float blendBand) {
    if (blendBand <= 0.0) {
        return 1.0;
    }
    const float bandInner = splitInner - blendBand;
    if (viewDepth <= bandInner) {
        return 1.0;
    }
    if (viewDepth >= splitInner) {
        return 0.0;
    }
    const float t = (viewDepth - bandInner) / blendBand;
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

float sampleShadow(vec3 relativePosition, vec3 geometricNormal, float viewDepth) {
    if (shadow.cameraForwardEnabled.w < 0.5) {
        return 0.0;  // 阴影关闭：整段跳过（不采样、不比较）
    }

    const int cascadeCount = int(shadow.shadowParams.x);
    // 缺陷 B8：默认 clamp 到最远级联（超出最远级联不再"视为受光"）。
    int cascade = cascadeCount - 1;
    for (int i = 0; i < kMaxShadowCascades; ++i) {
        if (i >= cascadeCount) {
            break;
        }
        if (viewDepth <= shadow.splitDistances[i]) {
            cascade = i;
            break;
        }
    }

    // 光空间矩阵作用于**相机相对坐标**（与顶点同坐标系），故用 v_relativePosition + 法线偏移。
    const vec3 shadowPosition = relativePosition + geometricNormal * shadow.shadowParams.w;

    // 缺陷 B8：级联边界附近同时采样相邻两级并加权混合（近级权重 + 远级权重恒为 1）。
    if (cascade + 1 < cascadeCount) {
        const float splitInner = shadow.splitDistances[cascade];
        const float blendBand  = shadow.cascadeBlendParams.x * splitInner;
        const float nearWeight = cascadeBlendWeight(viewDepth, splitInner, blendBand);
        if (nearWeight < 1.0) {
            const float nearAmount = sampleCascadeShadow(cascade, shadowPosition);
            const float farAmount  = sampleCascadeShadow(cascade + 1, shadowPosition);
            return nearWeight * nearAmount + (1.0 - nearWeight) * farAmount;
        }
    }
    return sampleCascadeShadow(cascade, shadowPosition);
}

void main() {
    const vec3  worldPosition   = v_relativePosition + material.renderOrigin.xyz;
    const vec3  geometricNormal = normalize(v_normal);
    const float slope           = clamp(1.0 - geometricNormal.y, 0.0, 1.0);
    const vec4  weights         = computeWeights(worldPosition.y, slope);

    const vec2 detail = vec2(valueNoise(worldPosition.xz * kDetailFrequency),
                             valueNoise(worldPosition.xz * kDetailFrequency + vec2(13.7, 71.3))) - 0.5;

    // ---- 材质四件套 + 宏观变化：全部在同一个 `weight > kWeightEpsilon` 守卫内采样 ----
    // 近似零权重的层**不做任何采样**（省带宽；ADR 0009 的第一性能旋钮）。
    // 多频细节法线（T23）：只在**权重最高**的那一层叠加第二频段，故先求权重最大的层号。
    // 判据 = `i == maxWeightIndex`（权重并列时取**层号较小者**，因为比较用严格 `>`）。
    int maxWeightIndex = 0;
    for (int i = 1; i < kMaxSampledLayers; ++i) {
        if (weights[i] > weights[maxWeightIndex]) {
            maxWeightIndex = i;
        }
    }

    vec3  albedo        = vec3(0.0);
    vec3  tangentNormal = vec3(0.0);
    float roughness     = 0.0;
    float ao            = 0.0;
    for (int i = 0; i < kMaxSampledLayers; ++i) {
        const float weight = weights[i];
        if (weight <= kWeightEpsilon) {
            continue;
        }
        const MaterialLayerParams layer = material.layers[i];
        const float textureLayer = layer.height.w;

        // 平面（+Y 轴）投影 UV —— 即原路径；roughness / AO / macro / 细节法线仍用它。
        const vec2 planarUv = worldPosition.xz * layer.tintUv.a + detail * kDetailStrength;

        // ---- C 项：陡壁三平面混合投影（**自动、逐像素、仅最高权重层**）----
        // 混合权重 triplanarWeight 由**世界空间几何法线**逐像素算出：地形被笔刷挖 / 堆后，受影响 tile
        // 重网格 → 顶点法线更新 → 这里自动跟随，**无需任何额外动作**（不重建材质、不重编 Shader）。
        // 平地路径（triWeight ≈ 0）**早退**为单次平面采样，采样次数与旧版完全相同（零额外开销）。
        // 取舍：三平面只作用于 albedo 与 normal（陡壁拉伸最明显的是颜色）；roughness / AO / macro / 细节
        //   法线保持平面投影（陡壁上的视觉影响小），以免每层采样次数翻三倍。
        const float triWeight = (i == maxWeightIndex) ? triplanarWeight(1.0 - abs(geometricNormal.y)) : 0.0;

        vec3 layerAlbedo = vec3(0.0);
        vec3 layerNormal = vec3(0.0, 0.0, 1.0);
        if (triWeight > kTriWeightEpsilon) {
            // 三轴权重：以 triWeight 在"纯 +Y（平面）"与"|N|^sharpness"之间插值——
            //   triWeight = 0 时退化为纯 +Y（与平面路径逐点一致，切换无跳变）；
            //   triWeight = 1 时按 |N| 的幂在三个轴投影间混合（sharpness 越大越只取最贴合的轴）。
            const vec3 axisRaw = mix(vec3(0.0, 1.0, 0.0),
                                     pow(abs(geometricNormal), vec3(material.triplanar.w)), triWeight);
            const vec3 axisWeight = axisRaw / max(axisRaw.x + axisRaw.y + axisRaw.z, 1e-5);

            const vec2 uvX = worldPosition.zy * layer.tintUv.a + detail * kDetailStrength;
            const vec2 uvZ = worldPosition.xy * layer.tintUv.a + detail * kDetailStrength;
            layerAlbedo = axisWeight.x * texture(u_albedo, vec3(uvX, textureLayer)).rgb +
                          axisWeight.y * texture(u_albedo, vec3(planarUv, textureLayer)).rgb +
                          axisWeight.z * texture(u_albedo, vec3(uvZ, textureLayer)).rgb;
            layerNormal = axisWeight.x * (texture(u_normal, vec3(uvX, textureLayer)).rgb * 2.0 - 1.0) +
                          axisWeight.y * (texture(u_normal, vec3(planarUv, textureLayer)).rgb * 2.0 - 1.0) +
                          axisWeight.z * (texture(u_normal, vec3(uvZ, textureLayer)).rgb * 2.0 - 1.0);
        } else {
            layerAlbedo = texture(u_albedo, vec3(planarUv, textureLayer)).rgb;
            layerNormal = texture(u_normal, vec3(planarUv, textureLayer)).rgb * 2.0 - 1.0;
        }
        // 多频细节法线（T23 / ADR 0010 P3）：**仅权重最高的层**再采一次同一张 normal 图，UV 尺度 ×4（高频）。
        // 合成方式 = RNM（Reoriented Normal Mapping，Whiteout 变体）：
        //   result = normalize(vec3(base.xy + highFreq.xy, base.z))
        // 它把高频法线的 XY 扰动按其自身切空间叠加到基础法线的切空间中，比"直接相加再归一化"更能保住
        // 基础法线的朝向；不引入新采样器 / 纹理，只多一次采样。
        // **带宽取舍**：只对 `i == maxWeightIndex` 采样一次；若每层都加一次，则每片元会多出最高 4 次采样
        // （材质采样已是最大的一笔带宽，见 tech-plan §7.2.2）。代价是层权重交接处高频细节会瞬间切换，
        // 因幅度有限、且处于窄带过渡内，肉眼不可辨。细节法线保持平面 UV（见上方取舍）。
        if (i == maxWeightIndex) {
            const vec3 highFrequencyNormal =
                texture(u_normal, vec3(planarUv * kDetailNormalUvRatio, textureLayer)).rgb * 2.0 - 1.0;
            layerNormal = normalize(vec3(layerNormal.xy + highFrequencyNormal.xy, layerNormal.z));
        }
        const float layerRough   = texture(u_roughness, vec3(planarUv, textureLayer)).r;
        const float layerAo      = texture(u_ao, vec3(planarUv, textureLayer)).r;

        // 宏观变化：独立 UV 尺度（显著小于基础 UV 尺度），采样单层 macro 图的 R 通道。
        // 乘子围绕 1 上下浮动（±macro_strength），同时调制 albedo 与 roughness（ADR 0010 P2）。
        const float macro       = texture(u_macro, vec3(worldPosition.xz * layer.macroAo.x, 0.0)).r;
        const float macroFactor = 1.0 + layer.macroAo.y * (macro * 2.0 - 1.0);

        // 粗糙度：材质表基准 × 贴图的 ±20% 变化，钳到 kMinRoughness（避免 GGX 除零）。
        const float layerSurfaceRoughness =
            clamp(layer.slope.w * mix(kRoughnessTexLow, kRoughnessTexHigh, layerRough) * macroFactor,
                  kMinRoughness, 1.0);
        // 环境项系数：材质表 ao × 贴图 AO（逐像素）。
        const float layerSurfaceAo = clamp(layer.macroAo.z * layerAo, 0.0, 1.0);

        albedo += layerAlbedo * layer.tintUv.rgb * macroFactor * weight;
        tangentNormal += layerNormal * weight;
        roughness += layerSurfaceRoughness * weight;
        ao += layerSurfaceAo * weight;
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

    // albedo（贴图 × tint × 宏观乘子）按 sRGB 观感给出，先转到**线性空间**，
    // 光照在线性空间完成，最后输出**线性 HDR** 颜色（色调映射 + sRGB 编码在 tonemap.frag）。
    const vec3 albedoLinear = pow(max(albedo, vec3(0.0)), vec3(2.2));
    const float safeRoughness = clamp(roughness, kMinRoughness, 1.0);
    const float ambientOcclusion = clamp(ao, 0.0, 1.0);

    // ---- P1 阴影（T21b）：方向光项乘 (1 - 遮蔽量)，天空光不受遮挡 ----
    const vec3  cameraWorld = lighting.cameraPositionWorld.xyz;
    const float viewDepth   = dot(worldPosition - cameraWorld, shadow.cameraForwardEnabled.xyz);
    const float shadowAmount = sampleShadow(v_relativePosition, geometricNormal, viewDepth);

    // ---- PBR（ADR 0010 P2）：GGX + Smith + Schlick，电介质 F0 = 0.04 ----
    const vec3  sunDirection = lighting.sunDirectionIntensity.xyz;  // 由地表指向太阳（单位向量）
    const vec3  viewDirection = normalize(cameraWorld - worldPosition);
    const vec3  halfVector    = normalize(sunDirection + viewDirection);

    const float nDotL = max(dot(mappedNormal, sunDirection), 0.0);
    const float nDotV = max(dot(mappedNormal, viewDirection), 1e-4);
    const float nDotH = max(dot(mappedNormal, halfVector), 0.0);
    const float vDotH = max(dot(viewDirection, halfVector), 0.0);

    const float alpha    = safeRoughness * safeRoughness;
    const float alphaSq  = alpha * alpha;
    const float dInner   = nDotH * nDotH * (alphaSq - 1.0) + 1.0;
    const float D        = alphaSq / (kPi * dInner * dInner);              // GGX 法线分布
    const float kGeometry = (safeRoughness + 1.0) * (safeRoughness + 1.0) / 8.0;
    const float G        = (nDotL / (nDotL * (1.0 - kGeometry) + kGeometry)) *
                           (nDotV / (nDotV * (1.0 - kGeometry) + kGeometry));  // Smith 几何项
    const vec3  F        = vec3(kF0Dielectric) + (1.0 - kF0Dielectric) * pow(1.0 - vDotH, 5.0);  // Schlick

    const vec3 specularBrdf = (D * G) * F / max(4.0 * nDotL * nDotV, 1e-4);
    const vec3 kD           = vec3(1.0) - F;  // 无金属

    // 入射辐照度（方向光）：颜色 × 强度 × (1 - 遮蔽量)；阴影只衰减**直接光**。
    const vec3 sunRadiance = lighting.sunColorLinear.rgb * (lighting.sunDirectionIntensity.w * (1.0 - shadowAmount));
    // 半球天空光：按法线 y 在"地面反弹色 ↔ 天顶色"之间插值，再乘天空强度。
    // **环境项乘 ambientOcclusion**（ADR 0010 P2：AO 只作用于环境项，不作用于直接光）。
    const float skyWeight   = mappedNormal.y * 0.5 + 0.5;
    const vec3  skyColor    = mix(lighting.skyGroundLinear.rgb, lighting.skyZenithIntensity.rgb, skyWeight);
    const vec3  skyRadiance = skyColor * lighting.skyZenithIntensity.w;

    const vec3 directDiffuse  = kD * albedoLinear * sunRadiance * nDotL;
    const vec3 directSpecular = specularBrdf * sunRadiance * nDotL;
    const vec3 ambient        = kD * albedoLinear * skyRadiance * ambientOcclusion;

    const vec3 litColor = directDiffuse + directSpecular + ambient;

    // ---- P1 指数高度雾（T21c）：在光照之后、写 o_color 之前，仍在线性空间 ----
    // 视距 = length(片元世界位置 - 相机世界位置)，单位 = 格。
    // 高度衰减项 = exp(-heightFalloff · max(片元高度 - 相机高度, 0)) ∈ (0, 1]：低于相机处最浓、越高越稀。
    // 雾量 = 1 - exp(-density · 视距 · 高度衰减项)，钳制到 [0, 1]。
    // 雾色默认取天空地平色 → 远景自然融入天空、无硬边；密度 0.0030 使 20 格内遮挡 < 7%、近景不被洗白。
    // fogParams.x 由 uniform 给出（0 = 关闭），关闭时**整体跳过** length / exp 运算（不留无用分支开销）。
    vec3 finalColor = litColor;
    if (lighting.fogParams.x > 0.5) {
        const float viewDistance = length(worldPosition - cameraWorld);
        const float heightAmount = exp(-lighting.fogParams.y * max(worldPosition.y - cameraWorld.y, 0.0));
        const float fogAmount =
            clamp(1.0 - exp(-lighting.fogColorDensity.a * viewDistance * heightAmount), 0.0, 1.0);
        finalColor = mix(litColor, lighting.fogColorDensity.rgb, fogAmount);
    }
    o_color = vec4(finalColor, 1.0);
}
