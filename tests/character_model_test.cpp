// T69 主角模型拼装的**纯函数**单测（无需 GPU）：用 T68 的固定夹具
// `tests/fixtures/skinned_triangle.gltf`（3 顶点 / 1 三角面 / 2 关节 / 1 条动画 "Wave"）。
//
// 覆盖：① 顶点 / 索引逐值搬运；② 索引按顶点基数偏移（多网格合并口径）；③ 绑定姿态"脚底中心"；
//       ④ 按名字查 clip（区分大小写）。

#include "character_model.hpp"

#include "render/model_loader.hpp"

#include <filesystem>

#include <gtest/gtest.h>

namespace {

std::filesystem::path FixturePath() {
    return std::filesystem::path(VOXEL_SOURCE_DIR) / "tests" / "fixtures" / "skinned_triangle.gltf";
}

}  // namespace

TEST(CharacterModel, FlattensFixtureMeshAndComputesFeetPivot) {
    const vx::Model                 model = vx::LoadModel(FixturePath());
    const vx::CharacterSkinnedMesh  built = vx::BuildSkinnedMeshFromModel(model);

    // 夹具只有一个网格 ⇒ 合并结果与源一一对应。
    ASSERT_EQ(built.mesh.vertices.size(), 3u);
    ASSERT_EQ(built.mesh.indices.size(), 3u);
    EXPECT_EQ(built.mesh.indices, (std::vector<std::uint32_t> { 0U, 1U, 2U }));

    // 位置 / 关节 / 权重逐值搬运。
    EXPECT_FLOAT_EQ(built.mesh.vertices[0].position[0], 0.0F);
    EXPECT_FLOAT_EQ(built.mesh.vertices[1].position[0], 1.0F);
    EXPECT_FLOAT_EQ(built.mesh.vertices[2].position[1], 1.0F);
    EXPECT_EQ(built.mesh.vertices[0].joints[0], 0U);
    EXPECT_EQ(built.mesh.vertices[2].joints[0], 1U);
    EXPECT_FLOAT_EQ(built.mesh.vertices[0].weights[0], 1.0F);

    // 绑定姿态 AABB：min = (0,0,0)、max = (1,1,0) ⇒ 脚底中心 = (0.5, 0, 0)。
    EXPECT_FLOAT_EQ(built.localPivot.x, 0.5F);
    EXPECT_FLOAT_EQ(built.localPivot.y, 0.0F);
    EXPECT_FLOAT_EQ(built.localPivot.z, 0.0F);
}

TEST(CharacterModel, FindsClipByNameCaseSensitively) {
    const vx::Model model = vx::LoadModel(FixturePath());

    const vx::AnimationClip* wave = vx::FindAnimationClip(model, "Wave");
    ASSERT_NE(wave, nullptr);
    EXPECT_FLOAT_EQ(wave->duration, 1.0F);

    EXPECT_EQ(vx::FindAnimationClip(model, "wave"), nullptr);  // 区分大小写
    EXPECT_EQ(vx::FindAnimationClip(model, "Idle"), nullptr);  // 夹具里没有
}
