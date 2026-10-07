#version 450

// 地表 / 体积网格的顶点着色器（真正的网格路径，非 PoC 冒烟三角形）。
//
// 顶点输入与 engine/render/mesh_renderer.hpp 的 MeshVertex 布局一一对应：
//   location 0 `vec3 position` —— **网格局部坐标**（T41）：地表 tile 内 0..64 格、可挖体积块内 0..32 格。
//                                世界定位用整数 / double（红线 6），一个网格到底是"哪一块"由下面的
//                                `meshOffset`（网格世界原点 − 渲染原点）在顶点阶段补上，
//                                故这里的顶点数值恒为小量，float 的有效位不会被大坐标吃掉。
//   location 1 `vec3 normal`   —— 世界空间单位法线（由高度场梯度算出，禁止面法线近似）。
//   location 2 `float material` —— **材质槽位覆盖**（ADR 0014）：
//                                 < 0 = 未指定（片元按世界高度 / 坡度逐像素算权重，地表走这条）；
//                                 >= 0 = 直接用该槽位（可挖体积的内表面走这条）。
//
// **不再**传递材质权重：ADR 0009 起权重由片元着色器逐像素按世界高度与坡度重算。
// 片元需要世界坐标才能算权重，故这里只传出**渲染原点相对**位置；片元用 uniform 的渲染原点把它还原为世界坐标。
//
// 相机常量：SDL3_gpu 的 SPIR-V 约定 —— 顶点着色器 set 0 放只读资源（采样器 / 存储缓冲），
// set 1 放 uniform。渲染侧用 SDL_BindGPUVertexStorageBuffers(pass, 0, ...) 绑定该缓冲，
// 故这里声明为 set = 0、binding = 0 的只读 storage buffer，布局与 CameraUniform 一致
// （std430 下 mat4 即 4 个 vec4，无隐式填充；见 ADR 0002 的双格式管线）。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inMaterial;
// W7-S3b：morph 目标高度（该顶点在**父级 LOD 网格**对应列上的采样高度，单位 = 格）。仅当地表网格的
// meshParams.y（morphStep）> 0 时被下方 morph 段消费；非地表网格缺省 0（不参与 morph）。
layout(location = 3) in float inMorph;

layout(set = 0, binding = 0, std430) readonly buffer CameraBuffer {
    mat4 viewProjection;
    // W7-S3b：LOD 原点（渲染相对坐标，xyz；w 预留；std430 偏移 = 64，紧跟 mat4）。morph 因子按顶点到
    // 该原点的 Chebyshev 距离计算；缺省 (0,0,0) ⇒ 与从前逐位一致。
    vec4 lodOrigin;
} camera;

// 逐网格**模型变换**（T41 起；T33 由 `vec4` 偏移泛化为 `mat4`）：
// `modelToRender` = 平移(网格世界原点 − 渲染原点) × 旋转，把**网格局部坐标**变成**渲染原点相对坐标**。
// 由渲染侧 `SDL_PushGPUVertexUniformData(cmd, 0, ...)` **逐网格**推送（顶点 set 1 = uniform，
// 与片元 set 3 同一套 SDL3_gpu 约定，个数须与创建 Shader 时声明的 `num_uniform_buffers` 一致）。
// 两个好处：① 渲染原点重定基只需改这一块，**不再重传整世界的 137 个网格**（T41）；
// ② 倒塌中的刚体（位置与姿态都在变）也只需改这一块，CPU **不必**重烘焙上万顶点（T33）。
layout(set = 1, binding = 0, std140) uniform MeshTransformBlock {
    mat4 modelToRender;  // 局部坐标 → 渲染原点相对坐标（std140：mat4 = 4 个 vec4）
    // W6e/W7-S3b：x = 逐网格不透明度（1 = 不透明；< 1 = 片元 Bayer 抖动淡出）；
    // y = morphStep（> 0 启用 CDLOD 顶点过渡；= 父级网格步长）；z / w = morph 起 / 止距离（格）。
    vec4 meshParams;
} meshTransform;

layout(location = 0) out vec3 v_relativePosition;
layout(location = 1) out vec3 v_normal;
// `flat`：材质槽位是**整面离散属性**，不做插值（插值会让三角形内出现"槽位 1.7"这种无意义的中间值）。
layout(location = 2) flat out float v_material;
// W6e：逐网格不透明度（插值无害；同一网格所有顶点同值）。
layout(location = 3) out float v_fade;
// V0.8 室内变暗：**非实例化路径没有围合体代理** ⇒ 恒写"未启用"（片元据此整段跳过室内变暗）。
// 位置必须与 `mesh_instanced.vert` 一致（两者共用 `mesh.frag`）。
layout(location = 4) flat out vec4 v_enclosureA;
layout(location = 5) flat out vec4 v_enclosureB;

void main() {
    // 网格局部坐标 → 渲染原点相对坐标（片元据此 + 渲染原点还原世界坐标）。
    vec3 relativePosition = (meshTransform.modelToRender * vec4(inPosition, 1.0)).xyz;
    gl_Position           = camera.viewProjection * vec4(relativePosition, 1.0);

    // W7-S3b：CDLOD 顶点过渡（ADR 0024 接缝策略）——把本环顶点向**父级（更粗一级）网格**的对应列靠拢，
    // 使相邻环在环边界处几何逐位一致，消除裂缝。`meshParams.y`（morphStep）= 父级网格步长，0 = 关闭（跳过）。
    const float morphStep = meshTransform.meshParams.y;
    if (morphStep > 0.0 && meshTransform.meshParams.w > meshTransform.meshParams.z) {
        // 顶点到 LOD 原点的 **Chebyshev 距离**（与 CPU 侧 tile 距离口径同源，ADR 0024「tile 对齐」）。
        const float lodDistance = max(abs(relativePosition.x - camera.lodOrigin.x),
                                      abs(relativePosition.z - camera.lodOrigin.z));
        // morph 因子：起点前 0（保留本环细节）、终点后 1（完全贴合父级网格）——
        // 目标是让 k 恰在环边界取 1（ADR 0024「边界处相邻环几何逐位相同」）。
        const float k = clamp((lodDistance - meshTransform.meshParams.z) /
                                  (meshTransform.meshParams.w - meshTransform.meshParams.z), 0.0, 1.0);
        // 父级网格的对应列（局部坐标向下对齐到 morphStep 的整数倍），高度取顶点自带的 morph 目标高度。
        const vec3 targetLocal = vec3(floor(inPosition.x / morphStep) * morphStep, inMorph,
                                      floor(inPosition.z / morphStep) * morphStep);
        const vec3 morphedLocal = mix(inPosition, targetLocal, k);
        relativePosition = (meshTransform.modelToRender * vec4(morphedLocal, 1.0)).xyz;
        gl_Position      = camera.viewProjection * vec4(relativePosition, 1.0);
    }

    // 注意：`v_relativePosition` 必须在 morph **之后**赋值（片元用它还原世界坐标算材质权重）。
    v_relativePosition = relativePosition;
    // 法线必须跟着**同一个旋转**走（T33：倒塌中的刚体会翻滚）——矩阵无缩放，故 mat3 仍是纯旋转、法线保持单位长。
    v_normal           = mat3(meshTransform.modelToRender) * inNormal;
    v_material         = inMaterial;
    // W6e：逐网格不透明度（CPU 侧恒显式写入：不透明网格为 1.0，淡出中的主角 < 1.0）。
    v_fade             = meshTransform.meshParams.x;
    // V0.8：地表 / 非实例化物件**没有**围合体代理 ⇒ 启用位 0 ⇒ 片元不做室内变暗（逐位退回旧行为）。
    v_enclosureA       = vec4(0.0);
    v_enclosureB       = vec4(0.0);
}
