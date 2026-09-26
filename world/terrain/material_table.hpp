#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vx {

/// 全局三平面（triplanar）混合配置（C 项 / 陡壁 UV 拉伸修复）。
///
/// **为什么"自动切换"是硬要求**：地形会被笔刷挖与堆，混合权重若在 CPU 侧按坡度预烘焙、或按 tile 分支、
/// 或需要重建网格，则地形一变就得重做——本项目**不允许**该路径。故混合权重**逐像素由世界空间法线**算出
/// （见 `TriplanarBlendWeight` 与 `assets/shaders/mesh.frag` 的 `triplanarWeight`）：法线一变混合即自动跟随。
struct TriplanarSettings {
    /// false 时着色器整段跳过三平面（混合权重恒为 0，退回纯平面投影）。
    bool enabled = true;

    /// 自动切换下界：坡度 `slope = 1 - |N.y|`（0 = 水平面、1 = 竖直面）低于它 → 纯平面路径（零额外采样）。
    float slopeMin = 0.45F;

    /// 自动切换上界：坡度达到它 → 完全三平面（按 |N| 的幂在三个轴投影间混合）。必须 > `slopeMin`。
    float slopeMax = 0.70F;

    /// 三轴投影的锐化指数（必须 > 0）：越大越"只取最贴合的那一两个轴"，过渡越干脆。
    float sharpness = 4.0F;
};

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

    /// PBR 粗糙度基准（ADR 0010 P2）；取值 [0, 1]。地表为电介质，**不引入 metallic**。
    /// 最终粗糙度 = 本值 × 粗糙度贴图的逐像素 ±20% 变化，再钳到 `kMinRoughness`（避免 GGX 除零）。
    float roughness = 1.0F;

    /// 环境光遮蔽系数；取值 [0, 1]。**只作用于天空光 / 环境项**，不作用于直接光（ADR 0010 P2）。
    float ao = 1.0F;

    /// 宏观变化贴图的 UV 尺度（每世界格的重复次数）；必须 > 0，且**显著小于 `uvScale`**。
    /// 宏观变化贴图只有 1 层，故各层用各自的尺度采样同一张图，用于打破基础贴图的平铺重复感。
    float macroUvScale = 0.02F;

    /// 宏观调制强度；取值 [0, 1]。乘在 albedo 与 roughness 上（围绕 1 上下浮动）。
    float macroStrength = 0.0F;

    /// **在可挖体积中替代本层的槽位**（[ADR 0014](../../docs/adr/0014-voxel-material-index.md)）；
    /// `-1` = 用本层自身。用于「表层 → 次表层」映射：`grass` / `sand` 这类只该出现在地表薄层的层，
    /// 在体积内映射为 `dirt` ⇒ **在草地上挖坑看到土、在山体（陡坡 ⇒ 岩）里挖洞看到岩**。
    int subsurfaceSlot = -1;
};

/// GPU 侧的一个材质层参数块；字段排布与 `assets/shaders/mesh.frag` 的 std140 块逐字对应。
///
/// 布局（std140，每行一个 `vec4`；共 4 行 = 64 字节）：
///   - `height`  = `(heightMin, heightMax, heightBlend, textureIndex)`，`textureIndex = textureLayer - 1`
///   - `slope`   = `(slopeMin, slopeMax, slopeBlend, roughness)` —— `w` 复用为 PBR 粗糙度，填满该 `vec4`
///   - `tintUv`  = `(tintR, tintG, tintB, uvScale)`
///   - `macroAo` = `(macroUvScale, macroStrength, ao, 0)` —— `w` 是整块**唯一**的填充槽
///
/// 4 个新增字段（roughness / macroUvScale / macroStrength / ao）**紧凑排进一个 `vec4`**，
/// 并把既有的 `slope.w` 填充位复用给 `roughness`，故 15 个有效字段只占 4 个 `vec4`、仅 1 个填充槽。
struct MaterialLayerUniform {
    // vec4 #1：高度带
    float heightMin    = 0.0F;
    float heightMax    = 0.0F;
    float heightBlend  = 0.0F;
    float textureIndex = 0.0F;
    // vec4 #2：坡度带；w 复用为 PBR 粗糙度
    float slopeMin    = 0.0F;
    float slopeMax    = 0.0F;
    float slopeBlend  = 0.0F;
    float roughness   = 1.0F;
    // vec4 #3：层色与每层 UV 尺度
    float tintR       = 1.0F;
    float tintG       = 1.0F;
    float tintB       = 1.0F;
    float uvScale     = 1.0F;
    // vec4 #4：宏观变化与 AO（w = 填充）
    float macroUvScale   = 0.02F;
    float macroStrength  = 0.0F;
    float ao             = 1.0F;
    float macroAoUnused  = 0.0F;
};

/// 片元着色器的材质 uniform 块（`set = 3, binding = 0`）。
///
/// `renderOrigin` 是相机相对渲染的**渲染原点**：顶点上传前已减去它，片元用它把相机相对坐标
/// 还原成世界坐标，才能按世界高度算权重（红线 6：世界定位不用 `float` 存储，仅上传时转换）。
///
/// `triplanar` 是全局三平面参数（C 项）：`x = enabled(1/0)`、`y = slope_min`、`z = slope_max`、
/// `w = sharpness`。与 mesh.frag 的 `MaterialBlock.triplanar` 逐字对应。
struct MaterialUniform {
    float renderOriginX = 0.0F;
    float renderOriginY = 0.0F;
    float renderOriginZ = 0.0F;
    float renderOriginUnused = 0.0F;

    float triplanarEnabled  = 1.0F;  ///< 1 = 启用三平面、0 = 关闭（着色器据此整段跳过）
    float triplanarSlopeMin = 0.45F;
    float triplanarSlopeMax = 0.70F;
    float triplanarSharpness = 4.0F;

    std::array<MaterialLayerUniform, static_cast<std::size_t>(kMaterialSlotCount)> layers {};
};

static_assert(sizeof(MaterialUniform) == 16 * (2 + 4 * static_cast<std::size_t>(kMaterialSlotCount)),
              "MaterialUniform 必须与 mesh.frag 的 std140 布局逐字节一致"
              "（渲染原点 + 三平面参数 + 4 层 × 4 个 vec4 = 288 字节）");

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
    /// 3：新增 `roughness` / `ao` / `macro_uv_scale` / `macro_strength`（ADR 0010 P2）。
    /// 4：调整高度 / 坡度带使 (高度 × 坡度) 全域被覆盖（缺陷 2 修复）；并新增全局 `[triplanar]` 段（C 项，
    ///    同一版本内落地，不重复升版）、每层可选的 `subsurface`（ADR 0014 的表层 → 次表层映射）。
    ///    `subsurface` **缺省 = 自身**，旧文件无需改动即可加载 ⇒ **不构成破坏性变更，故不升版**。
    static constexpr int kSchemaVersion = 4;

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

    /// 全局三平面（triplanar）参数（C 项）。供 `BuildMaterialUniform` 与着色器使用。
    [[nodiscard]] const TriplanarSettings& Triplanar() const noexcept { return m_triplanar; }

private:
    std::array<MaterialLayer, static_cast<std::size_t>(kMaterialSlotCount)> m_layers {};
    TriplanarSettings                                                       m_triplanar;
    int                                                                     m_schemaVersion = kSchemaVersion;
};

}  // namespace vx
