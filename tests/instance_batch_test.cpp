// V0.7 H1：实例化**纯函数**的单测（实例打包 / 容量钳制）。
// V0.8：追加**围合体代理**字段的打包与缺省（室内变暗 / ADR 0035 决策四）。
//
// 为什么只测纯函数：GPU 管线 / 实例缓冲的真正验证在 `--auto-test` 的冒烟与 F1 面板（draw call 由 N→1），
// 单元测试无法创建 GPU 设备。这里锁定的是"位姿 → 渲染相对矩阵"的语义、"超容量截断"的边界
// （`RenderFrame` 的 WARN 分支即依赖后者的返回值），以及围合体字段在实例缓冲里的**偏移与启用位**。

#include "render/instance_batch.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using vx::BuildInstanceModelToRender;
using vx::InstancePose;
using vx::PackInstanceTransforms;

/// 单个实例占的 `float` 数（= `kInstancePoseBytes / sizeof(float)`）：一次性口径，用例内复用。
constexpr std::size_t kFloatsPerInstance = vx::kInstancePoseBytes / sizeof(float);

/// 平移：矩阵第 4 列 = `原点 − 渲染原点`，线性部分 = 单位。
TEST(InstanceBatch, BuildModelTranslatesRelativeToRenderOrigin) {
    InstancePose pose;
    pose.origin = glm::dvec3(10.0, 20.0, -30.0);
    const glm::mat4 model = BuildInstanceModelToRender(pose, glm::dvec3(4.0, 20.0, 5.0));

    EXPECT_FLOAT_EQ(model[3][0], 6.0F);
    EXPECT_FLOAT_EQ(model[3][1], 0.0F);
    EXPECT_FLOAT_EQ(model[3][2], -35.0F);
    EXPECT_FLOAT_EQ(model[3][3], 1.0F);
    // 无旋转 ⇒ 线性部分是单位阵。
    EXPECT_FLOAT_EQ(model[0][0], 1.0F);
    EXPECT_FLOAT_EQ(model[1][1], 1.0F);
    EXPECT_FLOAT_EQ(model[2][2], 1.0F);
}

/// 旋转只作用在**线性部分**，不改变平移（法线直接乘 `mat3(model)` 即成立的前提）。
TEST(InstanceBatch, BuildModelRotationLeavesTranslationUntouched) {
    InstancePose pose;
    pose.origin   = glm::dvec3(1.0, 2.0, 3.0);
    // 绕 Y 轴 90°：局部 +X 轴应映射到世界 -Z 方向。
    pose.rotation = glm::angleAxis(glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 model = BuildInstanceModelToRender(pose, glm::dvec3(0.0));

    const glm::vec3 xAxis = glm::vec3(model * glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
    EXPECT_NEAR(xAxis.x, 0.0F, 1.0e-5F);
    EXPECT_NEAR(xAxis.y, 0.0F, 1.0e-5F);
    EXPECT_NEAR(xAxis.z, -1.0F, 1.0e-5F);
    // 平移不受旋转影响。
    EXPECT_FLOAT_EQ(model[3][0], 1.0F);
    EXPECT_FLOAT_EQ(model[3][1], 2.0F);
    EXPECT_FLOAT_EQ(model[3][2], 3.0F);
}

/// 精度（红线 6）：大坐标下平移仍在 **`double`** 相减后才落回 `float`。
/// `1e8 + 0.5 − 1e8` 在 `float` 下会因间距（该量级为 8）而丢成 0；在 `double` 下保持 0.5。
TEST(InstanceBatch, BuildModelSubtractsInDoublePrecision) {
    InstancePose pose;
    pose.origin = glm::dvec3(1.0e8 + 0.5, 0.0, 0.0);
    const glm::mat4 model = BuildInstanceModelToRender(pose, glm::dvec3(1.0e8, 0.0, 0.0));

    EXPECT_FLOAT_EQ(model[3][0], 0.5F);
}

/// V0.8：实例记录步长 = `mat4`(64) + 围合体 `vec4`×2(32) = **96 字节**（与两条实例化着色器的 std430 结构体一致）。
TEST(InstanceBatch, InstanceRecordStrideIs96Bytes) {
    EXPECT_EQ(vx::kInstancePoseBytes, 96U);
    EXPECT_EQ(kFloatsPerInstance, 24U);
}

/// 打包：`count < capacity` ⇒ 写入 `count` 个，且每个与 `BuildInstanceModelToRender` **逐位一致**。
TEST(InstanceBatch, PackWritesAllInstancesWhenWithinCapacity) {
    std::vector<InstancePose> poses(3);
    poses[0].origin = glm::dvec3(1.0, 0.0, 0.0);
    poses[1].origin = glm::dvec3(2.0, 0.0, 0.0);
    poses[2].origin = glm::dvec3(3.0, 0.0, 0.0);

    std::vector<float> out(4U * kFloatsPerInstance, -1.0F);  // 容量 4，后面留一格哨兵
    const std::uint32_t written =
        PackInstanceTransforms(poses.data(), 3U, 4U, glm::dvec3(0.0), out.data());

    EXPECT_EQ(written, 3U);
    for (std::uint32_t index = 0; index < 3U; ++index) {
        const glm::mat4 expected = BuildInstanceModelToRender(poses[index], glm::dvec3(0.0));
        for (int element = 0; element < 16; ++element) {
            EXPECT_FLOAT_EQ(out[index * kFloatsPerInstance + static_cast<std::size_t>(element)],
                            (&expected[0][0])[element]);
        }
    }
    // 未触及的尾部保持哨兵值（没有越界写）：第 4 个实例（索引 3）从未被写入。
    EXPECT_FLOAT_EQ(out[3U * kFloatsPerInstance], -1.0F);
}

/// 打包：`count == capacity` ⇒ 恰好写满（边界不截断）。
TEST(InstanceBatch, PackAcceptsExactlyCapacity) {
    std::vector<InstancePose> poses(2);
    std::vector<float> out(2U * kFloatsPerInstance, 0.0F);
    EXPECT_EQ(PackInstanceTransforms(poses.data(), 2U, 2U, glm::dvec3(0.0), out.data()), 2U);
}

/// 打包：`count > capacity` ⇒ **截断到容量**（返回实际写入数，调用方据此 WARN；SKILL「超容量不静默」）。
TEST(InstanceBatch, PackTruncatesBeyondCapacity) {
    std::vector<InstancePose> poses(5);
    std::vector<float> out(2U * kFloatsPerInstance, 0.0F);
    const std::uint32_t written =
        PackInstanceTransforms(poses.data(), 5U, 2U, glm::dvec3(0.0), out.data());

    EXPECT_EQ(written, 2U);  // 只写 2 个，绝不越界写第 3 个
}

/// 打包：空指针 / 零容量 ⇒ 返回 0（不写任何字节）。
TEST(InstanceBatch, PackRejectsEmptyInputs) {
    InstancePose pose;
    std::vector<float> out(kFloatsPerInstance, 0.0F);
    EXPECT_EQ(PackInstanceTransforms(nullptr, 1U, 1U, glm::dvec3(0.0), out.data()), 0U);
    EXPECT_EQ(PackInstanceTransforms(&pose, 1U, 1U, glm::dvec3(0.0), nullptr), 0U);
    EXPECT_EQ(PackInstanceTransforms(&pose, 1U, 0U, glm::dvec3(0.0), out.data()), 0U);
}

// --------------------------- V0.8：围合体代理（室内变暗）---------------------------

/// 围合体字段紧随 `mat4`（偏移 16..20），启用位在偏移 23；其余分量原样透传（无隐式默认）。
TEST(InstanceBatch, PackWritesEnclosureFields) {
    std::vector<InstancePose> poses(1);
    poses[0].enclosureEnabled  = true;
    poses[0].enclosureCenterX  = 1.5F;
    poses[0].enclosureCenterZ  = -2.5F;
    poses[0].enclosureHalfX    = 2.0F;
    poses[0].enclosureHalfZ    = 3.0F;
    poses[0].enclosureCeilingY = 12.25F;

    std::vector<float> out(kFloatsPerInstance, 0.0F);
    ASSERT_EQ(PackInstanceTransforms(poses.data(), 1U, 1U, glm::dvec3(0.0), out.data()), 1U);

    EXPECT_FLOAT_EQ(out[16], 1.5F);   // enclosureA.x = 中心 X
    EXPECT_FLOAT_EQ(out[17], -2.5F);  // enclosureA.y = 中心 Z
    EXPECT_FLOAT_EQ(out[18], 2.0F);   // enclosureA.z = 半尺寸 X
    EXPECT_FLOAT_EQ(out[19], 3.0F);   // enclosureA.w = 半尺寸 Z
    EXPECT_FLOAT_EQ(out[20], 12.25F); // enclosureB.x = 屋檐下沿高度
    EXPECT_FLOAT_EQ(out[23], 1.0F);   // enclosureB.w = 启用位
}

/// 缺省（室外物件 / 地表）⇒ 启用位为 0（片元据此整段跳过室内变暗，逐位退回旧行为）。
TEST(InstanceBatch, PackWritesDisabledEnclosureByDefault) {
    InstancePose pose;
    std::vector<float> out(kFloatsPerInstance, 1.0F);  // 全填哨兵，验证打包确实覆盖了启用位
    ASSERT_EQ(PackInstanceTransforms(&pose, 1U, 1U, glm::dvec3(0.0), out.data()), 1U);
    EXPECT_FLOAT_EQ(out[23], 0.0F);
}

/// V0.9：逐建筑变暗覆盖写在 `enclosureB.y`（偏移 21）；缺省 = `-1`（= 片元用全局值）。
TEST(InstanceBatch, PackWritesEnclosureDarkening) {
    std::vector<InstancePose> poses(1);
    poses[0].enclosureEnabled   = true;
    poses[0].enclosureDarkening = 0.20F;
    std::vector<float> out(kFloatsPerInstance, 0.0F);
    ASSERT_EQ(PackInstanceTransforms(poses.data(), 1U, 1U, glm::dvec3(0.0), out.data()), 1U);
    EXPECT_FLOAT_EQ(out[21], 0.20F);

    InstancePose fallback;  // 未给出 ⇒ -1（用全局值）
    std::vector<float> outDefault(kFloatsPerInstance, 0.0F);
    ASSERT_EQ(PackInstanceTransforms(&fallback, 1U, 1U, glm::dvec3(0.0), outDefault.data()), 1U);
    EXPECT_FLOAT_EQ(outDefault[21], -1.0F);
}

}  // namespace
