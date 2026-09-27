#pragma once

#include "terrain/material_table.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vx {

/// 程序生成的占位材质贴图的默认每层边长（像素）。
///
/// 保持 256（ADR 0010 P2 的显存记账区间即按 256² 给出）：五张纹理数组（albedo / normal /
/// roughness / AO 各 4 层 + macro 1 层）含 mip 约 5.67 MB，落在方案 §7.2.1 的
/// 「P2 ≈ 5.6 MB（256²）～ 22.4 MB（512²）」区间内。
inline constexpr std::uint32_t kMaterialTextureSize = 256;

/// 宏观变化（macro variation）纹理数组的层数：**只有 1 层**（ADR 0010 P2）。
/// 各材质层用自己的 `macro_uv_scale` 采样这同一张低频大尺度噪声，用于打破平铺重复感。
inline constexpr std::uint32_t kMaterialMacroLayerCount = 1;

/// 占位材质贴图集（ADR 0009 / ADR 0010 P2）：albedo / normal / roughness / AO 各 4 层，
/// 外加 1 层的宏观变化图，均为 `R8G8B8A8_UNORM`、layer-major 连续。
///
/// - **albedo**：由已有噪声生成的**单色细节**，且为**两个频段叠加**（低频"结构" + 高频"颗粒"）。
///   单频噪声看起来永远"平"，叠加高频段是"细腻"的关键（ADR 0010 P2）。实际层色由
///   `assets/config/materials.toml` 的 `tint_*` 决定（着色器里 `albedo * tint`），
///   因此"颜色"只有一个来源，贴图只负责细节与颗粒。
/// - **normal**：由**两个频段**叠加后的高度噪声梯度生成（`n = normalize(-dH/du, -dH/dv, 1)`），
///   编码为 `[0,255]`；解码后单位长度（误差仅来自 8 位量化）。
/// - **roughness**：逐像素粗糙度**变化**（灰阶，rgb 同值、a = 255）。基准值仍来自材质表的
///   `roughness`；着色器把它折算成 ±20% 的乘子（`mix(0.8, 1.2, tex)`），使高光有细微断续。
/// - **ao**：逐像素环境光遮蔽细节（灰阶，值域 `[0.6, 1.0]`，1 = 完全开阔）。基准值来自材质表
///   的 `ao`；着色器取两者的乘积作为**环境项（天空光）系数**（不作用于直接光）。
/// - **macro**：**1 层**低频大尺度噪声（值域 `[0,1]`，存入 R 通道；rgb 同值便于调试、a = 255），
///   供片元按 `macro_uv_scale` 采样，再用 `macro_strength` 调制 albedo 与 roughness。
///
/// 确定性：同一 `worldSeed` + 同一 `size` ⇒ **逐字节相同**（红线 7；不依赖时间 / 线程顺序）。
/// 无缝：亮度 / 高度 / 宏观等噪声在 tile 上做周期混合，重复平铺不出现硬接缝。
struct MaterialTextureSet {
    std::uint32_t             size       = kMaterialTextureSize;               ///< 每层边长（像素）
    std::uint32_t             layerCount = static_cast<std::uint32_t>(kMaterialSlotCount);
    std::vector<std::uint8_t> albedoRgba;     ///< layer-major：层 L 的像素从 `L * size * size * 4` 开始
    std::vector<std::uint8_t> normalRgba;     ///< 同上；xyz 存于 rgb，a 恒为 255
    std::vector<std::uint8_t> roughnessRgba;  ///< 同上；粗糙度变化存于 rgb（灰阶），a 恒为 255
    std::vector<std::uint8_t> aoRgba;         ///< 同上；AO 细节存于 rgb（灰阶），a 恒为 255
    std::vector<std::uint8_t> macroRgba;      ///< **1 层**；低频噪声存于 rgb（灰阶），a 恒为 255
};

/// 生成占位材质贴图集（纯函数、确定性、无二进制资产）。
///
/// 前置条件：`size ≥ 4`（法线用中心差分，需要邻域）。
/// 返回的各数组尺寸：albedo / normal / roughness / ao 为 `layerCount * size * size * 4`；
/// macro 为 `kMaterialMacroLayerCount * size * size * 4`。
///
/// 本函数等价于"建一个 `MaterialTextureBuilder` 再一次跑完"，两者**逐字节相同**（红线 7）。
/// 启动路径请改用构建器分步推进 —— 本函数一次跑完约需 1.9 s（256²），会让画面停下等待
/// （见 SKILL「不冻结画面」）。
[[nodiscard]] MaterialTextureSet GenerateMaterialTextures(std::uint64_t worldSeed,
                                                          std::uint32_t   size = kMaterialTextureSize);

/// **可分步（可切帧）**的材质贴图生成器：与 `GenerateMaterialTextures` 是同一份实现，
/// 只是把"什么时候算完"交给调用方 —— 每次 `Step` 只算几个像素行（数毫秒），
/// 调用方在两次调用之间出一帧加载画面即可让窗口与进度持续刷新。
///
/// 用途（SKILL「不冻结画面」）：启动加载时把 1.9 s 的贴图生成摊到数百帧上，玩家看到的是
/// "进度在走"，而不是"画面停住"。**分步只改变"何时可见"，不改变任何字节**（红线 7）。
///
/// 线程约定：只在逻辑线程（主线程）使用；生命周期内不触碰 GPU（上传由调用方做）。
class MaterialTextureBuilder final {
public:
    /// 前置条件：`size ≥ 4`（法线用中心差分，需要邻域）。
    explicit MaterialTextureBuilder(std::uint64_t worldSeed, std::uint32_t size = kMaterialTextureSize);
    ~MaterialTextureBuilder();

    MaterialTextureBuilder(const MaterialTextureBuilder&) = delete;
    MaterialTextureBuilder& operator=(const MaterialTextureBuilder&) = delete;
    MaterialTextureBuilder(MaterialTextureBuilder&&) = delete;
    MaterialTextureBuilder& operator=(MaterialTextureBuilder&&) = delete;

    /// 处理至多 `maxRows` 个像素行（"逐层像素 → 逐层法线 → 宏观变化"连续计数），返回是否全部完成。
    /// `maxRows == 0` 时不做任何工作，只返回当前是否完成。
    bool Step(std::size_t maxRows);

    [[nodiscard]] bool Done() const noexcept;

    /// 总进度（已完成行数 / 总行数），恒 ∈ `[0, 1]`。
    [[nodiscard]] float Progress() const noexcept;

    /// 取走结果（**只能调用一次**；之后本对象的进度语义不再有意义）。前置条件：`Done()`。
    [[nodiscard]] MaterialTextureSet Take();

private:
    /// 实现细节（噪声实例 / 分步状态）留在 .cpp：`FastNoiseLite` 依赖是 `voxel_world` 的
    /// **PRIVATE** 依赖（见 `world/CMakeLists.txt`），不得经公共头泄漏给 game / tests。
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vx
