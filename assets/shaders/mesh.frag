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
//   7. **真实美术贴图（T66 / V0.3 ⓒ）**：材质表 `[textures]` 启用且资源齐备时，四件套换成真实 CC0 贴图
//      （1024²，`R8G8B8A8_UNORM`）。此时 `material.textureMode.x = 1`：
//      **粗糙度 / AO 取贴图绝对值**（层基准值不参与）、层色 tint 由 CPU 侧置 1、**宏观变化改用该层 albedo 放大采样**
//      （`u_macro` 槽位此时绑的就是 albedo 数组，见 `SetSampledTextureArrays` 的调用处）。
//      资源缺失时由 CPU 侧 **WARN 并回落**程序生成贴图（`textureMode.x = 0`），着色器两条路径都在。
//   8. **室内变暗（V0.8 / ADR 0035 决策四）**：逐实例的**围合体代理**（屋顶构件并集的世界包围盒 +
//      屋檐下沿高度）判定"该片元是否在室内" ⇒ 是则把**环境项**（半球天空光 / IBL）乘 `kInteriorSkyVisibility`
//      （0.45，暗 55%）。直接光不受影响（它由级联阴影负责遮挡）。**判据**：同材质在室内的屏上亮度
//      比室外低 ≥ 30%。未启用（地表 / 室外物件 / 非实例化路径）时该分支**整段跳过**，逐位退回旧行为。
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
// 世界坐标还原：顶点着色器传出的 `v_relativePosition` 是**渲染相对**坐标
// （网格局部坐标 + 逐网格偏移 = 世界坐标 − 渲染原点，T41），这里加上 uniform 的**渲染原点**得到世界坐标。
//
// 绑定约定（SDL3_gpu 的 SPIR-V 资源集；每阶段采样器上限 16，见 SDL_gpu.h，故 6 个采样器无需打包）：
//   set 2 = 片元采样纹理：
//            binding 0 = albedo    （4 层数组）
//            binding 1 = normal    （4 层数组）
//            binding 2 = roughness （4 层数组）
//            binding 3 = ao        （4 层数组）
//            binding 4 = macro     （**1 层**数组）
//            binding 5 = shadow    （阴影深度数组，层 i = 级联 i）
//   set 3 = 片元 uniform 块（binding 0 = 材质 / 槽 0；binding 1 = 光照与雾 / 槽 1；binding 2 = 阴影 / 槽 2；
//           binding 3 = **自发光** / 槽 3，逐网格推送：`rgb` = 颜色（线性光）、`a` = 强度；0 = 普通网格）。
//           ⚠ binding 必须**从 0 连续编号**，且个数要与创建 Shader 时声明的 `num_uniform_buffers` 一致
//             （本文件 = 4）；SDL_gpu 每阶段最多 4 个 uniform 槽。
// 材质数值来自 assets/config/materials.toml（经 BuildMaterialUniform 投影）；
// 光照 / 雾 / 阴影数值来自 assets/config/lighting.toml（经 BuildLightingUniform / BuildShadowUniform 投影）。
// **三处都不在此另写一份**。

layout(location = 0) in vec3 v_relativePosition;
layout(location = 1) in vec3 v_normal;
// location 2：材质槽位**覆盖**（ADR 0014）；`flat` 与顶点着色器一致，不做插值。
//   < 0 = 未指定（地表网格：按世界高度与坡度逐像素算权重）
//   >= 0 = 直接用该槽位（可挖体积的内表面：洞里看到的应是"被切开的那种材质"）
layout(location = 2) flat in float v_material;
// W6e：逐网格不透明度 ∈ [0,1]（1 = 不透明）。< 1 时按 Bayer 抖动 discard 做 dither 淡出（见文件末）。
layout(location = 3) in float v_fade;
// V0.8 室内变暗（[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策四 /
// V0.9 [ADR 0036](../../docs/adr/0036-interior-darkening-param-and-building-placement.md) 决策二）：
// 逐**实例**围合体代理（`flat`，与两个顶点着色器同位置）。`v_enclosureB.w = 0` ⇒ 未启用（地表 / 室外物件）。
layout(location = 4) flat in vec4 v_enclosureA;  // xy = 围合体中心 XZ，zw = 半尺寸 XZ（世界坐标）
layout(location = 5) flat in vec4 v_enclosureB;  // x = 屋檐下沿绝对高度，y = **逐建筑变暗覆盖**（< 0 = 用全局），w = 启用位（1/0）
// V0.10：逐网格 tint（rgb = 目标色、a = 强度；摆放模式"不可放置"红色提示；强度 0 = 不变 ⇒ 逐位退回旧行为）。
layout(location = 6) in vec4 v_meshTint;

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

// ---- V0.8/V0.9 室内变暗（ADR 0035 决策四 / ADR 0036 决策一~二）：围合体判据的常量 ----
//
// 室内变暗的**乘子不再写死**：V0.9 起取「逐建筑覆盖，缺省用全局值」——
//   全局值 = `lighting.sunColorLinear.a`（启动参数 `--interior-darkening=<0~1>`，缺省 0.45）；
//   逐建筑覆盖 = `v_enclosureB.y`（`< 0` = 未给出 ⇒ 用全局值；`[0,1]` = 覆盖）。
//   `1.0` = 完全不调暗 ⇒ 环境项乘 1.0 = **恒等** ⇒ 逐位退回"引入室内变暗之前"的行为。
// 直接光不受它影响 —— 直接光是否被遮挡由级联阴影负责（本项只补"天空光不被阴影遮挡"的缺口）。
// kInteriorCeilingEpsilon：屋檐下沿的判定容差（格）—— 屋檐构件的**底面**恰在下沿高度，
//   容差把它（= 室内天花板）算作室内，而其**顶面**（下沿 + 板厚）仍算室外（受光正确）。
const float kInteriorCeilingEpsilon = 0.05;

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
// T67 / ADR 0021：环境贴图三件套（`lighting.fogParams.z = 0` 时**不采样**它们，
// 此时绑定的是渲染器持有的 1×1 占位纹理 —— 见 mesh_renderer.cpp 的 m_environmentPlaceholder）。
//   binding 6 = 漫反射 irradiance（32×16 等距柱状，余弦卷积）
//   binding 7 = 预过滤高光（按粗糙度分 mip 的等距柱状）
//   binding 8 = BRDF LUT（256²，RG：A / B）
layout(set = 2, binding = 6) uniform sampler2D u_irradiance;
layout(set = 2, binding = 7) uniform sampler2D u_prefiltered;
layout(set = 2, binding = 8) uniform sampler2D u_brdfLut;

struct MaterialLayerParams {
    vec4 height;   // x = min, y = max, z = blend, w = 纹理数组层号
    vec4 slope;    // x = min, y = max, z = blend, w = PBR 粗糙度基准
    vec4 tintUv;   // rgb = 层色 tint, a = 每层 UV 尺度
    vec4 macroAo;  // x = 宏观 UV 尺度, y = 宏观调制强度, z = AO 系数, w = 未用（整块唯一填充位）
};

layout(set = 3, binding = 0, std140) uniform MaterialBlock {
    vec4 renderOrigin;  // xyz = 渲染原点（世界坐标），片元用它把相机相对位置还原为世界坐标
    vec4 triplanar;     // C 项：x = 启用(1/0), y = slope_min, z = slope_max, w = sharpness（来自材质表 [triplanar]）
    // T66：x = 真实美术贴图模式(1/0)，其余为填充。
    //   0 = 程序生成的占位贴图：贴图是**相对变化**（粗糙度 ±20% 乘子、AO 为系数、albedo 为单色细节 × tint）
    //   1 = 真实 CC0 贴图：贴图是**绝对值**（粗糙度 / AO 直接取贴图值；层色 tint 由 CPU 侧置 1）
    //   两套语义必须在这里分支，否则真贴图会被再乘一次层基准值 ⇒ 贴图自身的对比被抹平。
    vec4 textureMode;
    MaterialLayerParams layers[kMaterialLayerCount];
} material;

/// 光照与雾 uniform 块：字段排布与 CPU 侧 engine/render/lighting_table.hpp 的 `LightingUniform`
/// **逐字对应**（8 个 vec4 = 128 字节）。所有颜色在此**已是线性光**——它们是常量，由 CPU 侧
/// 一次性 `pow(c, 2.2)` 转换；而材质 albedo 来自纹理、无法预转，故仍在 main() 内逐像素转线性。
layout(set = 3, binding = 1, std140) uniform LightingBlock {
    vec4 sunDirectionIntensity;  // xyz = 由地表指向太阳的单位方向（世界空间）, w = 强度
    vec4 sunColorLinear;         // rgb = 太阳颜色（线性光）, a = **室内变暗的全局默认值**（V0.9 / ADR 0036 决策一）
    vec4 skyZenithIntensity;     // rgb = 天顶色（线性光）, a = 天空光强度
    vec4 skyHorizonLinear;       // rgb = 地平色（线性光）, a = 未用
    vec4 skyGroundLinear;        // rgb = 地面反弹色（线性光）, a = 未用
    vec4 cameraPositionWorld;    // xyz = 相机世界位置, a = 未用（指数高度雾按视距插值需要）
    vec4 fogColorDensity;        // rgb = 雾色（线性光）, a = 密度（1 / 格）
    vec4 fogParams;              // x = 雾启用(1/0), y = 高度衰减（1 / 格）,
                                 // z = **IBL 启用(1/0)**（T67）, w = **预过滤最大 mip 级号**（T67）
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

/// 等距柱状投影的**逆映射**：世界方向 → UV（T67 / ADR 0021）。
/// 与 `sky.frag` / `ibl_irradiance.frag` / `ibl_prefilter.frag` 的映射**逐字一致**（改一处必须同步）。
/// v = 0 在天顶、v = 1 在天底（与 HDRI 文件的常规朝向一致）。
vec2 DirectionToEquirect(vec3 direction) {
    const float kPi = 3.14159265358979323846;
    return vec2(atan(direction.z, direction.x) / (2.0 * kPi) + 0.5,
                acos(clamp(direction.y, -1.0, 1.0)) / kPi);
}

/// 自发光 uniform 块（T27 / 光球）：片元 uniform **槽 3**，由 `MeshRenderer::DrawMeshes` **逐网格**推送
/// （`SDL_gpu.h`：push 数据对后续绘制生效 ⇒ 普通网格推零值、自发光网格推 `SetEmissiveColor` 的颜色）。
/// 颜色是**线性光**且刻意大于 1（HDR 通路），经色调映射后呈"发白发光"。
layout(set = 3, binding = 3, std140) uniform EmissiveBlock {
    vec4 emissive;  // rgb = 自发光颜色（线性光）, a = 强度（0 = 关）
} emissiveParams;

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

/// 2×2 Bayer 矩阵 `[[0, 2], [3, 1]]`（行主序）的单元素。
int bayer2(int x, int y) {
    if (y == 0) {
        return (x == 0) ? 0 : 2;
    }
    return (x == 0) ? 3 : 1;
}

/// 4×4 Bayer 有序抖动阈值 ∈ (0, 1)（屏幕像素坐标 → 16 级阈值）。
/// 递归构造：`B4(y,x) = 4·B2(y/2, x/2) + B2(y%2, x%2)`。
/// 用途：W6e 逐网格不透明度的 **dither 淡出** —— 阈值 ≥ 不透明度即 `discard`。
float bayer4x4(vec2 pixelCoord) {
    const int x = int(mod(pixelCoord.x, 4.0));
    const int y = int(mod(pixelCoord.y, 4.0));
    const int value = 4 * bayer2((x / 2) % 2, (y / 2) % 2) + bayer2(x % 2, y % 2);
    return (float(value) + 0.5) / 16.0;
}

/// **天空可见性** ∈ [0, 1]（V0.8 室内变暗 / [ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策四）：
/// 1 = 室外（环境项全亮）、`kInteriorSkyVisibility` = 判定为室内（环境项按此调暗）。
///
/// 判据（逐片元、**零额外采样**、全在世界坐标下）：
///   ① 片元的 **XZ** 落在围合体（屋顶构件并集的世界包围盒）内；
///   ② 且该面属于"室内面"：
///      - **竖直面**：法线的水平分量**指向围合体中心**（`dot(N.xz, rel) < 0`）⇒ 朝内的墙面；
///        （朝外的墙面 / 屋外任意面 `dot ≥ 0` ⇒ 室外，受光不变）
///      - **水平面**（地板 / 天花板）：高度 ≤ 屋檐下沿 `+ kInteriorCeilingEpsilon` ⇒ 室内。
///
/// 为什么用解析式围合体而不是逐顶点烘焙 AO：本阶段 kit 构件是**共享原型**
/// （同一原型被多座建筑 / 多处摆放复用），逐原型烘焙无法区分墙的内侧与外侧；而离线烘焙
/// （业界参照 UE5 Lightmass / Volumetric Lightmap、Unity Lightmap + Light Probes）需要尚不具备的烘焙管线
/// ⇒ 取最接近的替代（见 `world/object/object_layer.hpp` 的 `ObjectEnclosure` 说明与 ADR 0035 决策四）。
float computeSkyVisibility(vec3 worldPosition, vec3 geometricNormal) {
    if (v_enclosureB.w < 0.5) {
        return 1.0;  // 未启用（地表 / 室外物件 / 非实例化路径）⇒ 整段跳过
    }
    // V0.9 / ADR 0036 决策一~二：变暗乘子 = 「逐建筑覆盖，缺省用全局值」。
    //   `v_enclosureB.y < 0` = 该建筑未给 `interior_darkening` ⇒ 用全局值（`lighting.sunColorLinear.a`）；
    //   ≥ 0 = 逐建筑覆盖。`1.0` ⇒ 下面乘 1.0 = **恒等**（逐位退回"引入室内变暗之前"的行为）。
    const float interiorFactor = (v_enclosureB.y >= 0.0) ? v_enclosureB.y : lighting.sunColorLinear.a;
    const vec2  rel     = worldPosition.xz - v_enclosureA.xy;
    if (abs(rel.x) > v_enclosureA.z || abs(rel.y) > v_enclosureA.w) {
        return 1.0;  // ① 不在围合体水平范围内（含屋顶外表面、墙外侧面）⇒ 室外
    }
    if (abs(geometricNormal.y) >= 0.5) {
        // ② 水平面：不高于屋檐下沿 ⇒ 地板 / 天花板 = 室内；屋顶顶面（下沿 + 板厚）⇒ 室外。
        return (worldPosition.y <= v_enclosureB.x + kInteriorCeilingEpsilon) ? interiorFactor : 1.0;
    }
    // ② 竖直面：法线的水平分量指向中心 ⇒ 朝内的墙面 = 室内。
    return (dot(geometricNormal.xz, rel) < 0.0) ? interiorFactor : 1.0;
}

void main() {
    const vec3  worldPosition   = v_relativePosition + material.renderOrigin.xyz;
    const vec3  geometricNormal = normalize(v_normal);
    const float slope           = clamp(1.0 - geometricNormal.y, 0.0, 1.0);
    // 材质权重：体积网格带**槽位覆盖**时直接用它（ADR 0014），否则按世界高度与坡度逐像素算。
    // 为什么必须有覆盖：洞底位于地下、坡度 0 ⇒ 按高度/坡度会被判成"草"（"地下草地"）；
    // 而现实里挖开岩体看到的就是岩、挖开土层看到的就是土。
    vec4 weights;
    const int materialOverride = int(v_material + 0.5);
    if (materialOverride >= 0 && materialOverride < kMaterialLayerCount) {
        weights                   = vec4(0.0);
        weights[materialOverride] = 1.0;
    } else {
        weights = computeWeights(worldPosition.y, slope);
    }

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

            // ---- T35 降档（项目所有者选定）：**跳过近零轴** ----
            // 三轴权重之和为 1，而陡壁上通常只有一个轴显著（垂直壁 ≈ 1 个轴、45° 壁 ≈ 2 个轴），
            // 其余轴的贡献可忽略。故把低于 epsilon 的轴权重**置零**（该轴不采样），再对剩余轴重新归一化：
            //   最坏（斜壁）采样 3 → 2 次、垂直壁 3 → 1 次；外观差异仅来自被丢弃的近零贡献（< 2%）。
            // 权重取自上一行、与旧实现**同一公式**，因此权重显著时逐像素结果不变。
            const float kTriAxisEpsilon = 0.02;
            vec3        axisBlend       = axisWeight;
            if (axisBlend.x < kTriAxisEpsilon) {
                axisBlend.x = 0.0;
            }
            if (axisBlend.y < kTriAxisEpsilon) {
                axisBlend.y = 0.0;
            }
            if (axisBlend.z < kTriAxisEpsilon) {
                axisBlend.z = 0.0;
            }
            axisBlend /= max(axisBlend.x + axisBlend.y + axisBlend.z, 1e-5);

            const vec2 uvX = worldPosition.zy * layer.tintUv.a + detail * kDetailStrength;
            const vec2 uvZ = worldPosition.xy * layer.tintUv.a + detail * kDetailStrength;
            layerAlbedo = vec3(0.0);
            layerNormal = vec3(0.0);
            if (axisBlend.x > 0.0) {
                layerAlbedo += axisBlend.x * texture(u_albedo, vec3(uvX, textureLayer)).rgb;
                layerNormal += axisBlend.x * (texture(u_normal, vec3(uvX, textureLayer)).rgb * 2.0 - 1.0);
            }
            if (axisBlend.y > 0.0) {
                layerAlbedo += axisBlend.y * texture(u_albedo, vec3(planarUv, textureLayer)).rgb;
                layerNormal += axisBlend.y * (texture(u_normal, vec3(planarUv, textureLayer)).rgb * 2.0 - 1.0);
            }
            if (axisBlend.z > 0.0) {
                layerAlbedo += axisBlend.z * texture(u_albedo, vec3(uvZ, textureLayer)).rgb;
                layerNormal += axisBlend.z * (texture(u_normal, vec3(uvZ, textureLayer)).rgb * 2.0 - 1.0);
            }
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

        // 宏观变化：独立 UV 尺度（显著小于基础 UV 尺度）。
        //   - 程序生成模式：采样单层 macro 噪声图的 R 通道（值域 [0,1]）；
        //   - 真实贴图模式（T66）：真实资源里没有"宏观变化图"，改为**用该层自己的 albedo 按宏观尺度放大采样**
        //     —— 既打破平铺重复（真实 albedo 自带色斑 / 结构），又不额外占用显存与采样器。
        // 乘子围绕 1 上下浮动（±macro_strength），同时调制 albedo 与 roughness（ADR 0010 P2）。
        const bool  realTextures = material.textureMode.x > 0.5;
        const vec2  macroUv      = worldPosition.xz * layer.macroAo.x;
        const float macro = realTextures ? texture(u_albedo, vec3(macroUv, textureLayer)).r
                                        : texture(u_macro, vec3(macroUv, 0.0)).r;
        const float macroFactor = 1.0 + layer.macroAo.y * (macro * 2.0 - 1.0);

        // 粗糙度：程序生成模式 = 材质表基准 × 贴图的 ±20% 变化；真实贴图模式 = **贴图值本身**。
        // 两者都乘宏观乘子并钳到 kMinRoughness（避免 GGX 除零）。
        const float layerSurfaceRoughness =
            clamp((realTextures ? layerRough : layer.slope.w * mix(kRoughnessTexLow, kRoughnessTexHigh, layerRough)) *
                      macroFactor,
                  kMinRoughness, 1.0);
        // 环境项系数：程序生成模式 = 材质表 ao × 贴图 AO（系数）；真实贴图模式 = **贴图 AO 本身**。
        const float layerSurfaceAo = clamp(realTextures ? layerAo : layer.macroAo.z * layerAo, 0.0, 1.0);

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

    // ---- 环境项（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）：**IBL 三件套** ----
    //   `环境漫反射 = albedo · irradiance(N)`（irradiance 已含 1/π 口径，见 ibl_irradiance.frag）
    //   `环境高光   = prefiltered(R, roughness) · (F0·A + B)`（split-sum，A/B 来自 BRDF LUT）
    // **回落路径**（未烘焙 / HDRI 缺失）：退回 ADR 0010 P1 的半球天空光（按法线 y 插值天顶色 ↔ 地面反弹色）。
    // 启用位与预过滤 mip 级数由光照 uniform 的 `fogParams.zw` 给出（CPU 侧投影，见 lighting_table.hpp）。
    // 两条路径都**只乘 ambientOcclusion**（ADR 0010 P2：AO 只作用于环境项，不作用于直接光）。
    const bool  useIbl        = lighting.fogParams.z > 0.5;
    const float prefilterLodMax = max(lighting.fogParams.w, 0.0);

    vec3 ambientDiffuse = vec3(0.0);
    if (useIbl) {
        ambientDiffuse = kD * albedoLinear * texture(u_irradiance, DirectionToEquirect(mappedNormal)).rgb;
    } else {
        const float skyWeight   = mappedNormal.y * 0.5 + 0.5;
        const vec3  skyColor    = mix(lighting.skyGroundLinear.rgb, lighting.skyZenithIntensity.rgb, skyWeight);
        ambientDiffuse          = kD * albedoLinear * skyColor * lighting.skyZenithIntensity.w;
    }
    ambientDiffuse *= ambientOcclusion;

    vec3 ambientSpecular = vec3(0.0);
    if (useIbl) {
        // 粗粗糙度取更高 mip：lod = roughness × (mip 级数 − 1)（烘焙时 mip i ⇒ roughness = i/(级数−1)）。
        const vec3 reflection = reflect(-viewDirection, mappedNormal);
        const vec3 prefiltered = textureLod(u_prefiltered, DirectionToEquirect(reflection),
                                            safeRoughness * prefilterLodMax).rgb;
        const vec2 brdfTerms   = texture(u_brdfLut, vec2(nDotV, safeRoughness)).rg;
        ambientSpecular = prefiltered * (F * brdfTerms.x + brdfTerms.y) * ambientOcclusion;
    }

    const vec3 directDiffuse  = kD * albedoLinear * sunRadiance * nDotL;
    const vec3 directSpecular = specularBrdf * sunRadiance * nDotL;
    // V0.8 室内变暗：环境项（天空光 / IBL）按**天空可见性**调制；直接光不受影响（它由级联阴影负责遮挡）。
    const float skyVisibility = computeSkyVisibility(worldPosition, geometricNormal);
    const vec3 ambient        = (ambientDiffuse + ambientSpecular) * skyVisibility;

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

    // ---- 自发光（T27 / 光球）：在**雾之后**叠加 ----
    // 放在雾之后是刻意的：光球是光源，不该被大气雾按距离洗掉（否则远距离射击时看不见弹丸）。
    // 强度 0（普通地表 / 角色网格）时这一项严格加 0，不影响任何既有观感。
    finalColor += emissiveParams.emissive.rgb * emissiveParams.emissive.a;

    // ---- W6e：逐网格淡出（Bayer 抖动 discard）----
    // 为什么用 dither 而不是 alpha 混合：混合会引入**深度排序**问题（角色在透明队列里与地形互相穿插）；
    // 抖动 discard 保持在**不透明管线**内（照常写深度、无排序），是业界"贴脸淡出主角"的通行做法。
    // 不透明度 ≥ 1 时不进入该分支（普通网格零开销）。
    if (v_fade < 0.999) {
        if (bayer4x4(gl_FragCoord.xy) >= v_fade) {
            discard;
        }
    }

    // V0.10：逐网格 **tint**（摆放模式的"不可放置"红色提示）。强度 0（默认）⇒ `mix` 退化为原色、逐位不变。
    finalColor = mix(finalColor, v_meshTint.rgb, clamp(v_meshTint.a, 0.0, 1.0));

    o_color = vec4(finalColor, 1.0);
}
