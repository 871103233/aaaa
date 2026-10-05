// 离线烘焙 CLI（阶段 W / W2-S2b；[ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）。
//
// 用法：`voxel_bake <地图预设 .toml> <输出的预制地图文件>`
//
// 当前只烘焙**宏地形高度场**（与运行时生成同源）；体积壳网格 / 水体在 W4~W6 追加块类型。
// 确定性：同一预设两次运行 ⇒ 输出文件**逐字节相同**。

#include "core/log.hpp"
#include "generation/map_preset.hpp"
#include "generation/terrain_params.hpp"
#include "platform/command_line.hpp"
#include "premade/premade_bake.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

int main(int argc, char** argv) {
    // 命令行按 UTF-8 取回（Windows 的 CRT `argv` 会把中文路径变成 `?`，见 engine/platform/command_line.hpp）。
    const std::vector<std::string> args = vx::CommandLineArgumentsUtf8(argc, argv);
    if (args.size() != 4) {
        VX_LOG_ERROR("用法：voxel_bake <地表生成参数 terrain.toml> <地图预设 .toml> <输出的预制地图文件>"
                     "（当前参数 %zu 个）",
                     args.size());
        return 2;
    }

    const std::filesystem::path terrainPath = args[1];
    const std::filesystem::path presetPath  = args[2];
    const std::filesystem::path outputPath  = args[3];

    try {
        const vx::TerrainGenerationParams terrainParams = vx::TerrainGenerationParams::LoadFromFile(terrainPath);
        const vx::MapPreset               preset        = vx::MapPreset::LoadFromFile(presetPath);
        const std::size_t tileCount = static_cast<std::size_t>(2 * preset.tileRadiusX + 1) *
                                      static_cast<std::size_t>(2 * preset.tileRadiusZ + 1);
        VX_LOG_INFO("烘焙开始：地图 \"%s\"（种子 %llu，tile 半径 [%d, %d]，地形编辑 %zu 条，tile 数 %zu）；"
                    "地貌分区 %s",
                    preset.name.c_str(), static_cast<unsigned long long>(preset.seed), preset.tileRadiusX,
                    preset.tileRadiusZ, preset.edits.size(), tileCount,
                    terrainParams.landform.enabled ? "**启用**" : "关闭");

        // 输出目录可能不存在（如 build/premade）⇒ 先建目录，再写文件。
        if (const std::filesystem::path parent = outputPath.parent_path(); !parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error) {
                VX_LOG_ERROR("无法创建输出目录 %s：%s", parent.string().c_str(), error.message().c_str());
                return 1;
            }
        }

        vx::BakeMacroHeightTilesIntoPremadeMap(preset, terrainParams, outputPath);

        std::error_code  error;
        const std::uintmax_t sizeBytes = std::filesystem::file_size(outputPath, error);
        if (error) {
            VX_LOG_ERROR("烘焙后无法读取输出文件大小：%s", error.message().c_str());
            return 1;
        }
        VX_LOG_INFO("烘焙完成：%s（%llu 字节，%zu 个块，块类型 = 宏地形高度场）", outputPath.string().c_str(),
                    static_cast<unsigned long long>(sizeBytes), tileCount);
        return 0;
    } catch (const std::exception& error) {
        VX_LOG_ERROR("烘焙失败：%s", error.what());
        return 1;
    }
}
