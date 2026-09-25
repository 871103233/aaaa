#version 450

// 顶点位置写死在 Shader 内：本冒烟测试刻意不引入顶点缓冲，
// 以便把「设备 + 管线 + 交换链 + Shader 字节码加载」这条链路单独隔离出来验证。

const vec2 kPositions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

const vec3 kColors[3] = vec3[](
    vec3(0.90, 0.30, 0.25),
    vec3(0.30, 0.80, 0.40),
    vec3(0.25, 0.50, 0.95)
);

layout(location = 0) out vec3 v_color;

void main() {
    gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
    v_color     = kColors[gl_VertexIndex];
}
