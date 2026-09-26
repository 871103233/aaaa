#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

namespace vx {

/// splat 槽位数量：与 `render/mesh_renderer.hpp` 里 `MeshVertex::materialWeights` 的 4 个分量一一对应。
inline constexpr int kMaterialSlotCount = 4;

/// 一个地表材质层（= 一个 splat 槽位）的配置。
///
/// 权重模型：`weight = heightFactor * slopeFactor`，两个因子分别是「高度带」与「坡度带」的
/// 平滑隶属度（见 `material_blender.hpp`）。同一份表既可表达「低地草、高地岩」，
/// 也可表达「陡坡露岩」。
struct MaterialLayer {
    std::string name;  ///< 供调试与日志；不参与权重计算。

    /// 纹理数组层号。`0` 号层保留给"缺失纹理"占位，故合法值从 `1` 起
    /// （见 `.trae/skills/voxel-engine-dev-standards/references/meshing-and-render.md` §3）。
    int textureLayer = 1;

    float heightMin   = 0.0F;  ///< 高度带下界（格）
    float heightMax   = 0.0F;  ///< 高度带上界（格）
    float heightBlend = 0.0F;  ///< 高度过渡带宽度（格）；0 表示硬边界

    float slopeMin   = 0.0F;  ///< 坡度带下界（`1 - normal.y`，0 = 水平面、1 = 垂直面）
    float slopeMax   = 0.0F;  ///< 坡度带上界
    float slopeBlend = 0.0F;  ///< 坡度过渡带宽度
};

/// 地表材质表：启动期从 `assets/config/materials.toml` 一次性读入（ADR 0005）。
///
/// 边界：配置表是**世界定义的输入**，不是世界状态；运行期只读，热路径不触碰解析器。
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值。
class TerrainMaterialTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    static constexpr int kSchemaVersion = 1;

    /// 从 TOML 文件加载并校验；失败抛 `std::runtime_error`（启动期允许异常，ADR 0005）。
    /// 前置条件：`path` 指向待加载的材质表文件。
    [[nodiscard]] static TerrainMaterialTable LoadFromFile(const std::filesystem::path& path);

    /// 内置默认表：仅供不读配置文件的单元测试与离线工具使用。
    /// **不是** `LoadFromFile` 失败时的回退路径 —— 那条路径必须报错。
    [[nodiscard]] static TerrainMaterialTable Default();

    /// 前置条件：`slot ∈ [0, kMaterialSlotCount)`。
    [[nodiscard]] const MaterialLayer& Layer(int slot) const noexcept {
        return m_layers[static_cast<std::size_t>(slot)];
    }

    [[nodiscard]] int SchemaVersion() const noexcept { return m_schemaVersion; }

private:
    std::array<MaterialLayer, static_cast<std::size_t>(kMaterialSlotCount)> m_layers {};
    int                                                                     m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
