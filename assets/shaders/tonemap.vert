#version 450

// 全屏三角形顶点着色器（T20 / ADR 0010 P0 的色调映射通道）。
//
// **顶点缓冲为空**：位置由顶点索引在着色器内直接生成，单个三角形覆盖整个渲染目标，
// 因此本通道不需要绑定顶点 / 索引资源，也不需要深度目标。
//
// 绑定约定（与 mesh.frag 一致，见 ADR 0002 的双格式管线）：
//   片元侧 set = 2、binding = 0：采样纹理（主通道产出的 HDR 颜色目标）；
//   片元侧 set = 3、binding = 0：片元 uniform 块（曝光等参数）。
// 顶点着色器本身不声明任何资源集。

layout(location = 0) out vec2 v_uv;

void main() {
    // 顶点 0/1/2 → (-1,-1) / (3,-1) / (-1,3)：一个三角形覆盖 [-1,1]² 的裁剪空间。
    const vec2 kPositions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    const vec2 position = kPositions[gl_VertexIndex];

    // **V 必须翻转**：SDL_gpu 的 NDC 是"左下角 (-1,-1)、+Y 向上"，而纹理坐标是"左上角 (0,0)、+Y 向下"
    //（SDL_gpu.h §Coordinate System；后端差异由 SDL 自动转换，禁止在 shader 里做别的翻转）。
    // 若照直写 `position * 0.5 + 0.5`，屏幕上方会取到图像的**底部** ⇒ 整帧上下翻转
    //（叠加层在 tonemap 之后直接写交换链，因此会出现"UI 正常、世界倒置"的怪象）。
    v_uv        = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
    gl_Position = vec4(position, 0.0, 1.0);
}
