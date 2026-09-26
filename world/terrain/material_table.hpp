#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vx {

/// splat 槽位数量：等于纹理数组的层数，并与 `render/mesh_renderer.hpp` 的纹理采样约定一致。
///
/// ADR 0009 起权重在**片元着色器**逐像素计算，不再写入顶点属性；本常量只约束
/// 「材质表行数 == 纹理数组层数 == 片元着色器可采样的层数上限」。
inline constexpr int kMaterialSlotCount = 4;

/// 一个地表材质层（= 一个 splat 槽位）的配置。
///
/// 权重模型：`weight = heightFactor * slopeFactor`，两个因子分别是「高度带」与「坡度带」的
/// 平滑隶属度（见 `material_blender.hpp`）。同一份表既可表达「低地草、高地岩」，
/// 也可表达「陡坡露岩」。
///
/// ADR 0009 起本表是**唯一事实来源**：高度带 / 坡度带 / 每层 UV 尺度 / 层色（tint）都被
/// 打包成 GPU uniform 块（`MaterialUniform`），CPU 与 GPU 消费的是同一份数值，杜绝漂移。
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

    float uvScale = 1.0F;  ///< 每层平面 UV 尺度（每世界格的纹理重复次数）；必须 > 0
    float tintR   = 1.0F;  ///< 层色（乘在采样到的 albedo 上）；取值 [0, 1]
    float tintG   = 1.0F;
    float tintB   = 1.0F;
};

/// GPU 侧的一个材质层参数块；字段排布与 `assets/shaders/mesh.frag` 的 std140 块逐字对应。
///
/// 布局（std140，每行一个 `vec4`）：
///   - `height` = `(heightMin, heightMax, heightBlend, textureIndex)`，`textureIndex = textureLayer - 1`
///   - `slope`  = `(slopeMin, slopeMax, slopeBlend, 0)`
///   - `tintUv` = `(tintR, tintG, tintB, uvScale)`
struct MaterialLayerUniform {
    float heightMin      = 0.0F;
    float heightMax      = 0.0F;
    float heightBlend    = 0.0F;
    float textureIndex   = 0.0F;
    float slopeMin       = 0.0F;
    float slopeMax       = 0.0F;
    float slopeBlend     = 0.0F;
    float slopeUnused    = 0.0F;
    float tintR          = 1.0F;
    float tintG          = 1.0F;
    float tintB          = 1.0F;
    float uvScale        = 1.0F;
};

/// 片元着色器的材质 uniform 块（`set = 3, binding = 0`）。
///
/// `renderOrigin` 是相机相对渲染的**渲染原点**：顶点上传前已减去它，片元用它把相机相对坐标
/// 还原成世界坐标，才能按世界高度算权重（红线 6：世界定位不用 `float` 存储，仅上传时转换）。
struct MaterialUniform {
    float renderOriginX = 0.0F;
    float renderOriginY = 0.0F;
    float renderOriginZ = 0.0F;
    float renderOriginUnused = 0.0F;
    std::array<MaterialLayerUniform, static_cast<std::size_t>(kMaterialSlotCount)> layers {};
};

static_assert(sizeof(MaterialUniform) == 16 + 48 * static_cast<std::size_t>(kMaterialSlotCount),
              "MaterialUniform 必须与 mesh.frag 的 std140 布局逐字节一致");

class TerrainMaterialTable;

/// 用**同一份**已加载材质表构建 GPU uniform 块（唯一事实来源，ADR 0009 后果一节）。
///
/// 这是 CPU→GPU 材质参数的**唯一**入口：任何新增参数都必须先加进 `MaterialLayer`，
/// 再在这里投影；**禁止**在着色器里硬编码第二份带 / 尺度 / 层色。
/// 前置条件：`table` 已通过 `LoadFromFile` 或 `Default()` 填充。
[[nodiscard]] MaterialUniform BuildMaterialUniform(const TerrainMaterialTable& table, double originX,
                                                   double originY, double originZ) noexcept;

/// 地表材质表：启动期从 `assets/config/materials.toml` 一次性读入（ADR 0005）。
///
/// 边界：配置表是**世界定义的输入**，不是世界状态；运行期只读，热路径不触碰解析器。
/// 加载失败（文件缺失 / 语法错 / 校验不过 / `schema_version` 不符）一律**抛异常**并中止启动，
/// **禁止**静默回退到默认值。
class TerrainMaterialTable {
public:
    /// 当前表格式版本；写入配置文件的 `schema_version` 必须与之相等。
    /// 2：新增 `uv_scale` 与 `tint_r/g/b`（ADR 0009）。
    static constexpr int kSchemaVersion = 2;

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
