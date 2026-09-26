#pragma once

#include "render/mesh_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

/// 程序化胶囊网格参数（**局部空间**：脚底为原点，+Y 向上，与 Jolt `CharacterVirtual` 的胶囊原点一致）。
struct CapsuleMeshSpec {
    float radius             = 0.3F;  ///< 胶囊半径（格）
    float cylinderHalfHeight = 0.6F;  ///< 圆柱段半高（格）；总高 = `2 * (半高 + 半径)`
    int   radialSegments     = 20;    ///< 绕 Y 轴的径向分段（下限 3）
    int   capRings           = 6;     ///< 每个半球沿经线的分段（下限 1）
};

/// `CapsuleMeshSpec` 派生的几何尺寸：半球球心高度与总高（格）。
struct CapsuleGeometry {
    float bottomCenterY = 0.0F;  ///< 下半球球心高度（= 半径）
    float topCenterY    = 0.0F;  ///< 上半球球心高度（= 半径 + 2 * 圆柱半高）
    float totalHeight   = 0.0F;  ///< 总高（= 2 * (圆柱半高 + 半径)）
};

/// 由参数推出几何尺寸（纯函数，供网格构建与测试复用）。
[[nodiscard]] inline CapsuleGeometry CapsuleGeometryFor(const CapsuleMeshSpec& spec) noexcept {
    CapsuleGeometry geometry;
    geometry.bottomCenterY = spec.radius;
    geometry.topCenterY    = spec.radius + 2.0F * spec.cylinderHalfHeight;
    geometry.totalHeight   = 2.0F * (spec.cylinderHalfHeight + spec.radius);
    return geometry;
}

/// 构建主角**可视**胶囊网格（`MeshVertex` 格式，与地表网格共用渲染路径）。
///
/// 尺寸与 Jolt 碰撞胶囊完全一致，但本网格**只用于渲染**，不参与任何物理。
/// 顶点位置为**局部空间**坐标（脚底为原点）：调用方需按相机相对偏移约定
/// `world = 角色脚底 + 局部顶点 - 渲染原点` 平移到世界位置后再上传（红线 6）。
///
/// 法线朝外且为单位向量；三角形绕序在**从外部观察**时为逆时针
/// （与 `MeshRenderer` 的 `FRONTFACE_COUNTER_CLOCKWISE` + 背面剔除一致）。
/// 顶点只含位置与法线：ADR 0009 起材质权重由片元着色器按世界高度与坡度逐像素重算，
/// 故主角与地表共用同一条网格管线、同一套材质参数。
[[nodiscard]] inline MeshData BuildCapsuleMesh(const CapsuleMeshSpec& spec = CapsuleMeshSpec {}) {
    const int   radial = std::max(spec.radialSegments, 3);
    const int   rings  = std::max(spec.capRings, 1);
    const float radius = spec.radius;

    const CapsuleGeometry geometry = CapsuleGeometryFor(spec);

    constexpr float kPi    = 3.14159265358979323846F;
    constexpr float kTwoPi = 2.0F * kPi;

    /// 一圈顶点的数量：θ = 2π 处**复制**一份，保证四边形闭合而不必对索引取模。
    const std::size_t ringVertexCount = static_cast<std::size_t>(radial) + 1;

    /// 环的总数：上半球（含极点与赤道）+ 一根圆柱底环 + 下半球（不含赤道）。
    const std::size_t ringCount = static_cast<std::size_t>(2 * rings + 2);

    MeshData mesh;
    mesh.vertices.reserve(ringCount * ringVertexCount);

    /// 追加一圈**球面**顶点：`centerY` 为球心高度，`phi` 为相对 +Y 的极角（0 = 球顶）。
    const auto appendSphereRing = [&](float centerY, float phi) {
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int j = 0; j <= radial; ++j) {
            const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(radial);
            const float nx    = sinPhi * std::cos(theta);
            const float ny    = cosPhi;
            const float nz    = sinPhi * std::sin(theta);

            MeshVertex vertex;
            vertex.position[0] = radius * nx;
            vertex.position[1] = centerY + radius * ny;
            vertex.position[2] = radius * nz;
            vertex.normal[0]   = nx;
            vertex.normal[1]   = ny;
            vertex.normal[2]   = nz;
            mesh.vertices.push_back(vertex);
        }
    };

    /// 追加一圈**圆柱侧壁**顶点：法线水平，`y` 为环高。
    const auto appendCylinderRing = [&](float y) {
        for (int j = 0; j <= radial; ++j) {
            const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(radial);
            const float nx    = std::cos(theta);
            const float nz    = std::sin(theta);

            MeshVertex vertex;
            vertex.position[0] = radius * nx;
            vertex.position[1] = y;
            vertex.position[2] = radius * nz;
            vertex.normal[0]   = nx;
            vertex.normal[1]   = 0.0F;
            vertex.normal[2]   = nz;
            mesh.vertices.push_back(vertex);
        }
    };

    // 上半球：phi 从 0（极点）到 π/2（赤道），高度由高到低。
    for (int r = 0; r <= rings; ++r) {
        const float phi = (kPi * 0.5F) * static_cast<float>(r) / static_cast<float>(rings);
        appendSphereRing(geometry.topCenterY, phi);
    }
    // 圆柱段底环（与下半球赤道重合，此处只放一圈，下半球从下一纬度开始）。
    appendCylinderRing(geometry.bottomCenterY);
    // 下半球：phi 从 π/2 的下一个纬度开始，到 π（极点）。
    for (int r = 1; r <= rings; ++r) {
        const float phi = (kPi * 0.5F) * (1.0F + static_cast<float>(r) / static_cast<float>(rings));
        appendSphereRing(geometry.bottomCenterY, phi);
    }

    // 每两个相邻环之间铺一圈四边形；绕序使三角形从外部看为逆时针（正面）。
    const std::size_t quadCount = (ringCount - 1) * static_cast<std::size_t>(radial);
    mesh.indices.reserve(quadCount * 6);
    for (std::size_t r = 0; r + 1 < ringCount; ++r) {
        const std::size_t row0 = r * ringVertexCount;
        const std::size_t row1 = (r + 1) * ringVertexCount;
        for (int j = 0; j < radial; ++j) {
            const std::uint32_t p00 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j));
            const std::uint32_t p01 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j) + 1);
            const std::uint32_t p10 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j));
            const std::uint32_t p11 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j) + 1);

            mesh.indices.push_back(p00);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p10);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p11);
            mesh.indices.push_back(p10);
        }
    }

    return mesh;
}

}  // namespace vx
