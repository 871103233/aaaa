// T68 模型导入单测：glTF / .glb 的加载与骨骼动画采样。
//
// 夹具 = tests/fixtures/skinned_triangle.gltf（3 顶点 / 1 三角面 / 2 关节 / 1 条旋转动画，
// 由 tests/fixtures/generate_skinned_triangle.ps1 生成，**进仓库**）。
// 覆盖判据（见 docs/plans/v0.3.md §1.2）：① 计数确定；② 加载与采样**逐值可复现**（红线 7）；
// ③ 已知时刻（t=1，90° 绕 Z）的具体矩阵值；④ 失败即抛、不静默回退。

#include "render/model_loader.hpp"

#include <glm/glm.hpp>

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::filesystem::path FixturePath() {
    return std::filesystem::path(VOXEL_SOURCE_DIR) / "tests" / "fixtures" / "skinned_triangle.gltf";
}

/// 逐位比较两组矩阵（确定性判据：同输入应得到逐位相同的输出）。
bool MatricesBitEqual(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(glm::mat4)) == 0;
}

} // namespace

TEST(ModelLoader, SkinnedFixtureCounts) {
    const vx::Model model = vx::LoadModel(FixturePath());

    ASSERT_TRUE(model.hasSkin);
    ASSERT_EQ(model.meshes.size(), 1u);
    EXPECT_EQ(model.meshes[0].vertices.size(), 3u);
    EXPECT_EQ(model.meshes[0].indices.size(), 3u);
    ASSERT_EQ(model.joints.size(), 2u);
    ASSERT_EQ(model.animations.size(), 1u);
    EXPECT_EQ(model.animations[0].name, "Wave");
    EXPECT_FLOAT_EQ(model.animations[0].duration, 1.0f);

    // 关节表按层级排序：parent < 自身索引。
    EXPECT_EQ(model.joints[0].name, "BoneA");
    EXPECT_EQ(model.joints[0].parent, -1);
    EXPECT_EQ(model.joints[1].name, "BoneB");
    EXPECT_EQ(model.joints[1].parent, 0);

    // 顶点绑定：前两点绑到 BoneA，第三点绑到 BoneB（权重归一化为 1）。
    EXPECT_EQ(model.meshes[0].vertices[0].joints[0], 0u);
    EXPECT_FLOAT_EQ(model.meshes[0].vertices[0].weights[0], 1.0f);
    EXPECT_EQ(model.meshes[0].vertices[2].joints[0], 1u);
    EXPECT_FLOAT_EQ(model.meshes[0].vertices[2].weights[0], 1.0f);

    // 位置逐值确定。
    EXPECT_EQ(model.meshes[0].vertices[0].position, glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(model.meshes[0].vertices[1].position, glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(model.meshes[0].vertices[2].position, glm::vec3(0.5f, 1.0f, 0.0f));
}

TEST(ModelLoader, LoadIsDeterministic) {
    const vx::Model first = vx::LoadModel(FixturePath());
    const vx::Model second = vx::LoadModel(FixturePath());

    ASSERT_EQ(first.meshes.size(), second.meshes.size());
    for (std::size_t m = 0; m < first.meshes.size(); ++m) {
        ASSERT_EQ(first.meshes[m].vertices.size(), second.meshes[m].vertices.size());
        EXPECT_EQ(std::memcmp(first.meshes[m].vertices.data(),
                              second.meshes[m].vertices.data(),
                              first.meshes[m].vertices.size() * sizeof(vx::ModelVertex)),
                  0);
        EXPECT_EQ(first.meshes[m].indices, second.meshes[m].indices);
    }
    ASSERT_EQ(first.joints.size(), second.joints.size());
    for (std::size_t j = 0; j < first.joints.size(); ++j) {
        EXPECT_EQ(first.joints[j].name, second.joints[j].name);
        EXPECT_EQ(first.joints[j].parent, second.joints[j].parent);
        EXPECT_EQ(first.joints[j].localBind, second.joints[j].localBind);
        EXPECT_EQ(first.joints[j].inverseBind, second.joints[j].inverseBind);
    }
}

TEST(ModelLoader, BindPoseSkinningIsIdentity) {
    const vx::Model model = vx::LoadModel(FixturePath());
    const auto skin = vx::ComputeSkinningMatrices(model, model.animations[0], 0.0f);

    ASSERT_EQ(skin.size(), 2u);
    // 绑定姿态（t=0）下 Global(j) × inverseBind(j) = 单位矩阵。
    for (const glm::mat4& matrix : skin) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(matrix[col][row], (col == row) ? 1.0f : 0.0f, 1e-5f);
            }
        }
    }
}

TEST(ModelLoader, AnimationSampleIsReproducible) {
    const vx::Model model = vx::LoadModel(FixturePath());
    const auto a = vx::ComputeSkinningMatrices(model, model.animations[0], 0.5f);
    const auto b = vx::ComputeSkinningMatrices(model, model.animations[0], 0.5f);

    // 同一时刻两次采样必须逐位相同（红线 7）。
    EXPECT_TRUE(MatricesBitEqual(a, b));
}

TEST(ModelLoader, AnimationKnownValueAtEnd) {
    const vx::Model model = vx::LoadModel(FixturePath());
    // t = duration(=1) 时 BoneA 绕 Z 旋转 90°：蒙皮矩阵的列 0 = (0,1,0)、列 1 = (-1,0,0)。
    const auto skin = vx::ComputeSkinningMatrices(model, model.animations[0], 1.0f);

    ASSERT_EQ(skin.size(), 2u);
    EXPECT_NEAR(skin[0][0][0], 0.0f, 1e-5f);
    EXPECT_NEAR(skin[0][0][1], 1.0f, 1e-5f);
    EXPECT_NEAR(skin[0][1][0], -1.0f, 1e-5f);
    EXPECT_NEAR(skin[0][1][1], 0.0f, 1e-5f);
}

TEST(ModelLoader, MissingFileThrows) {
    EXPECT_THROW((void)vx::LoadModel(FixturePath().parent_path() / "does_not_exist.gltf"), std::runtime_error);
}
