#version 450

// 阴影通道的**占位片元着色器**（T21b）：本通道是纯深度写入，没有任何颜色输出。
//
// 为什么需要它（而不是 pipeline 里传 nullptr）：SDL3_gpu 的 `SDL_CreateGPUGraphicsPipeline`
// 会断言片元着色器非空（`'!"Fragment shader cannot be NULL!"'`，见 SDL_gpu.c），
// 因此即使 `num_color_targets = 0`、深度写入完全由光栅化阶段完成，也必须挂一个空入口。
// 这里不做任何计算、不写任何输出，所以该通道的开销可忽略。

void main() {}
