#include "render/model_loader.hpp"

// Assimp 头**只允许在本 .cpp 内出现**（公共头不含 Assimp 类型，见 model_loader.hpp 顶部说明）。
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

// 注：gtx/matrix_decompose 属 GLM 实验性扩展，需在包含前开启开关（仅为拆分绑定姿态的 TRS）。
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "core/log.hpp"

namespace vx {
namespace {

/// 导入后处理：只做三角化 —— 其余后处理会改变顶点数量与顺序，破坏"同一文件 ⇒ 逐值相同"的可复现性。
constexpr unsigned kImportFlags = aiProcess_Triangulate;

glm::vec2 ToVec2(const aiVector3D& v) {
    return {v.x, v.y};
}

glm::vec3 ToVec3(const aiVector3D& v) {
    return {v.x, v.y, v.z};
}

/// Assimp `aiMatrix4x4` 是**行主序**、`glm::mat4` 是**列主序** ⇒ 逐元素按 `glm[col][row] = ai[row][col]` 搬运。
glm::mat4 ToMat4(const aiMatrix4x4& m) {
    glm::mat4 r(1.0f);
    r[0][0] = m.a1;
    r[1][0] = m.a2;
    r[2][0] = m.a3;
    r[3][0] = m.a4;
    r[0][1] = m.b1;
    r[1][1] = m.b2;
    r[2][1] = m.b3;
    r[3][1] = m.b4;
    r[0][2] = m.c1;
    r[1][2] = m.c2;
    r[2][2] = m.c3;
    r[3][2] = m.c4;
    r[0][3] = m.d1;
    r[1][3] = m.d2;
    r[2][3] = m.d3;
    r[3][3] = m.d4;
    return r;
}

/// 由绑定姿态矩阵取回 TRS（用于"某分量无动画轨道时回落绑定姿态"）。
void DecomposeBind(const glm::mat4& bind, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    glm::vec3 skew(0.0f);
    glm::vec4 perspective(0.0f);
    glm::decompose(bind, scale, rotation, translation, skew, perspective);
}

/// 采样一条 vec3 轨道（线性插值）。空轨道 ⇒ `fallback`。
glm::vec3
SampleVec3(const std::vector<float>& times, const std::vector<glm::vec3>& values, float t, const glm::vec3& fallback) {
    if (times.empty() || values.size() != times.size()) {
        return fallback;
    }
    if (t <= times.front()) {
        return values.front();
    }
    if (t >= times.back()) {
        return values.back();
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        if (t < times[i]) {
            const float span = times[i] - times[i - 1];
            const float u = span > 0.0f ? (t - times[i - 1]) / span : 0.0f;
            return glm::mix(values[i - 1], values[i], u);
        }
    }
    return values.back();
}

/// 采样一条四元数轨道（slerp，结果重新归一化）。
glm::quat
SampleQuat(const std::vector<float>& times, const std::vector<glm::quat>& values, float t, const glm::quat& fallback) {
    if (times.empty() || values.size() != times.size()) {
        return fallback;
    }
    if (t <= times.front()) {
        return glm::normalize(values.front());
    }
    if (t >= times.back()) {
        return glm::normalize(values.back());
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        if (t < times[i]) {
            const float span = times[i] - times[i - 1];
            const float u = span > 0.0f ? (t - times[i - 1]) / span : 0.0f;
            return glm::normalize(glm::slerp(values[i - 1], values[i], u));
        }
    }
    return glm::normalize(values.back());
}

} // namespace

Model LoadModel(const std::filesystem::path& path) {
    Assimp::Importer importer;
    const std::string pathUtf8 = path.u8string();
    const aiScene* scene = importer.ReadFile(pathUtf8, kImportFlags);
    if (scene == nullptr) {
        throw std::runtime_error("模型加载失败：" + pathUtf8 + " —— " + importer.GetErrorString());
    }
    if (scene->mRootNode == nullptr) {
        throw std::runtime_error("模型加载失败：" + pathUtf8 + " —— 场景无根节点");
    }

    // ---- 1. 收集骨骼名与其逆绑定矩阵（跨 mesh 去重） ----
    std::unordered_map<std::string, glm::mat4> boneInverseBind;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh == nullptr) {
            continue;
        }
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            if (bone == nullptr) {
                continue;
            }
            const std::string name = bone->mName.C_Str();
            // 同一骨骼跨 mesh 重复；逆绑定矩阵应一致，取首个即可。
            boneInverseBind.emplace(name, ToMat4(bone->mOffsetMatrix));
        }
    }

    Model model;
    model.hasSkin = !boneInverseBind.empty();

    // ---- 2. 按层级 DFS 生成关节表：保证 parent < 自身索引 ----
    std::vector<SkeletonJoint> joints;
    std::unordered_map<std::string, int> boneIndex;
    if (model.hasSkin) {
        std::function<void(const aiNode*, int)> dfs = [&](const aiNode* node, int parentBone) {
            int thisBone = parentBone;
            const std::string name = node->mName.C_Str();
            const auto it = boneInverseBind.find(name);
            if (it != boneInverseBind.end()) {
                const int idx = static_cast<int>(joints.size());
                SkeletonJoint joint;
                joint.name = name;
                joint.parent = parentBone;
                joint.localBind = ToMat4(node->mTransformation);
                joint.inverseBind = it->second;
                joints.push_back(joint);
                boneIndex.emplace(name, idx);
                thisBone = idx;
            }
            for (unsigned i = 0; i < node->mNumChildren; ++i) {
                dfs(node->mChildren[i], thisBone);
            }
        };
        dfs(scene->mRootNode, -1);

        // 骨骼表里有、但不在节点层级中的（异常文件）：补成根关节，不静默丢弃。
        std::vector<std::string> orphan;
        for (const auto& [name, ibm] : boneInverseBind) {
            if (boneIndex.find(name) == boneIndex.end()) {
                orphan.push_back(name);
            }
        }
        std::sort(orphan.begin(), orphan.end()); // 确定性
        for (const std::string& name : orphan) {
            SkeletonJoint joint;
            joint.name = name;
            joint.parent = -1;
            joint.localBind = glm::mat4(1.0f);
            joint.inverseBind = boneInverseBind.at(name);
            boneIndex.emplace(name, static_cast<int>(joints.size()));
            joints.push_back(joint);
        }
        if (!orphan.empty()) {
            VX_LOG_WARN("模型导入：%zu 个骨骼不在节点层级中（已补为根关节）：%s", orphan.size(), pathUtf8.c_str());
        }
    }
    model.joints = std::move(joints);

    // ---- 3. 网格 ----
    model.meshes.reserve(scene->mNumMeshes);
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh == nullptr) {
            continue;
        }
        ModelMesh out;
        out.name = mesh->mName.C_Str();

        // 逐顶点收集骨骼影响（顶点数 = 装配后的顶点数，与面索引同源）。
        std::vector<std::vector<std::pair<int, float>>> influences(mesh->mNumVertices);
        if (model.hasSkin) {
            for (unsigned b = 0; b < mesh->mNumBones; ++b) {
                const aiBone* bone = mesh->mBones[b];
                if (bone == nullptr) {
                    continue;
                }
                const auto it = boneIndex.find(bone->mName.C_Str());
                if (it == boneIndex.end()) {
                    continue;
                }
                for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                    const aiVertexWeight& weight = bone->mWeights[w];
                    if (weight.mVertexId < influences.size()) {
                        influences[weight.mVertexId].emplace_back(it->second, weight.mWeight);
                    }
                }
            }
        }

        out.vertices.reserve(mesh->mNumVertices);
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            ModelVertex vertex;
            vertex.position = ToVec3(mesh->mVertices[v]);
            if (mesh->HasNormals()) {
                vertex.normal = ToVec3(mesh->mNormals[v]);
            }
            if (mesh->HasTextureCoords(0)) {
                vertex.uv = ToVec2(mesh->mTextureCoords[0][v]);
            }

            auto& inf = influences[v];
            if (!inf.empty()) {
                // 权重降序；同权重按关节索引升序 —— 保证"同一文件 ⇒ 逐值相同"。
                std::sort(inf.begin(), inf.end(), [](const auto& a, const auto& b) {
                    if (a.second != b.second) {
                        return a.second > b.second;
                    }
                    return a.first < b.first;
                });
                const std::size_t count = std::min<std::size_t>(4, inf.size());
                float sum = 0.0f;
                for (std::size_t k = 0; k < count; ++k) {
                    const int slot = static_cast<int>(k); // glm::vec4::operator[] 取 int
                    vertex.joints[slot] = static_cast<std::uint32_t>(inf[k].first);
                    vertex.weights[slot] = inf[k].second;
                    sum += inf[k].second;
                }
                if (sum > 0.0f) {
                    for (std::size_t k = 0; k < count; ++k) {
                        vertex.weights[static_cast<int>(k)] /= sum;
                    }
                } else {
                    vertex.weights[0] = 1.0f;
                }
            }
            out.vertices.push_back(vertex);
        }

        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned k = 0; k < face.mNumIndices; ++k) {
                out.indices.push_back(face.mIndices[k]);
            }
        }
        model.meshes.push_back(std::move(out));
    }

    // ---- 4. 动画 ----
    model.animations.reserve(scene->mNumAnimations);
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        if (anim == nullptr) {
            continue;
        }
        AnimationClip clip;
        clip.name = anim->mName.C_Str();
        // 时间统一换算成**秒**：源以 tick 为单位，除以 ticksPerSecond（缺省 1 ⇒ 视为秒）。
        const double ticksPerSecond = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 1.0;

        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* channel = anim->mChannels[c];
            if (channel == nullptr) {
                continue;
            }
            const auto it = boneIndex.find(channel->mNodeName.C_Str());
            if (it == boneIndex.end()) {
                continue; // 只保留针对关节的通道
            }
            AnimationChannel out;
            out.joint = it->second;

            out.translation.times.reserve(channel->mNumPositionKeys);
            out.translation.values.reserve(channel->mNumPositionKeys);
            for (unsigned k = 0; k < channel->mNumPositionKeys; ++k) {
                const aiVectorKey& key = channel->mPositionKeys[k];
                out.translation.times.push_back(static_cast<float>(key.mTime / ticksPerSecond));
                out.translation.values.push_back(ToVec3(key.mValue));
            }

            out.rotation.times.reserve(channel->mNumRotationKeys);
            out.rotation.values.reserve(channel->mNumRotationKeys);
            for (unsigned k = 0; k < channel->mNumRotationKeys; ++k) {
                const aiQuatKey& key = channel->mRotationKeys[k];
                out.rotation.times.push_back(static_cast<float>(key.mTime / ticksPerSecond));
                // Assimp 的 aiQuaternion 分量序为 (w, x, y, z)，与 glm::quat 构造一致。
                out.rotation.values.emplace_back(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z);
            }

            out.scale.times.reserve(channel->mNumScalingKeys);
            out.scale.values.reserve(channel->mNumScalingKeys);
            for (unsigned k = 0; k < channel->mNumScalingKeys; ++k) {
                const aiVectorKey& key = channel->mScalingKeys[k];
                out.scale.times.push_back(static_cast<float>(key.mTime / ticksPerSecond));
                out.scale.values.push_back(ToVec3(key.mValue));
            }

            clip.channels.push_back(std::move(out));
        }

        for (const AnimationChannel& channel : clip.channels) {
            if (!channel.translation.times.empty()) {
                clip.duration = std::max(clip.duration, channel.translation.times.back());
            }
            if (!channel.rotation.times.empty()) {
                clip.duration = std::max(clip.duration, channel.rotation.times.back());
            }
            if (!channel.scale.times.empty()) {
                clip.duration = std::max(clip.duration, channel.scale.times.back());
            }
        }
        model.animations.push_back(std::move(clip));
    }

    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    for (const ModelMesh& mesh : model.meshes) {
        vertexCount += mesh.vertices.size();
        indexCount += mesh.indices.size();
    }
    VX_LOG_INFO("模型已加载：%s（网格 %zu / 顶点 %zu / 索引 %zu / 关节 %zu / 动画 %zu%s）",
                pathUtf8.c_str(),
                model.meshes.size(),
                vertexCount,
                indexCount,
                model.joints.size(),
                model.animations.size(),
                model.hasSkin ? "，蒙皮" : "");
    return model;
}

std::vector<glm::mat4> SampleJointLocalTransforms(const Model& model, const AnimationClip& clip, float timeSeconds) {
    const float t = std::max(0.0f, std::min(timeSeconds, clip.duration));

    // 每关节先取绑定姿态的 TRS，再用动画通道覆盖。
    std::vector<glm::vec3> translation(model.joints.size());
    std::vector<glm::quat> rotation(model.joints.size());
    std::vector<glm::vec3> scale(model.joints.size());
    for (std::size_t j = 0; j < model.joints.size(); ++j) {
        DecomposeBind(model.joints[j].localBind, translation[j], rotation[j], scale[j]);
    }

    for (const AnimationChannel& channel : clip.channels) {
        if (channel.joint < 0 || static_cast<std::size_t>(channel.joint) >= model.joints.size()) {
            continue;
        }
        const std::size_t j = static_cast<std::size_t>(channel.joint);
        if (!channel.translation.times.empty()) {
            translation[j] = SampleVec3(channel.translation.times, channel.translation.values, t, translation[j]);
        }
        if (!channel.rotation.times.empty()) {
            rotation[j] = SampleQuat(channel.rotation.times, channel.rotation.values, t, rotation[j]);
        }
        if (!channel.scale.times.empty()) {
            scale[j] = SampleVec3(channel.scale.times, channel.scale.values, t, scale[j]);
        }
    }

    std::vector<glm::mat4> local(model.joints.size(), glm::mat4(1.0f));
    for (std::size_t j = 0; j < model.joints.size(); ++j) {
        local[j] = glm::translate(glm::mat4(1.0f), translation[j]) * glm::mat4_cast(rotation[j]) *
                   glm::scale(glm::mat4(1.0f), scale[j]);
    }
    return local;
}

std::vector<glm::mat4> ComputeSkinningMatrices(const Model& model, const AnimationClip& clip, float timeSeconds) {
    const std::vector<glm::mat4> local = SampleJointLocalTransforms(model, clip, timeSeconds);

    // 关节表保证 parent < 自身索引 ⇒ 一次正向遍历即可合成全局变换。
    std::vector<glm::mat4> global(model.joints.size(), glm::mat4(1.0f));
    for (std::size_t j = 0; j < model.joints.size(); ++j) {
        const int parent = model.joints[j].parent;
        global[j] = (parent >= 0 ? global[static_cast<std::size_t>(parent)] : glm::mat4(1.0f)) * local[j];
    }

    std::vector<glm::mat4> skinning(model.joints.size(), glm::mat4(1.0f));
    for (std::size_t j = 0; j < model.joints.size(); ++j) {
        skinning[j] = global[j] * model.joints[j].inverseBind;
    }
    return skinning;
}

} // namespace vx
