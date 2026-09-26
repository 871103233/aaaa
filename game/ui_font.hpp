#pragma once

#include <cstddef>
#include <string>

/// ImGui 的 IO 上下文类型：本头文件**不引入 imgui.h**（保持可在无 ImGui 的单测里包含），
/// 只对 `ApplyUiFont` 的参数做前置声明。
struct ImGuiIO;

namespace vx {

/// UI 字体候选（一条优先级表项）。
struct UiFontCandidate {
    const char* path      = nullptr;  ///< 字体文件路径（UTF-8）
    unsigned    fontIndex = 0;        ///< `.ttc` / `.otc` 字体集合内的索引
};

/// 字体解析结果。
struct UiFontSelection {
    bool        cjkAvailable = false;  ///< 是否找到可用的 CJK 字体
    std::string path;                  ///< 用到的字体路径（未找到时为空）
    unsigned    fontIndex    = 0;      ///< 字体集合索引
    bool        bundled      = false;  ///< 是否来自仓库内 `assets/fonts/`
};

/// 纯函数：路径是否以受支持的字体扩展名结尾（`.ttf` / `.otf` / `.ttc` / `.otc`，大小写不敏感）。
[[nodiscard]] inline bool IsSupportedFontExtension(const char* path) noexcept {
    if (path == nullptr) {
        return false;
    }
    const std::string text(path);
    const std::size_t dot = text.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = text.substr(dot);
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
}

/// 系统 CJK 字体优先级表（按顺序取**第一个存在**的文件）。
///
/// 编译期按平台裁剪；路径全部为系统自带字体，仓库不携带任何字体资产（见 T16 的范围约定）。
#if defined(_WIN32)
inline constexpr UiFontCandidate kSystemFontCandidates[] = {
    { "C:\\Windows\\Fonts\\msyh.ttc", 0 },    ///< 微软雅黑（简体，TTC）
    { "C:\\Windows\\Fonts\\msyh.ttf", 0 },    ///< 微软雅黑（旧版单文件）
    { "C:\\Windows\\Fonts\\simhei.ttf", 0 },  ///< 黑体
    { "C:\\Windows\\Fonts\\simsun.ttc", 0 },  ///< 宋体（TTC）
    { "C:\\Windows\\Fonts\\Deng.ttf", 0 },    ///< 等线
    { "C:\\Windows\\Fonts\\msyhbd.ttc", 0 },  ///< 微软雅黑粗体（兜底）
};
#elif defined(__APPLE__)
inline constexpr UiFontCandidate kSystemFontCandidates[] = {
    { "/System/Library/Fonts/PingFang.ttc", 0 },        ///< 苹方（TTC）
    { "/System/Library/Fonts/STHeiti Light.ttc", 0 },   ///< 华文黑体（TTC）
    { "/Library/Fonts/Arial Unicode.ttf", 0 },          ///< Arial Unicode（含 CJK）
};
#else
inline constexpr UiFontCandidate kSystemFontCandidates[] = {
    { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0 },  ///< Noto Sans CJK（TTC）
    { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0 },       ///< Noto Sans CJK（备用路径）
    { "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 0 },  ///< Noto Sans CJK（Debian 布局）
    { "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0 },          ///< 文泉驿微米黑（TTC）
    { "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", 0 }, ///< 文泉驿（备用布局）
};
#endif

/// 系统候选表长度。
inline constexpr std::size_t kSystemFontCandidateCount =
    sizeof(kSystemFontCandidates) / sizeof(kSystemFontCandidates[0]);

/// 纯函数：按顺序返回第一个 `exists()` 为真的候选；全都不存在时 `cjkAvailable = false`。
///
/// `exists` 由调用方注入（真实实现为文件系统查询，单测注入假实现），因此本函数不触碰文件系统、
/// 也不读全局状态。
[[nodiscard]] inline UiFontSelection SelectUiFont(const UiFontCandidate* candidates, std::size_t count,
                                                  bool (*exists)(const char*)) {
    UiFontSelection selection;
    if (candidates == nullptr || exists == nullptr) {
        return selection;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const UiFontCandidate& candidate = candidates[i];
        if (candidate.path == nullptr || !exists(candidate.path)) {
            continue;
        }
        selection.cjkAvailable = true;
        selection.path         = candidate.path;
        selection.fontIndex    = candidate.fontIndex;
        selection.bundled      = false;
        return selection;
    }
    return selection;
}

/// 解析本机可用的 UI 字体：先仓库内 `assets/fonts/`（可缺省，缺失不算错误），
/// 再按平台优先级表取系统 CJK 字体；都没有则返回 `cjkAvailable = false`。
[[nodiscard]] UiFontSelection ResolveUiFont();

/// 在 ImGui 初始化阶段应用字体并返回**是否加载到 CJK 字体**（该返回值决定标签用中文还是英文）。
///
/// 语义：找到候选字体时以约 18 px、简体中文常用字集（`GetGlyphRangesChineseSimplifiedCommon`）
/// 加载，`.ttc` 集合显式传入解析到的集合索引；加载失败或找不到字体时回退 ImGui 默认字体，
/// 并返回 false（此时标签缝会整表切换到纯 ASCII 英文）。
/// 前置条件：`io` 为已创建上下文的 `ImGuiIO`，且尚未开始第一帧。
[[nodiscard]] bool ApplyUiFont(ImGuiIO& io);

}  // namespace vx
