#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vx {

/// 模型导入（T68）：glTF / `.glb` 的静态网格与**蒙皮网格** + 骨骼动画**采样**。
///
/// 分层（SKILL §2 / 方案 §9.1）：本模块位于 **engine 层**，只提供**通用**的模型数据结构与采样纯函数，
/// **不含任何地形 / 玩法专有类型**；Assimp 的类型**全部**隐藏在 `model_loader.cpp` 内，
/// 公共头**不出现 Assimp 类型**（与 `physics_world` 隔离 Jolt 的口径一致）。
///
/// 线程约定：`LoadModel` 是**纯 CPU 路径**（不触碰 GPU、不读全局状态），可从加载期 / 工作线程调用；
/// 采样函数无状态、确定性（同输入 ⇒ 逐值相同，红线 7）。**GPU 上传仍必须留在渲染线程**。
///
/// **T68 只做"加载 + 采样"，不做渲染接线**（蒙皮着色器与主角替换属 T69）。

/// 网格顶点：位置 / 法线 / 主 UV + **4 路骨骼影响**（关节索引指向 `Model` 的 `joints`）。
///
/// 权重已按"取权重最大的 4 路并重新归一化"处理 ⇒ 着色器可直接加权求和。
/// 静态（非蒙皮）网格的 `joints` / `weights` 全 0；缺法线或 UV 时对应分量为 0。
struct ModelVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
};

/// 一个网格（源文件里的一个 primitives / aiMesh）；`indices` 为三角形列表。
struct ModelMesh {
    std::string name;
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

/// 骨骼关节。约定：**`joints` 按层级顺序排列 ⇒ `parent < 自身索引` 恒成立**（根为 `-1`），
/// 因此合成全局变换可一次正向遍历完成，且与遍历顺序无关（确定性）。
struct SkeletonJoint {
    std::string name;
    int parent = -1;             ///< 父关节索引；`-1` = 根
    glm::mat4 localBind{1.0f};   ///< 绑定姿态的**局部**变换
    glm::mat4 inverseBind{1.0f}; ///< 逆绑定矩阵（世界 → 关节局部）
};

/// 平移 / 缩放关键帧轨道（时间单位 = **秒**，升序）。
struct TranslationTrack {
    std::vector<float> times;
    std::vector<glm::vec3> values;
};

/// 旋转关键帧轨道（时间单位 = **秒**，升序）。
struct RotationTrack {
    std::vector<float> times;
    std::vector<glm::quat> values;
};

/// 缩放关键帧轨道（时间单位 = **秒**，升序）。
struct ScaleTrack {
    std::vector<float> times;
    std::vector<glm::vec3> values;
};

/// 针对某个关节的一条动画通道；三类轨道为空表示"该分量不参与动画"（回落绑定姿态）。
struct AnimationChannel {
    int joint = -1;
    TranslationTrack translation;
    RotationTrack rotation;
    ScaleTrack scale;
};

/// 一段动画（源文件里的一个 `aiAnimation`）。
struct AnimationClip {
    std::string name;
    float duration = 0.0f; ///< 秒（各通道最大时间）
    std::vector<AnimationChannel> channels;
};

/// 一个模型：网格 + 关节 + 动画。静态模型 `joints` 为空、`hasSkin == false`。
struct Model {
    bool hasSkin = false;
    std::vector<ModelMesh> meshes;
    std::vector<SkeletonJoint> joints;
    std::vector<AnimationClip> animations;
};

/// 从 glTF / `.glb` 载入模型。
///
/// 语义：**纯函数** —— 同一份文件字节 ⇒ 逐值相同的输出（红线 7）。
/// 失败一律抛 `std::runtime_error` 并说明原因（打不开 / 解析失败 / 无场景），**不静默回退**
/// （口径同 `texture_loader` / ADR 0005）。
[[nodiscard]] Model LoadModel(const std::filesystem::path& path);

/// 采样 `clip` 在 `timeSeconds` 时刻各关节的**局部**变换（时间会被钳到 `[0, duration]`；
/// 缺轨道的分量回落该关节的**绑定姿态**）。返回长度 = `model.joints.size()`。
[[nodiscard]] std::vector<glm::mat4>
SampleJointLocalTransforms(const Model& model, const AnimationClip& clip, float timeSeconds);

/// 采样并合成**蒙皮矩阵**：`skinning[j] = Global(j) × inverseBind[j]`。
/// 顶点着色器用 `Σ wᵢ · skinning[jointsᵢ] · position` 计算蒙皮后位置。
[[nodiscard]] std::vector<glm::mat4>
ComputeSkinningMatrices(const Model& model, const AnimationClip& clip, float timeSeconds);

} // namespace vx
