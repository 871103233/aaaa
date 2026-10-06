#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace vx {

/// 物件**底面支撑探测点**的局部 XZ 偏移（相对底面中心；已按 `yawDegrees` 绕 Y 旋转）。
///
/// 见 [`plans/v0.5.md`](../../docs/plans/v0.5.md) §1.5：物件失去支撑时必须**落下**（不得悬空）。
struct ObjectProbeOffset {
    float x = 0.0F;
    float z = 0.0F;
};

/// 探测点数量（底面中心 + 四角）。
inline constexpr std::size_t kObjectSupportProbeCount = 5;

/// 生成支撑探测点：底面中心 + 四个角（`±halfX, ±halfZ`），整体绕 Y 旋转 `yawDegrees`。
///
/// 为什么用"中心 + 四角"：物件底面压在**不规则地面**上时，只要有**任一**探测点仍是实心就算有支撑
/// （保守口径：不把"架在棱上 / 半悬空"误判成失去支撑）；只有**全部**落空才判为失去支撑。
///
/// 旋转方向与 `RotateMeshAboutY`（`object_mesh.hpp`）及渲染的四元数**同一约定**（绕 `+Y` 右手系）。
[[nodiscard]] inline std::array<ObjectProbeOffset, kObjectSupportProbeCount> ObjectSupportProbes(
    float halfX, float halfZ, float yawDegrees) noexcept {
    constexpr float kProbeDegreesToRadians = 3.14159265358979323846F / 180.0F;
    const float     radians   = yawDegrees * kProbeDegreesToRadians;
    const float     c         = std::cos(radians);
    const float     s         = std::sin(radians);

    const float corners[4][2] = { { -halfX, -halfZ }, { halfX, -halfZ }, { halfX, halfZ }, { -halfX, halfZ } };

    std::array<ObjectProbeOffset, kObjectSupportProbeCount> probes {};
    probes[0] = ObjectProbeOffset { 0.0F, 0.0F };
    for (std::size_t i = 0; i < 4; ++i) {
        const float localX = corners[i][0];
        const float localZ = corners[i][1];
        probes[i + 1]     = ObjectProbeOffset { c * localX + s * localZ, -s * localX + c * localZ };
    }
    return probes;
}

/// 由探测结果判定**是否仍有支撑**：**任一**探测点实心 ⇒ 有支撑；全空 ⇒ 失去支撑。
/// 前置条件：`solidFlags` 至少 `count` 个元素。
[[nodiscard]] inline bool ObjectHasSupport(const bool* solidFlags, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (solidFlags[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace vx
