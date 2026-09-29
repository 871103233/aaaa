#version 450

// IBL 烘焙之三：**BRDF LUT**（T67 / [ADR 0021](../../docs/adr/0021-environment-ibl.md)）。
//
// 输出 = 256×256、RG16F；`x = A`、`y = B`，供运行期做 split-sum 的第二项：
//     `环境高光 = prefiltered(R, roughness) · (F0 · A + B)`
//
// 用 **Karis 的解析拟合**（"Real Shading in UE4"）而不是数值积分：① 无采样 ⇒ 烘焙瞬间完成、**逐位确定**（红线 7）；
// ② 该项只依赖 `(N·V, roughness)` 两个量、且是低频函数，解析拟合的误差在目视范围内可忽略（**已登记为取舍**）。
// 切换条件：目视发现"掠射角下的高光边沿不对" ⇒ 换成数值积分版本（采样序固定即可保持确定性）。
//
// 顶点阶段复用 `tonemap.vert`。

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    const float nDotV     = max(v_uv.x, 1e-3);   // 行 = N·V ∈ (0,1]
    const float roughness = max(v_uv.y, 1e-3);   // 列 = roughness

    // Karis 的 4 项拟合：r = (roughness·c0 + c1)，再按 N·V 做一次指数混合。
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r  = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;

    // 钳到非负：拟合在极端输入下可能给出极小负值，负的 A/B 会让高光变负（颜色失真）。
    o_color = vec4(max(ab, vec2(0.0)), 0.0, 1.0);
}
