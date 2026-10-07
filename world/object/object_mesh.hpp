#pragma once

#include "object/object_layer.hpp"
#include "render/mesh_renderer.hpp"
#include "render/model_loader.hpp"  // V8：`BuildObjectMeshFromModel` 消费 `Model`

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace vx {

/// 物件**程序化代形**的分段参数（V0b；美术 kit 未就位前的占位几何）。
struct ObjectMeshSpec {
    int radialSegments = 20;  ///< 绕 Y 轴的径向分段（下限 3）
    int rings          = 8;   ///< 竖向分段：椭球沿经线 / 锥沿母线（下限 2）
};

/// 由形态取**材质槽位覆盖**（[ADR 0014](../../docs/adr/0014-voxel-material-index.md) 的逐顶点
/// `MeshVertex::material`）：物件走地表同一套 4 槽素材质数组（草 0 / 土 1 / 岩 2 / 沙 3）。
///
/// 为什么必须给覆盖：不覆盖时片元按**世界高度与坡度**算权重（那是**地表**规则）——
/// 摆在平地上的物件坡度 ≈ 0、高度落在草带 ⇒ 石块 / 木箱会被染成草绿。给出槽位后按该槽位取单一材质。
/// **占位口径**：木箱无对应地表材质，暂用**土**（最近似的褐色）；正式材质随 kit 资产一并引入。
[[nodiscard]] constexpr float ObjectMaterialSlot(ObjectAssetKind kind) noexcept {
    switch (kind) {
        case ObjectAssetKind::DirtPile:
            return 1.0F;  // 土
        case ObjectAssetKind::Stone:
            return 2.0F;  // 岩
        case ObjectAssetKind::Crate:
            return 1.0F;  // 土（占位：无木质地表材质）
        case ObjectAssetKind::Portal:
            return 2.0F;  // 岩（占位：门环暂用石材；正式外观随 kit 资产引入）
        case ObjectAssetKind::Model:
            // V8：**本函数不用于 `Model`** —— 模型外观由配置 `ObjectType::materialSlot` 给出
            //（形态 → 槽位的映射对"外部模型"没有意义：树与岩石同属 `Model`，需要不同槽位）。
            // 这里返回草槽只是让枚举穷尽、并作为"未指定 material_slot"时的约定默认值。
            return 0.0F;  // 草
        case ObjectAssetKind::Kit:
            // V0.8：建筑构件 = **代理体**（ADR 0035 决策二）⇒ 用**土**槽占位（最近似的褐色，与木箱同一口径）；
            // 正式外观 = kit 美术资产或室内外专用材质槽（登记为后续，见 ADR 0035「何时重新审视」）。
            return 1.0F;  // 土
    }
    return 1.0F;
}

namespace object_mesh_detail {

constexpr float kPi    = 3.14159265358979323846F;
constexpr float kTwoPi = 2.0F * kPi;

[[nodiscard]] inline MeshVertex MakeVertex(float px, float py, float pz, float nx, float ny, float nz,
                                           float material) noexcept {
    MeshVertex vertex;
    vertex.position[0] = px;
    vertex.position[1] = py;
    vertex.position[2] = pz;
    vertex.normal[0]   = nx;
    vertex.normal[1]   = ny;
    vertex.normal[2]   = nz;
    vertex.material    = material;
    return vertex;
}

/// 轴对齐盒：中心 `(0, halfY, 0)`、底面在 `y = 0`。每个面 4 个独占顶点（面法线）。
inline void AppendBox(MeshData& mesh, float halfX, float halfY, float halfZ, float material) {
    const float x0 = -halfX;
    const float x1 = halfX;
    const float z0 = -halfZ;
    const float z1 = halfZ;
    const float y0 = 0.0F;
    const float y1 = 2.0F * halfY;

    // 面顶点顺序按"从外部看逆时针"排定（与渲染器的 `FRONTFACE_COUNTER_CLOCKWISE` + 背面剔除一致）。
    struct Face {
        float n[3];
        float v[4][3];
    };
    const Face faces[6] = {
        { { 1.0F, 0.0F, 0.0F }, { { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } } },   // +X
        { { -1.0F, 0.0F, 0.0F }, { { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 } } },  // -X
        { { 0.0F, 0.0F, 1.0F }, { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } } },   // +Z
        { { 0.0F, 0.0F, -1.0F }, { { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 } } },  // -Z
        { { 0.0F, 1.0F, 0.0F }, { { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 } } },   // +Y
        { { 0.0F, -1.0F, 0.0F }, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } } },  // -Y
    };

    for (const Face& face : faces) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto& p : face.v) {
            mesh.vertices.push_back(MakeVertex(p[0], p[1], p[2], face.n[0], face.n[1], face.n[2], material));
        }
        mesh.indices.push_back(base + 0U);
        mesh.indices.push_back(base + 1U);
        mesh.indices.push_back(base + 2U);
        mesh.indices.push_back(base + 0U);
        mesh.indices.push_back(base + 2U);
        mesh.indices.push_back(base + 3U);
    }
}

/// **任意位置**的轴对齐盒：中心 `(centerX, centerY, centerZ)`、半尺寸 `(halfX, halfY, halfZ)`。
///
/// 与 `AppendBox` 的唯一差别是**没有"底面贴 `y = 0`"的约束** —— 建筑构件需要把板 / 墙 / 门楣
/// 放在构件局部的任意高度（V0.8；`AppendBox` 的"底面贴地"口径对构件不适用）。
/// 每个面 4 个独占顶点（面法线），绕组与 `AppendBox` 一致（从外部看逆时针）。
inline void AppendBoxCentered(MeshData& mesh, float centerX, float centerY, float centerZ, float halfX, float halfY,
                              float halfZ, float material) {
    const float x0 = centerX - halfX;
    const float x1 = centerX + halfX;
    const float y0 = centerY - halfY;
    const float y1 = centerY + halfY;
    const float z0 = centerZ - halfZ;
    const float z1 = centerZ + halfZ;

    struct Face {
        float n[3];
        float v[4][3];
    };
    const Face faces[6] = {
        { { 1.0F, 0.0F, 0.0F }, { { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } } },   // +X
        { { -1.0F, 0.0F, 0.0F }, { { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 } } },  // -X
        { { 0.0F, 0.0F, 1.0F }, { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } } },   // +Z
        { { 0.0F, 0.0F, -1.0F }, { { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 } } },  // -Z
        { { 0.0F, 1.0F, 0.0F }, { { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 } } },   // +Y
        { { 0.0F, -1.0F, 0.0F }, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } } },  // -Y
    };

    for (const Face& face : faces) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto& p : face.v) {
            mesh.vertices.push_back(MakeVertex(p[0], p[1], p[2], face.n[0], face.n[1], face.n[2], material));
        }
        mesh.indices.push_back(base + 0U);
        mesh.indices.push_back(base + 1U);
        mesh.indices.push_back(base + 2U);
        mesh.indices.push_back(base + 0U);
        mesh.indices.push_back(base + 2U);
        mesh.indices.push_back(base + 3U);
    }
}


/// 椭球：中心 `(0, halfY, 0)`、半轴 `(halfX, halfY, halfZ)`、底面落在 `y = 0`。
///
/// 法线取椭球隐函数 `((x/hx)² + ((y−hy)/hy)² + (z/hz)²)` 的梯度方向（单位球方向 `m` ⇒
/// `normalize(m.x/hx, m.y/hy, m.z/hz)`），非"用球面方向近似"。
inline void AppendEllipsoid(MeshData& mesh, float halfX, float halfY, float halfZ, int radial, int rings,
                            float material) {
    const float invX = 1.0F / halfX;
    const float invY = 1.0F / halfY;
    const float invZ = 1.0F / halfZ;

    const auto appendRing = [&](float phi) {
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int j = 0; j <= radial; ++j) {
            const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(radial);
            const float mx    = sinPhi * std::cos(theta);
            const float my    = cosPhi;
            const float mz    = sinPhi * std::sin(theta);

            float nx = mx * invX;
            float ny = my * invY;
            float nz = mz * invZ;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 0.0F) {
                nx /= length;
                ny /= length;
                nz /= length;
            }
            mesh.vertices.push_back(MakeVertex(halfX * mx, halfY + halfY * my, halfZ * mz, nx, ny, nz, material));
        }
    };

    for (int r = 0; r <= rings; ++r) {
        appendRing(kPi * static_cast<float>(r) / static_cast<float>(rings));
    }

    const std::size_t ringVertexCount = static_cast<std::size_t>(radial) + 1U;
    for (int r = 0; r + 1 <= rings; ++r) {
        const std::size_t row0 = static_cast<std::size_t>(r) * ringVertexCount;
        const std::size_t row1 = static_cast<std::size_t>(r + 1) * ringVertexCount;
        // 两极的环会塌缩到一点（`sin(phi) = 0` ⇒ 整圈顶点的位置重合）⇒ 该侧四边形的一半是**退化三角形**
        // （零面积、法线为零）。跳过它们（`ADR 0007` 的"退化三角形 = 0"口径同样适用于本模块的产物）：
        // 顶极点行只保留 `(p01, p11, p10)`，底极点行只保留 `(p00, p01, p10)`。
        const bool topPole    = (r == 0);
        const bool bottomPole = (r == rings - 1);
        for (int j = 0; j < radial; ++j) {
            const std::uint32_t p00 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j));
            const std::uint32_t p01 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j) + 1U);
            const std::uint32_t p10 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j));
            const std::uint32_t p11 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j) + 1U);
            if (!topPole) {
                mesh.indices.push_back(p00);
                mesh.indices.push_back(p01);
                mesh.indices.push_back(p10);
            }
            if (!bottomPole) {
                mesh.indices.push_back(p01);
                mesh.indices.push_back(p11);
                mesh.indices.push_back(p10);
            }
        }
    }
}

/// 圆锥（土堆）：底面椭圆半径 `(baseX, baseZ)` 在 `y = 0`，顶点在 `y = 2 * halfY`，另加底面圆盘封闭。
///
/// 侧面法线用解析式（对椭圆锥同样成立）：`normalize(baseZ*H*cosθ, baseX*baseZ, baseX*H*sinθ)`，`H = 2*halfY`。
inline void AppendCone(MeshData& mesh, float baseX, float baseZ, float halfY, int radial, int rings, float material) {
    const float apexY = 2.0F * halfY;

    const auto ringNormal = [&](float theta) {
        const float cosT = std::cos(theta);
        const float sinT = std::sin(theta);
        float nx = baseZ * apexY * cosT;
        float ny = baseX * baseZ;
        float nz = baseX * apexY * sinT;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 0.0F) {
            nx /= length;
            ny /= length;
            nz /= length;
        }
        return std::array<float, 3> { nx, ny, nz };
    };

    // 侧面：rings 圈（自底向上），再补一个顶点。
    const int sideRows = std::max(rings, 2);
    const auto appendSideRing = [&](float s) {
        for (int j = 0; j <= radial; ++j) {
            const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(radial);
            const auto  n     = ringNormal(theta);
            const float radiusScale = 1.0F - s;
            mesh.vertices.push_back(MakeVertex(baseX * radiusScale * std::cos(theta), apexY * s,
                                               baseZ * radiusScale * std::sin(theta), n[0], n[1], n[2], material));
        }
    };
    for (int r = 0; r < sideRows; ++r) {
        appendSideRing(static_cast<float>(r) / static_cast<float>(sideRows));
    }
    const std::uint32_t apex = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(MakeVertex(0.0F, apexY, 0.0F, 0.0F, 1.0F, 0.0F, material));

    const std::size_t ringVertexCount = static_cast<std::size_t>(radial) + 1U;
    for (int r = 0; r + 1 < sideRows; ++r) {
        const std::size_t row0 = static_cast<std::size_t>(r) * ringVertexCount;
        const std::size_t row1 = static_cast<std::size_t>(r + 1) * ringVertexCount;
        for (int j = 0; j < radial; ++j) {
            const std::uint32_t p00 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j));
            const std::uint32_t p01 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(j) + 1U);
            const std::uint32_t p10 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j));
            const std::uint32_t p11 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(j) + 1U);
            // 锥的环序是**自底向上**（与椭球的"自上向下"相反）⇒ 绕序取反，法线才朝外。
            mesh.indices.push_back(p00);
            mesh.indices.push_back(p10);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p10);
            mesh.indices.push_back(p11);
        }
    }
    // 最上一圈三角形扇 → 顶点。
    const std::size_t lastRow = static_cast<std::size_t>(sideRows - 1) * ringVertexCount;
    for (int j = 0; j < radial; ++j) {
        const std::uint32_t p0 = static_cast<std::uint32_t>(lastRow + static_cast<std::size_t>(j));
        const std::uint32_t p1 = static_cast<std::uint32_t>(lastRow + static_cast<std::size_t>(j) + 1U);
        mesh.indices.push_back(p0);
        mesh.indices.push_back(apex);
        mesh.indices.push_back(p1);
    }

    // 底面圆盘（法线 -Y）：与侧面同环序，绕序使从下看为逆时针。
    const std::uint32_t center = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(MakeVertex(0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, material));
    const std::uint32_t baseRing = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int j = 0; j <= radial; ++j) {
        const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(radial);
        mesh.vertices.push_back(MakeVertex(baseX * std::cos(theta), 0.0F, baseZ * std::sin(theta), 0.0F, -1.0F,
                                           0.0F, material));
    }
    for (int j = 0; j < radial; ++j) {
        mesh.indices.push_back(center);
        mesh.indices.push_back(baseRing + static_cast<std::uint32_t>(j));
        mesh.indices.push_back(baseRing + static_cast<std::uint32_t>(j) + 1U);
    }
}

}  // namespace object_mesh_detail

/// **传送门**（V3）：**立起来的门环**（椭圆环面，轴沿 `+Z`，门洞在环心 ⇒ **可以从中间走过**）。
///
/// 尺寸口径：环心在 `(0, halfY, 0)`；**环心线半轴** `(halfX - halfZ, halfY - halfZ)`、管半径 `halfZ`
/// ⇒ 竖直跨度 `0 ~ 2*halfY`、外形**正好**装进 `half_extent` 包围盒；**门洞半高 = `halfY - 2*halfZ`**、
/// **半宽 = `halfX - 2*halfZ`**（调用方据此保证角色能过）。
/// **占位形态**：正式外观（glTF / kit）待美术资源就位；"门叫什么 / 为何能传送"属世界观设定，待所有者提供。
/// 法线为单位向量（环面解析式；椭圆情形为近似，占位可接受）；无退化三角形（环面两向都无极点）。
inline void AppendPortalRing(MeshData& mesh, float halfX, float halfY, float halfZ, int radial,
                             int tubeSegments, float material) {
    const float tube = halfZ;  // 管半径（= 厚度的一半）
    // **环心线**半轴取"外缘减去管半径" ⇒ 外形**正好**装满 `half_extent` 的包围盒，
    // 与其它形态同守"底面在 y = 0、顶面在 2*halfY、|x| ≤ halfX、|z| ≤ halfZ"的约定
    // （放置 / 剔除 / 碰撞都依赖它）。门洞半高 = `halfY - 2*halfZ`、半宽 = `halfX - 2*halfZ`。
    const float a = halfX - tube;
    const float b = halfY - tube;

    const auto appendRing = [&](float phi) {
        const float cosPhi = std::cos(phi);
        const float sinPhi = std::sin(phi);
        for (int i = 0; i <= radial; ++i) {
            const float theta = object_mesh_detail::kTwoPi * static_cast<float>(i) / static_cast<float>(radial);
            const float cosT  = std::cos(theta);
            const float sinT  = std::sin(theta);
            const float offset = tube * cosPhi;  // 环心线沿径向的外扩
            mesh.vertices.push_back(object_mesh_detail::MakeVertex((a + offset) * cosT,
                                                                   halfY + (b + offset) * sinT, tube * sinPhi,
                                                                   cosPhi * cosT, cosPhi * sinT, sinPhi, material));
        }
    };
    for (int j = 0; j <= tubeSegments; ++j) {
        appendRing(object_mesh_detail::kTwoPi * static_cast<float>(j) / static_cast<float>(tubeSegments));
    }

    const std::size_t ringVertexCount = static_cast<std::size_t>(radial) + 1U;
    for (int j = 0; j < tubeSegments; ++j) {
        const std::size_t row0 = static_cast<std::size_t>(j) * ringVertexCount;
        const std::size_t row1 = static_cast<std::size_t>(j + 1) * ringVertexCount;
        for (int i = 0; i < radial; ++i) {
            const std::uint32_t p00 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(i));
            const std::uint32_t p01 = static_cast<std::uint32_t>(row0 + static_cast<std::size_t>(i) + 1U);
            const std::uint32_t p10 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(i));
            const std::uint32_t p11 = static_cast<std::uint32_t>(row1 + static_cast<std::size_t>(i) + 1U);
            // 行 = 管向 `phi`、列 = 环向 `theta`。法线为 `∂P/∂theta × ∂P/∂phi`（朝外）
            // ⇒ 绕序取 `(p00, p01, p11)` / `(p00, p11, p10)`（先列后行），与逐顶点法线一致。
            mesh.indices.push_back(p00);
            mesh.indices.push_back(p01);
            mesh.indices.push_back(p11);
            mesh.indices.push_back(p00);
            mesh.indices.push_back(p11);
            mesh.indices.push_back(p10);
        }
    }
}

/// 构建一个**模块化建筑构件**的代理几何（V0.8；[ADR 0035](../../docs/adr/0035-modular-building-kit-and-enterable-spaces.md) 决策二）。
///
/// 口径（与其它形态共用同一套约定）：**构件局部原点 = 水平中心 / 底面中心**，`+Y` 向上，底面在 `y = 0`；
/// 水平占地 = `2*half_extent.x × 2*half_extent.z`（解析期已强制它是 `module_blocks` 的整数倍）；
/// 法线朝外、绕序从外部看逆时针、**无退化三角形**（与 `AppendBox` 同源实现）。
///
/// | 角色 | 几何 |
/// | --- | --- |
/// | `Floor` / `Roof` | 一块厚 `kKitSlabThicknessBlocks` 的板（顶面 = 可站面） |
/// | `Wall` | 沿 Z 居中、厚 `kKitSlabThicknessBlocks` 的立板（高 = `2*half_extent.y`） |
/// | `WallDoor` | 左 / 右两段立板 + 上方**门楣** ⇒ 洞口宽 `kKitDoorWidthBlocks`、净高 `kKitDoorClearanceBlocks` |
///
/// **可进入性的硬保证**：`WallDoor` 的洞口净高 = `kKitDoorClearanceBlocks`（2.2 格 > 角色总高 1.80 格）；
/// 解析期另强制 `2*half_extent.y > kKitDoorClearanceBlocks`（门楣必须存在）⇒ 不会出现"门洞通到顶"的退化件。
[[nodiscard]] inline MeshData BuildKitPieceMesh(const ObjectType& type) {
    const float halfX = type.halfExtentX;
    const float halfZ = type.halfExtentZ;
    const float height = 2.0F * type.halfExtentY;
    const float thickness = kKitSlabThicknessBlocks;
    const float material = ObjectMaterialSlot(type.kind);

    MeshData mesh;
    switch (type.kitRole) {
        case ObjectKitRole::Floor:
        case ObjectKitRole::Roof:
            // 板：底面贴 `y = 0`，顶面在 `y = thickness`。
            object_mesh_detail::AppendBoxCentered(mesh, 0.0F, thickness * 0.5F, 0.0F, halfX, thickness * 0.5F, halfZ,
                                                  material);
            break;
        case ObjectKitRole::Wall:
            // 立板：占满模数格的**地面投影**，几何 = 沿 Z 居中的一块薄板（拼接时相邻件自然对齐）。
            object_mesh_detail::AppendBoxCentered(mesh, 0.0F, height * 0.5F, 0.0F, halfX, height * 0.5F,
                                                  thickness * 0.5F, material);
            break;
        case ObjectKitRole::WallDoor: {
            // 左 / 右两段 + 门楣：洞口 = 宽 `kKitDoorWidthBlocks`、高 `kKitDoorClearanceBlocks`。
            const float halfDoor  = kKitDoorWidthBlocks * 0.5F;
            const float doorTop   = kKitDoorClearanceBlocks;
            const float sideHalfX = (halfX - halfDoor) * 0.5F;              // 单侧板块的半宽
            const float sideCenterX = halfDoor + sideHalfX;                 // 单侧板块的中心 |x|
            object_mesh_detail::AppendBoxCentered(mesh, -sideCenterX, height * 0.5F, 0.0F, sideHalfX, height * 0.5F,
                                                  thickness * 0.5F, material);
            object_mesh_detail::AppendBoxCentered(mesh, sideCenterX, height * 0.5F, 0.0F, sideHalfX, height * 0.5F,
                                                  thickness * 0.5F, material);
            // 门楣（洞口上方到墙顶）—— 解析期已保证 `height > doorTop` ⇒ 高度为正，不产生退化件。
            const float lintelHalfY = (height - doorTop) * 0.5F;
            object_mesh_detail::AppendBoxCentered(mesh, 0.0F, doorTop + lintelHalfY, 0.0F, halfDoor, lintelHalfY,
                                                  thickness * 0.5F, material);
            break;
        }
    }
    return mesh;
}

/// 构建物件的**局部**网格（底面中心为原点、`+Y` 向上、底面在 `y = 0`）。
///
/// 渲染与碰撞**共用这一份 `MeshData`**（"谁画谁挡"同源，尺寸不可能漂移）：渲染侧由
/// `MeshRenderer::SetMeshTransform` 施加位姿；物理侧把"朝向烘进顶点"后交 `PhysicsWorld::AddMesh`
/// （静态体没有旋转接口，见 `RotateMeshAboutY`）。顶点为局部小数值（红线 6）。
///
/// 法线朝外且为单位向量；三角形绕序在**从外部观察**时为逆时针（与渲染器一致）。
inline MeshData BuildObjectMesh(const ObjectType& type, const ObjectMeshSpec& spec = ObjectMeshSpec {}) {
    const int   radial   = std::max(spec.radialSegments, 3);
    const int   rings    = std::max(spec.rings, 2);
    const float halfX    = type.halfExtentX;
    const float halfY    = type.halfExtentY;
    const float halfZ    = type.halfExtentZ;
    const float material = ObjectMaterialSlot(type.kind);

    MeshData mesh;
    switch (type.kind) {
        case ObjectAssetKind::Crate:
            object_mesh_detail::AppendBox(mesh, halfX, halfY, halfZ, material);
            break;
        case ObjectAssetKind::Stone:
            object_mesh_detail::AppendEllipsoid(mesh, halfX, halfY, halfZ, radial, rings, material);
            break;
        case ObjectAssetKind::DirtPile:
            object_mesh_detail::AppendCone(mesh, halfX, halfZ, halfY, radial, rings, material);
            break;
        case ObjectAssetKind::Portal:
            AppendPortalRing(mesh, halfX, halfY, halfZ, radial, rings, material);
            break;
        case ObjectAssetKind::Model:
            // V8：外部模型**不是**程序化几何 —— 由 `BuildObjectMeshFromModel` 产出（需要文件 IO，
            // 因此不放在本纯几何函数里）。这里留**空网格**，调用方（`game/main.cpp`）按形态分流。
            break;
        case ObjectAssetKind::Kit:
            // V0.8：建筑构件 = **纯程序化代理体**（无文件 IO）⇒ 直接由 `BuildKitPieceMesh` 产出。
            mesh = BuildKitPieceMesh(type);
            break;
    }
    return mesh;
}

/// **外部模型** → 物件局部网格（V8）：把 `Model` 的全部网格合并，**等比缩放"装进" `2*half_extent` 的盒**、
/// **底面贴 `y = 0`**、**水平居中** —— 与程序化形态同守"底面中心为原点、`+Y` 向上"的约定。
///
/// 为什么"装进盒"而不是"按模型原尺寸"：GLB 的建模单位与轴向各异（Kenney 的树约 1~2 单位、有的模型带偏移），
/// 由 `half_extent` 统一给出**目标尺寸** ⇒ 同一模型在不同类型下可得不同大小，且**落点 / 剔除包围盒 / 碰撞体**
/// 都可预期（与程序化形态同一套下游逻辑，零分叉）。
///
/// 前置条件：`model.meshes` 非空且包围盒**非退化**（至少一个轴向尺寸 > 0）；否则返回**空网格**
/// （调用方按"网格为空 ⇒ 跳过上传"处理，与既有口径一致）。
/// **已知限制**：`LoadModel` **不烘焙节点变换**（见 `model_loader.cpp`）⇒ 本函数只适用**单节点 / 无变换**模型
/// （Kenney Nature Kit 的 12 个 GLB 即如此）；带层级变换的模型须先烘焙（登记为后续）。
[[nodiscard]] inline MeshData BuildObjectMeshFromModel(const Model& model, float halfX, float halfY, float halfZ,
                                                       float material) {
    MeshData mesh;

    // 1) 模型空间包围盒。
    bool  have = false;
    float minP[3] = { 0.0F, 0.0F, 0.0F };
    float maxP[3] = { 0.0F, 0.0F, 0.0F };
    for (const ModelMesh& source : model.meshes) {
        for (const ModelVertex& vertex : source.vertices) {
            const float p[3] = { vertex.position.x, vertex.position.y, vertex.position.z };
            if (!have) {
                for (int axis = 0; axis < 3; ++axis) {
                    minP[axis] = p[axis];
                    maxP[axis] = p[axis];
                }
                have = true;
                continue;
            }
            for (int axis = 0; axis < 3; ++axis) {
                minP[axis] = std::min(minP[axis], p[axis]);
                maxP[axis] = std::max(maxP[axis], p[axis]);
            }
        }
    }
    if (!have) {
        return mesh;
    }

    // 2) **等比**缩放因子 = 各轴「目标 / 实际」的**最小值**（不拉伸；退化轴不参与 ⇒ 不产生除零）。
    const float size[3]   = { maxP[0] - minP[0], maxP[1] - minP[1], maxP[2] - minP[2] };
    const float target[3] = { 2.0F * halfX, 2.0F * halfY, 2.0F * halfZ };
    float       scale     = 0.0F;
    for (int axis = 0; axis < 3; ++axis) {
        if (size[axis] > 0.0F) {
            const float candidate = target[axis] / size[axis];
            scale = (scale <= 0.0F) ? candidate : std::min(scale, candidate);
        }
    }
    if (scale <= 0.0F) {
        return mesh;  // 三个轴全部退化 ⇒ 无有效几何
    }

    // 3) 缩放 + 平移：底面贴 `y = 0`（Y 取下界）、水平居中（XZ 取包围盒中心）。
    //    等比缩放 ⇒ **法线不需重算 / 归一化**，原样搬运即可。
    const float centerX = (minP[0] + maxP[0]) * 0.5F;
    const float centerZ = (minP[2] + maxP[2]) * 0.5F;
    for (const ModelMesh& source : model.meshes) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const ModelVertex& vertex : source.vertices) {
            mesh.vertices.push_back(object_mesh_detail::MakeVertex((vertex.position.x - centerX) * scale,
                                                                   (vertex.position.y - minP[1]) * scale,
                                                                   (vertex.position.z - centerZ) * scale,
                                                                   vertex.normal.x, vertex.normal.y, vertex.normal.z,
                                                                   material));
        }
        for (const std::uint32_t index : source.indices) {
            mesh.indices.push_back(base + index);
        }
    }
    return mesh;
}

/// 把网格**绕 Y 轴**旋转 `yawDegrees`（位置与法线一并旋转；纯函数，输入不变）。
///
/// 用途：**静态碰撞体没有旋转接口**（`PhysicsWorld::MeshDesc` 只带平移原点）⇒ 物件的朝向必须
/// **烘进顶点**。旋转方向与渲染侧 `MeshRenderer::SetMeshTransform` 的四元数一致（绕 `+Y`、右手系），
/// 因此碰撞与视觉不会出现"朝向对不上"。旋转轴 = 局部原点（= 底面中心）。
[[nodiscard]] inline MeshData RotateMeshAboutY(const MeshData& mesh, float yawDegrees) {
    const float radians = yawDegrees * object_mesh_detail::kPi / 180.0F;
    const float c       = std::cos(radians);
    const float s       = std::sin(radians);

    MeshData out = mesh;
    for (MeshVertex& vertex : out.vertices) {
        const float px = vertex.position[0];
        const float pz = vertex.position[2];
        vertex.position[0] = c * px + s * pz;
        vertex.position[2] = -s * px + c * pz;

        const float nx = vertex.normal[0];
        const float nz = vertex.normal[2];
        vertex.normal[0] = c * nx + s * nz;
        vertex.normal[2] = -s * nx + c * nz;
    }
    return out;
}

/// 生成一个**低模代理**（V0.7 H4 / [ADR 0034](../../docs/adr/0034-object-instancing-and-hlod.md) 决策四）：
/// 用**顶点聚类**（vertex clustering）做简化，供远景 LOD 链使用。
///
/// 做法：把局部坐标按 `cellBlocks` 的立方格聚类（同格顶点合并为一个代表点：位置 = 簇内平均、
/// 法线 = 平均后归一化、材质槽 = 簇内首个），再按簇 id 重建三角形并丢弃**退化片**（两簇相同）与重复片。
/// 纯函数、**确定性**（红线 7；用 `std::map` 保证簇顺序与输入顺序无关），只在**加载期**调用。
///
/// 为什么用聚类而不是边坍缩：实现小、对任意网格鲁棒、顶点数随格距**单调下降**，且与拓扑 / 朝向无关
/// （本工程的物件既有程序化形态也有外部低模，形态差异大）。`cellBlocks <= 0`、结果为空或代理顶点 `< 8`
/// ⇒ **原样返回**（调用方据此决定是否真的使用代理 —— 若代理不比原网格小就没有意义）。
///
/// **已知取舍（登记在 ADR 0034）**：聚类会轻微改变轮廓 ⇒ 只应在**远处**（像素占有率很低）切换。
[[nodiscard]] inline MeshData BuildLowPolyProxy(const MeshData& mesh, float cellBlocks) {
    if (cellBlocks <= 0.0F || mesh.vertices.empty() || mesh.indices.empty()) {
        return mesh;
    }

    struct Cluster {
        float         position[3];
        float         normalAccumulator[3];
        float         material;
        std::uint32_t count;
    };

    std::map<std::array<int, 3>, std::uint32_t> clusterOf;
    std::vector<Cluster>                        clusters;
    std::vector<std::uint32_t>                  vertexCluster(mesh.vertices.size(), 0U);
    const float                                 invCell = 1.0F / cellBlocks;

    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        const std::array<int, 3> key { static_cast<int>(std::floor(vertex.position[0] * invCell)),
                                       static_cast<int>(std::floor(vertex.position[1] * invCell)),
                                       static_cast<int>(std::floor(vertex.position[2] * invCell)) };
        const auto found = clusterOf.find(key);
        if (found == clusterOf.end()) {
            const std::uint32_t id = static_cast<std::uint32_t>(clusters.size());
            clusterOf.emplace(key, id);
            clusters.push_back(Cluster { { vertex.position[0], vertex.position[1], vertex.position[2] },
                                         { vertex.normal[0], vertex.normal[1], vertex.normal[2] }, vertex.material,
                                         1U });
            vertexCluster[i] = id;
        } else {
            Cluster& cluster = clusters[found->second];
            cluster.position[0] += vertex.position[0];
            cluster.position[1] += vertex.position[1];
            cluster.position[2] += vertex.position[2];
            cluster.normalAccumulator[0] += vertex.normal[0];
            cluster.normalAccumulator[1] += vertex.normal[1];
            cluster.normalAccumulator[2] += vertex.normal[2];
            ++cluster.count;
            vertexCluster[i] = found->second;
        }
    }

    MeshData out;
    out.vertices.reserve(clusters.size());
    for (const Cluster& cluster : clusters) {
        const float inv = 1.0F / static_cast<float>(cluster.count);
        MeshVertex  vertex;
        vertex.position[0] = cluster.position[0] * inv;
        vertex.position[1] = cluster.position[1] * inv;
        vertex.position[2] = cluster.position[2] * inv;
        float nx = cluster.normalAccumulator[0];
        float ny = cluster.normalAccumulator[1];
        float nz = cluster.normalAccumulator[2];
        const float lengthSquared = nx * nx + ny * ny + nz * nz;
        if (lengthSquared > 1.0e-12F) {
            const float invLength = 1.0F / std::sqrt(lengthSquared);
            nx *= invLength;
            ny *= invLength;
            nz *= invLength;
        } else {
            nx = 0.0F;
            ny = 1.0F;
            nz = 0.0F;  // 退化（法线互相抵消）⇒ 回退 +Y（与 `MeshVertex` 缺省一致）
        }
        vertex.normal[0] = nx;
        vertex.normal[1] = ny;
        vertex.normal[2] = nz;
        vertex.material  = cluster.material;
        vertex.morph     = 0.0F;
        out.vertices.push_back(vertex);
    }

    out.indices.reserve(mesh.indices.size());
    std::set<std::array<std::uint32_t, 3>> seen;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t a = vertexCluster[mesh.indices[t + 0]];
        const std::uint32_t b = vertexCluster[mesh.indices[t + 1]];
        const std::uint32_t c = vertexCluster[mesh.indices[t + 2]];
        if (a == b || b == c || a == c) {
            continue;  // 退化片（合并后塌成一条线 / 一个点）
        }
        if (!seen.insert(std::array<std::uint32_t, 3> { a, b, c }).second) {
            continue;  // 重复片
        }
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }

    if (out.indices.empty() || out.vertices.size() < 8U) {
        // 聚类过度（格距太大）⇒ 几何被塌成一两个面 / 空网格 ⇒ 回退原网格，绝不产生"退化代理"。
        return mesh;
    }
    return out;
}

}  // namespace vx
