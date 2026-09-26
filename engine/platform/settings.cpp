#include "platform/settings.hpp"

#include "core/log.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <toml++/toml.hpp>

namespace vx {
namespace {

/// 设置文件名（与 `SystemSettingsPath()` 的目录拼成完整路径）。
constexpr const char* kSettingsFileName = "settings.toml";

/// SDL 应用标识：决定每用户可写目录（Windows 上为 `%APPDATA%\<org>\<app>\`）。
constexpr const char* kPrefOrg = "voxel-engine";
constexpr const char* kPrefApp = "voxel_game";

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::int64_t ReadInt(const toml::table& document, const std::filesystem::path& path, const char* field) {
    const std::optional<std::int64_t> value = document[field].value<std::int64_t>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是整数");
    }
    return *value;
}

[[nodiscard]] std::string ReadString(const toml::table& document, const std::filesystem::path& path,
                                     const char* field) {
    const std::optional<std::string> value = document[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是字符串");
    }
    return *value;
}

[[nodiscard]] DisplayMode ParseDisplayMode(const std::string& text, const std::filesystem::path& path) {
    if (text == "windowed") {
        return DisplayMode::Windowed;
    }
    if (text == "fullscreen") {
        return DisplayMode::Fullscreen;
    }
    throw std::runtime_error(Describe(path, "display_mode") + "取值必须是 \"windowed\" 或 \"fullscreen\"，实际为 \"" +
                             text + "\"");
}

[[nodiscard]] const char* DisplayModeName(DisplayMode mode) noexcept {
    return (mode == DisplayMode::Fullscreen) ? "fullscreen" : "windowed";
}

}  // namespace

SystemSettings LoadSystemSettings(const std::filesystem::path& path) {
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        return SystemSettings {};  // 文件缺失 → 默认值，不报错
    }

    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("无法解析设置文件 " + path.string() + ": " + error.what());
    }

    const std::int64_t schemaVersion = ReadInt(document, path, "schema_version");
    if (schemaVersion != static_cast<std::int64_t>(kSettingsSchemaVersion)) {
        throw std::runtime_error(Describe(path, "schema_version") + "不匹配（期望 " +
                                 std::to_string(kSettingsSchemaVersion) + "，实际 " +
                                 std::to_string(schemaVersion) + "）");
    }

    const std::int64_t width  = ReadInt(document, path, "window_width");
    const std::int64_t height = ReadInt(document, path, "window_height");
    if (width <= 0 || height <= 0) {
        throw std::runtime_error(Describe(path, "window_width/window_height") + "必须为正整数");
    }

    SystemSettings settings;
    settings.displayMode  = ParseDisplayMode(ReadString(document, path, "display_mode"), path);
    settings.windowWidth  = static_cast<int>(width);
    settings.windowHeight = static_cast<int>(height);
    // 音量类型正确但越界时钳制（见头文件契约），不因用户手改出界而拒绝启动。
    settings.masterVolume = ClampMasterVolume(static_cast<int>(ReadInt(document, path, "master_volume")));
    // 帧率上限（T17）：**可选字段**——旧版设置文件没有它，缺失即保持哨兵（未设置 → 载入后取刷新率）；
    // 存在但不是整数则报错（与其它字段同一错误策略）。越界数值由 `ResolveFrameRateCap` 后续钳制。
    if (document.contains("frame_rate_cap")) {
        settings.frameRateCap = static_cast<int>(ReadInt(document, path, "frame_rate_cap"));
    }
    return settings;
}

void SaveSystemSettings(const std::filesystem::path& path, const SystemSettings& settings) {
    toml::table document;
    (void)document.insert_or_assign("schema_version", static_cast<std::int64_t>(kSettingsSchemaVersion));
    (void)document.insert_or_assign("display_mode", std::string(DisplayModeName(settings.displayMode)));
    (void)document.insert_or_assign("window_width", static_cast<std::int64_t>(settings.windowWidth));
    (void)document.insert_or_assign("window_height", static_cast<std::int64_t>(settings.windowHeight));
    (void)document.insert_or_assign("master_volume",
                                    static_cast<std::int64_t>(ClampMasterVolume(settings.masterVolume)));
    // 帧率上限始终写**解析后的绝对值**（哨兵 0 在启动时已由 `ResolveFrameRateCap` 落成实际 Hz）。
    (void)document.insert_or_assign("frame_rate_cap", static_cast<std::int64_t>(settings.frameRateCap));

    // binary 模式：禁止运行库做 CRLF 转换，保证落盘为纯 LF（仓库行尾约定）。
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("无法写入设置文件 " + path.string());
    }
    out << "# 体素引擎系统设置（T15 自动生成；删除本文件将以默认值重建）\n";
    out << document;
    out.flush();
    if (!out) {
        throw std::runtime_error("写入设置文件失败 " + path.string());
    }
}

std::filesystem::path SystemSettingsPath() {
    char* directory = SDL_GetPrefPath(kPrefOrg, kPrefApp);
    if (directory == nullptr) {
        VX_LOG_WARN("SDL_GetPrefPath 失败（%s），设置回退到当前目录下的 %s", SDL_GetError(), kSettingsFileName);
        return std::filesystem::path(kSettingsFileName);
    }
    const std::filesystem::path directoryPath(directory);
    SDL_free(directory);
    return directoryPath / kSettingsFileName;
}

void ApplyMasterVolumeGain(int volume) noexcept {
    const int   clamped = ClampMasterVolume(volume);
    const float gain    = static_cast<float>(clamped) / static_cast<float>(kMasterVolumeMax);
    // 唯一的音频增益路径。当前无音源 / 混音器，故只记录设置与折算增益（不会发声）。
    VX_LOG_INFO("主音量增益入口：设置 %d / %d → 线性增益 %.2f（音频系统未实现，当前无声音输出）", clamped,
                kMasterVolumeMax, static_cast<double>(gain));
}

}  // namespace vx
