#include "ui_font.hpp"

#include "core/log.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace vx {
namespace {

/// 解析到的 CJK 字体加载尺寸（像素）。18 px 在 1080p 下清晰可读，是常见游戏 UI 正文尺寸。
constexpr float kUiFontSizePixels = 18.0F;

/// 文件是否存在；用 `error_code` 重载，避免在启动路径上抛异常。
[[nodiscard]] bool FileExists(const char* path) noexcept {
    std::error_code error;
    const bool      found = std::filesystem::exists(std::filesystem::path(path), error);
    return found && !error;
}

/// 仓库内字体目录候选（按顺序）：可执行文件旁 / 其父 / 祖父目录，以及编译期源目录。
[[nodiscard]] std::vector<std::filesystem::path> BundledFontDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* base = SDL_GetBasePath()) {
        const std::filesystem::path basePath(base);
        dirs.push_back(basePath / "assets" / "fonts");
        dirs.push_back(basePath.parent_path() / "assets" / "fonts");
        dirs.push_back(basePath.parent_path().parent_path() / "assets" / "fonts");
    }
#ifdef VOXEL_SOURCE_DIR
    dirs.push_back(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "fonts");
#endif
    return dirs;
}

/// 在仓库内字体目录里找第一个受支持的字体文件（文件名升序，保证跨机器确定）。
///
/// 目录不存在或为空都返回空串——这是**默认状态且不算错误**（仓库默认不携带字体资产）。
[[nodiscard]] std::string FindBundledFont() {
    for (const std::filesystem::path& dir : BundledFontDirs()) {
        std::error_code                      error;
        const std::filesystem::file_status   status = std::filesystem::status(dir, error);
        if (error || !std::filesystem::is_directory(status)) {
            continue;
        }

        std::vector<std::string> names;
        std::filesystem::directory_iterator iterator(dir, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            const std::string name = iterator->path().filename().string();
            if (IsSupportedFontExtension(name.c_str())) {
                names.push_back(name);
            }
            iterator.increment(error);
        }
        if (!names.empty()) {
            std::sort(names.begin(), names.end());
            return (dir / names.front()).string();
        }
    }
    return {};
}

}  // namespace

UiFontSelection ResolveUiFont() {
    const std::string bundled = FindBundledFont();
    if (!bundled.empty()) {
        UiFontSelection selection;
        selection.cjkAvailable = true;
        selection.path         = bundled;
        selection.fontIndex    = 0;
        selection.bundled      = true;
        return selection;
    }
    return SelectUiFont(kSystemFontCandidates, kSystemFontCandidateCount, &FileExists);
}

bool ApplyUiFont(ImGuiIO& io) {
    const UiFontSelection selection = ResolveUiFont();

    if (selection.cjkAvailable) {
        // `.ttc` 是字体集合，必须给集合内的索引；`.ttf` 用默认索引 0。
        ImFontConfig config;
        config.FontNo     = selection.fontIndex;
        config.OversampleH = 2;
        config.OversampleV = 1;

        ImFont* font = io.Fonts->AddFontFromFileTTF(selection.path.c_str(), kUiFontSizePixels, &config,
                                                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font != nullptr) {
            io.FontDefault = font;
            VX_LOG_INFO("UI 字体：已加载%s CJK 字体 %s（集合索引 %u，%.0f px，简体中文常用字集）"
                        "—— 面板使用中文标签",
                        selection.bundled ? "仓库内" : "系统", selection.path.c_str(), selection.fontIndex,
                        static_cast<double>(kUiFontSizePixels));
            return true;
        }
        VX_LOG_WARN("UI 字体：候选 %s 加载失败，回退 ImGui 默认字体", selection.path.c_str());
    } else {
        VX_LOG_INFO("UI 字体：未找到任何 CJK 字体（仓库内 assets/fonts 与系统字体均不可用）"
                    "—— 面板回退纯英文标签，不会出现缺字 `?`");
    }

    io.FontDefault = nullptr;
    io.Fonts->AddFontDefault();
    return false;
}

}  // namespace vx
