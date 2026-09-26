#pragma once

#include <cstdint>
#include <filesystem>

namespace vx {

/// 显示模式（T15 系统面板：窗口 / 全屏）。
enum class DisplayMode : std::uint8_t {
    Windowed   = 0,  ///< 窗口模式：分辨率可调，客户区尺寸由设置决定
    Fullscreen = 1,  ///< 桌面无边框全屏：分辨率由显示器决定
};

/// 帧率上限（T17）的取值范围与存储约定（见 `docs/ui-inventory.md` §3）。
///
/// **存储表示**：绝对帧率（Hz）；`kFrameRateCapUnset`（0）是**哨兵**，表示"未设置"，
/// 载入后解析为当前显示器刷新率。写盘时始终写解析后的绝对值。
inline constexpr int kFrameRateCapMin     = 60;  ///< 滑块下限（Hz）
inline constexpr int kFrameRateCapUnset   = 0;   ///< 存储哨兵：未设置 → 取当前显示器刷新率
inline constexpr int kFallbackRefreshRate = 60;  ///< 刷新率未知 / 非正时的回退刷新率（Hz）

/// 系统设置（T15 / T17 / T20 / T23）：显示模式、窗口分辨率、主音量、帧率上限、曝光、MSAA 档位。
/// 随程序退出落盘、下次启动读回。
struct SystemSettings {
    DisplayMode displayMode  = DisplayMode::Windowed;          ///< 显示模式
    int         windowWidth  = 1280;                           ///< 窗口模式下的客户区宽度（像素）
    int         windowHeight = 720;                            ///< 窗口模式下的客户区高度（像素）
    int         masterVolume = 80;                             ///< 主音量（0–100）
    int         frameRateCap = kFrameRateCapUnset;             ///< 帧率上限（Hz）；哨兵 0 = 取刷新率
    float       exposure     = 1.0F;                           ///< HDR 色调映射曝光（T20 / ADR 0010）
    int         msaaSamples  = 4;                              ///< MSAA 档位（1 / 2 / 4 / 8；T23 / ADR 0010 P3）
};

/// 设置文件格式版本；不匹配即报错（不做静默迁移）。
inline constexpr int kSettingsSchemaVersion = 1;

/// 主音量取值范围与默认值（见 `docs/ui-inventory.md` §3）。
inline constexpr int kMasterVolumeMin     = 0;
inline constexpr int kMasterVolumeMax     = 100;
inline constexpr int kMasterVolumeDefault = 80;

/// 曝光取值范围与默认值（T20 / ADR 0010 P0：色调映射的参数进配置，改值不需重编 Shader）。
inline constexpr float kExposureMin     = 0.1F;
inline constexpr float kExposureMax     = 8.0F;
inline constexpr float kExposureDefault = 1.0F;

/// MSAA 档位（T23 / ADR 0010 P3）：**只允许 {1, 2, 4, 8}**，默认 4×（1 = 关闭）。
///
/// 口径与 `exposure` 完全一致：`msaa_samples` 是**可选字段**（缺失取默认，旧版设置文件仍能载入），
/// 类型错误报错，数值越界由 `ClampMsaaSampleCount` 钳制到**最近的合法档**。
inline constexpr int kMsaaSampleCountMin     = 1;
inline constexpr int kMsaaSampleCountMax     = 8;
inline constexpr int kMsaaSampleCountDefault = 4;

/// 纯函数：把 MSAA 档位钳制到**最近的合法档** `{1, 2, 4, 8}`（越界即钳制，不报错）。
///
/// 规则（由 `tests/system_settings_test.cpp` 钉死）：取距离最近的合法档；**距离相等时向上取**
/// （例：3 距 2 与 4 各 1 ⇒ 取 4；6 距 4 与 8 各 2 ⇒ 取 8）。低于 1 → 1，高于 8 → 8。
[[nodiscard]] inline int ClampMsaaSampleCount(int samples) noexcept {
    if (samples <= 1) {
        return 1;
    }
    constexpr int kTiers[] = { 1, 2, 4, 8 };
    int           best     = kTiers[0];
    int           bestGap  = samples - kTiers[0];
    for (const int tier : kTiers) {
        const int gap = (samples > tier) ? (samples - tier) : (tier - samples);
        // `<=` 使距离相等时**后出现的更大档位**胜出（即向上取）。
        if (gap <= bestGap) {
            bestGap = gap;
            best    = tier;
        }
    }
    return best;
}

/// 纯函数：把曝光钳制到 `[kExposureMin, kExposureMax]`（越界即钳制，不报错）。
[[nodiscard]] inline float ClampExposure(float exposure) noexcept {
    if (exposure < kExposureMin) {
        return kExposureMin;
    }
    if (exposure > kExposureMax) {
        return kExposureMax;
    }
    return exposure;
}

/// 纯函数：把音量钳制到 `[0, 100]`。
[[nodiscard]] inline int ClampMasterVolume(int volume) noexcept {
    if (volume < kMasterVolumeMin) {
        return kMasterVolumeMin;
    }
    if (volume > kMasterVolumeMax) {
        return kMasterVolumeMax;
    }
    return volume;
}

/// 纯函数：分辨率控件是否可用。**仅窗口模式**下可用；全屏时由显示器决定，控件置灰。
[[nodiscard]] inline bool IsResolutionEditable(DisplayMode mode) noexcept {
    return mode == DisplayMode::Windowed;
}

/// 纯函数：把显示器刷新率折算为可用上界；未知 / 非正时返回 `kFallbackRefreshRate`。
///
/// 这是"零刷新率 / 取不到刷新率"的唯一回退点（见 `docs/ui-inventory.md` §2.1「帧数上限」行的限制列）。
[[nodiscard]] inline int EffectiveRefreshRate(int refreshRate) noexcept {
    return (refreshRate > 0) ? refreshRate : kFallbackRefreshRate;
}

/// 纯函数：把帧率上限钳制到 `[kFrameRateCapMin, 刷新率]`。
///
/// 规则（由 `tests/frame_limiter_test.cpp` / `tests/system_settings_test.cpp` 钉死）：
///   - 高于显示器刷新率 → 钳到刷新率；
///   - 低于 `kFrameRateCapMin`（60） → 钳到 60；
///   - 刷新率未知 / 非正 → 上界取 `kFallbackRefreshRate`（60）；
///   - 刷新率本身低于 60 时把上界抬到 60，保证区间非空（下限优先）。
[[nodiscard]] inline int ClampFrameRateCap(int frameRate, int refreshRate) noexcept {
    const int effective = EffectiveRefreshRate(refreshRate);
    const int upper     = (effective > kFrameRateCapMin) ? effective : kFrameRateCapMin;
    if (frameRate < kFrameRateCapMin) {
        return kFrameRateCapMin;
    }
    if (frameRate > upper) {
        return upper;
    }
    return frameRate;
}

/// 纯函数：把**存储值**解析为可用的帧率上限。
///
/// `kFrameRateCapUnset`（0）及以下表示"未设置"，解析为当前显示器刷新率（`EffectiveRefreshRate`）；
/// 随后一律经 `ClampFrameRateCap` 钳制。因此**换显示器 / 改刷新率后启动时会被重新钳制**，
/// 不会留下超过新刷新率的非法值。
[[nodiscard]] inline int ResolveFrameRateCap(int stored, int refreshRate) noexcept {
    const int requested = (stored > kFrameRateCapUnset) ? stored : EffectiveRefreshRate(refreshRate);
    return ClampFrameRateCap(requested, refreshRate);
}

/// 从 TOML 读取系统设置。
///
/// 语义（由 `tests/system_settings_test.cpp` 钉死）：
///   - **文件不存在**：返回默认值，**不报错**；
///   - 文件存在但非法（语法错误 / schema 不匹配 / 字段缺失或类型错误 / 取值非法）：抛 `std::runtime_error`；
///   - `master_volume` 类型正确但越界：钳制到 `[0, 100]`（纯函数 `ClampMasterVolume`）。
///   - `frame_rate_cap`（T17）：**可选字段**——缺失按 `kFrameRateCapUnset`（取刷新率）处理，
///     以保证旧版设置文件仍能载入；存在但类型错误则报错；越界由 `ResolveFrameRateCap` 后续钳制。
///   - `exposure`（T20）：**可选字段**——缺失按 `kExposureDefault` 处理（旧版设置文件仍能载入）；
///     存在但类型错误则报错；越界由 `ClampExposure` 钳制到 `[kExposureMin, kExposureMax]`。
///   - `msaa_samples`（T23）：**可选字段**——缺失按 `kMsaaSampleCountDefault` 处理（旧版设置文件仍能载入）；
///     存在但类型错误则报错；越界由 `ClampMsaaSampleCount` 钳制到最近的合法档 `{1, 2, 4, 8}`。
[[nodiscard]] SystemSettings LoadSystemSettings(const std::filesystem::path& path);

/// 把设置写为 TOML（UTF-8、LF）。前置条件：父目录已存在，否则抛 `std::runtime_error`。
void SaveSystemSettings(const std::filesystem::path& path, const SystemSettings& settings);

/// 本机设置文件的完整路径（`SDL_GetPrefPath` 给出的每用户可写目录 + `settings.toml`）。
///
/// 前置条件：SDL 已完成初始化（即 `Window` 已构造）。SDL 取不到目录时回退到当前工作目录下的
/// `settings.toml`，并记一条警告，绝不因为设置路径而中断启动。
[[nodiscard]] std::filesystem::path SystemSettingsPath();

/// **音频增益入口（唯一）**：把 0–100 的主音量设置应用到音频增益路径。
///
/// 当前限制：工程尚无音源与混音子系统（`engine/audio/` 未创建），本函数只把设置折算为线性增益并记录，
/// **不发出任何声音**；待音频系统落地后在此接入混音器即可，调用方无需改动。
void ApplyMasterVolumeGain(int volume) noexcept;

}  // namespace vx
