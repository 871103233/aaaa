#include "platform/settings.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace {

using vx::ClampExposure;
using vx::ClampFrameRateCap;
using vx::ClampMasterVolume;
using vx::ClampMsaaSampleCount;
using vx::DisplayMode;
using vx::IsResolutionEditable;
using vx::LoadSystemSettings;
using vx::ResolveFrameRateCap;
using vx::SaveSystemSettings;
using vx::SystemSettings;

/// 每个用例用独立临时文件，避免相互干扰与残留。
[[nodiscard]] std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void WriteText(const std::filesystem::path& path, const char* text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out << text;
}

void RemoveQuietly(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace

// T15 ①：写入 → 读取，字段逐一相等。
TEST(SystemSettings, RoundTripPreservesValues) {
    const std::filesystem::path path = TempPath("vx_settings_roundtrip.toml");

    SystemSettings written;
    written.displayMode  = DisplayMode::Fullscreen;
    written.windowWidth  = 1920;
    written.windowHeight = 1080;
    written.masterVolume = 55;
    written.frameRateCap = 90;  // T17：新增字段必须一并往返
    SaveSystemSettings(path, written);

    const SystemSettings read = LoadSystemSettings(path);
    EXPECT_EQ(read.displayMode, DisplayMode::Fullscreen);
    EXPECT_EQ(read.windowWidth, 1920);
    EXPECT_EQ(read.windowHeight, 1080);
    EXPECT_EQ(read.masterVolume, 55);
    EXPECT_EQ(read.frameRateCap, 90);

    RemoveQuietly(path);
}

// T15 ②：文件缺失 → 默认值，且**不报错**。
TEST(SystemSettings, MissingFileUsesDefaultsWithoutError) {
    const std::filesystem::path path = TempPath("vx_settings_does_not_exist.toml");
    RemoveQuietly(path);

    const SystemSettings settings = LoadSystemSettings(path);
    EXPECT_EQ(settings.displayMode, DisplayMode::Windowed);
    EXPECT_EQ(settings.windowWidth, 1280);
    EXPECT_EQ(settings.windowHeight, 720);
    EXPECT_EQ(settings.masterVolume, vx::kMasterVolumeDefault);
    EXPECT_EQ(settings.frameRateCap, vx::kFrameRateCapUnset);  // T17：未设置 → 载入后取刷新率
}

// T15 ③：文件存在但语法非法 → 明确报错（不静默回退）。
TEST(SystemSettings, MalformedTomlThrows) {
    const std::filesystem::path path = TempPath("vx_settings_malformed.toml");
    WriteText(path, "this is not = = valid toml\n");

    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error);

    RemoveQuietly(path);
}

// T15 ③（schema 不匹配）：必须报错，而不是接受未知版本。
TEST(SystemSettings, SchemaMismatchThrows) {
    const std::filesystem::path path = TempPath("vx_settings_schema.toml");
    WriteText(path,
              "schema_version = 99\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n");

    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error);

    RemoveQuietly(path);
}

// T15 ③（字段类型错误 / 取值非法）：必须报错。
TEST(SystemSettings, InvalidFieldThrows) {
    const std::filesystem::path path = TempPath("vx_settings_invalid_field.toml");

    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"fullscreen\"\n"
              "window_width = 0\n"
              "window_height = 720\n"
              "master_volume = 80\n");
    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error) << "非正的窗口宽度应报错";

    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = \"loud\"\n");
    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error) << "音量为字符串应报错";

    RemoveQuietly(path);
}

// T15 ③（音量越界）：类型正确但越界 → 按纯函数钳制（不因用户手改出界而拒绝启动）。
TEST(SystemSettings, OutOfRangeVolumeIsClampedOnLoad) {
    const std::filesystem::path path = TempPath("vx_settings_volume_range.toml");
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 500\n");

    const SystemSettings settings = LoadSystemSettings(path);
    EXPECT_EQ(settings.masterVolume, vx::kMasterVolumeMax);

    RemoveQuietly(path);
}

// T15 ④（纯函数）：音量钳制到 [0, 100]。
TEST(SystemSettings, ClampMasterVolumePureFunction) {
    EXPECT_EQ(ClampMasterVolume(-1), 0);
    EXPECT_EQ(ClampMasterVolume(0), 0);
    EXPECT_EQ(ClampMasterVolume(37), 37);
    EXPECT_EQ(ClampMasterVolume(100), 100);
    EXPECT_EQ(ClampMasterVolume(101), 100);
}

// T15 ④（纯函数）：分辨率控件仅在窗口模式下可用。
TEST(SystemSettings, ResolutionEditableOnlyInWindowedMode) {
    EXPECT_TRUE(IsResolutionEditable(DisplayMode::Windowed));
    EXPECT_FALSE(IsResolutionEditable(DisplayMode::Fullscreen));
}

// T17 ①：`frame_rate_cap` 为**可选字段**——旧版设置文件（无该字段）仍能载入，取值保持哨兵。
TEST(SystemSettings, FrameRateCapFieldIsOptional) {
    const std::filesystem::path path = TempPath("vx_settings_no_frame_cap.toml");
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n");

    const SystemSettings settings = LoadSystemSettings(path);
    EXPECT_EQ(settings.frameRateCap, vx::kFrameRateCapUnset);

    RemoveQuietly(path);
}

// T17 ①：`frame_rate_cap` 存在但类型错误 → 明确报错（与其它字段同一错误策略）。
TEST(SystemSettings, FrameRateCapWrongTypeThrows) {
    const std::filesystem::path path = TempPath("vx_settings_frame_cap_type.toml");
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n"
              "frame_rate_cap = \"fast\"\n");

    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error);

    RemoveQuietly(path);
}

// T17 ②（纯函数）：帧率上限钳制到 [60, 刷新率]；刷新率未知 → 回退 60。
TEST(SystemSettings, ClampFrameRateCapPureFunction) {
    // 高于刷新率 → 钳到刷新率
    EXPECT_EQ(ClampFrameRateCap(200, 144), 144);
    EXPECT_EQ(ClampFrameRateCap(145, 144), 144);
    // 正常区间内保持不变
    EXPECT_EQ(ClampFrameRateCap(144, 144), 144);
    EXPECT_EQ(ClampFrameRateCap(90, 144), 90);
    EXPECT_EQ(ClampFrameRateCap(60, 144), 60);
    // 低于下限 → 钳到 60
    EXPECT_EQ(ClampFrameRateCap(59, 144), 60);
    EXPECT_EQ(ClampFrameRateCap(1, 144), 60);
    EXPECT_EQ(ClampFrameRateCap(0, 144), 60);
    EXPECT_EQ(ClampFrameRateCap(-30, 144), 60);
    // 刷新率未知 / 非正 → 回退 kFallbackRefreshRate（60），区间退化为单点 60
    EXPECT_EQ(ClampFrameRateCap(200, 0), vx::kFallbackRefreshRate);
    EXPECT_EQ(ClampFrameRateCap(90, 0), vx::kFallbackRefreshRate);
    EXPECT_EQ(ClampFrameRateCap(-1, -5), vx::kFallbackRefreshRate);
    // 刷新率本身低于 60：上界抬到 60，区间非空（下限优先）
    EXPECT_EQ(ClampFrameRateCap(120, 30), 60);
}

// T17 ②（纯函数）：存储值解析——哨兵 0 取刷新率，越界值再钳制（换显示器后不会留非法值）。
TEST(SystemSettings, ResolveFrameRateCapClampsStoredValue) {
    EXPECT_EQ(ResolveFrameRateCap(vx::kFrameRateCapUnset, 144), 144);  // 未设置 → 刷新率
    EXPECT_EQ(ResolveFrameRateCap(vx::kFrameRateCapUnset, 0), vx::kFallbackRefreshRate);
    EXPECT_EQ(ResolveFrameRateCap(240, 144), 144);  // 换到低刷新率显示器 → 重新钳制
    EXPECT_EQ(ResolveFrameRateCap(30, 144), 60);
    EXPECT_EQ(ResolveFrameRateCap(90, 144), 90);  // 合法值保持
}

// T20 ①（纯函数）：曝光钳制到 [0.1, 8.0]。
TEST(SystemSettings, ClampExposurePureFunction) {
    EXPECT_FLOAT_EQ(ClampExposure(0.0F), vx::kExposureMin);
    EXPECT_FLOAT_EQ(ClampExposure(-3.0F), vx::kExposureMin);
    EXPECT_FLOAT_EQ(ClampExposure(vx::kExposureMin), vx::kExposureMin);
    EXPECT_FLOAT_EQ(ClampExposure(1.0F), 1.0F);
    EXPECT_FLOAT_EQ(ClampExposure(2.5F), 2.5F);
    EXPECT_FLOAT_EQ(ClampExposure(vx::kExposureMax), vx::kExposureMax);
    EXPECT_FLOAT_EQ(ClampExposure(100.0F), vx::kExposureMax);
}

// T20 ②：曝光——越界载入钳制、字段缺失用默认（旧设置文件兼容）、写入后往返一致。
TEST(SystemSettings, ExposureClampedOnLoadAndRoundTrips) {
    const std::filesystem::path path = TempPath("vx_settings_exposure.toml");

    // 越界 → 载入时钳制到上界（不因用户手改出界而拒绝启动）。
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n"
              "exposure = 20.0\n");
    EXPECT_FLOAT_EQ(LoadSystemSettings(path).exposure, vx::kExposureMax);

    // 字段缺失 → 默认值（旧版设置文件仍能载入）。
    RemoveQuietly(path);
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n");
    EXPECT_FLOAT_EQ(LoadSystemSettings(path).exposure, vx::kExposureDefault);

    // 往返：写 2.5 → 读 2.5。
    SystemSettings written;
    written.exposure = 2.5F;
    SaveSystemSettings(path, written);
    EXPECT_FLOAT_EQ(LoadSystemSettings(path).exposure, 2.5F);

    RemoveQuietly(path);
}

// T23 ①（纯函数）：MSAA 档位钳制到最近的合法档 {1, 2, 4, 8}；**距离相等时向上取**。
// 规则在头文件 `ClampMsaaSampleCount` 的注释里写死，本用例逐点钉死：
//   3 距 2 与 4 各 1 ⇒ 取 4；6 距 4 与 8 各 2 ⇒ 取 8；5 距 4 为 1（更近）⇒ 取 4；7 距 8 为 1 ⇒ 取 8。
TEST(SystemSettings, ClampMsaaSampleCountPureFunction) {
    // 合法档保持原值。
    EXPECT_EQ(ClampMsaaSampleCount(1), 1);
    EXPECT_EQ(ClampMsaaSampleCount(2), 2);
    EXPECT_EQ(ClampMsaaSampleCount(4), 4);
    EXPECT_EQ(ClampMsaaSampleCount(8), 8);
    // 档位之间：取最近；距离相等 → 向上取。
    EXPECT_EQ(ClampMsaaSampleCount(3), 4) << "3 距 2 与 4 相等 → 向上取 4";
    EXPECT_EQ(ClampMsaaSampleCount(5), 4) << "5 距 4 更近 → 取 4";
    EXPECT_EQ(ClampMsaaSampleCount(6), 8) << "6 距 4 与 8 相等 → 向上取 8";
    EXPECT_EQ(ClampMsaaSampleCount(7), 8) << "7 距 8 更近 → 取 8";
    // 越界钳制到端点。
    EXPECT_EQ(ClampMsaaSampleCount(0), vx::kMsaaSampleCountMin);
    EXPECT_EQ(ClampMsaaSampleCount(-5), vx::kMsaaSampleCountMin);
    EXPECT_EQ(ClampMsaaSampleCount(9), vx::kMsaaSampleCountMax);
    EXPECT_EQ(ClampMsaaSampleCount(64), vx::kMsaaSampleCountMax);
}

// T23 ②：`msaa_samples` 为**可选字段**——缺失取默认 4；类型错误报错；越界载入时钳制；写入后往返一致。
TEST(SystemSettings, MsaaSamplesOptionalClampedAndRoundTrips) {
    const std::filesystem::path path = TempPath("vx_settings_msaa.toml");

    // 字段缺失 → 默认 4（旧版设置文件仍能载入）。
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n");
    EXPECT_EQ(LoadSystemSettings(path).msaaSamples, vx::kMsaaSampleCountDefault);

    // 存在且合法 → 原样读入（1 = 关闭 MSAA 的合法档）。
    RemoveQuietly(path);
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n"
              "msaa_samples = 1\n");
    EXPECT_EQ(LoadSystemSettings(path).msaaSamples, 1);

    // 越界（3）→ 载入时钳制到最近的合法档 4（不因用户手改出界而拒绝启动）。
    RemoveQuietly(path);
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n"
              "msaa_samples = 3\n");
    EXPECT_EQ(LoadSystemSettings(path).msaaSamples, 4);

    // 类型错误 → 明确报错（与其它字段同一错误策略）。
    RemoveQuietly(path);
    WriteText(path,
              "schema_version = 1\n"
              "display_mode = \"windowed\"\n"
              "window_width = 1280\n"
              "window_height = 720\n"
              "master_volume = 80\n"
              "msaa_samples = \"high\"\n");
    EXPECT_THROW((void)LoadSystemSettings(path), std::runtime_error);

    // 往返：写 8 → 读 8；写越界值 5 → 落盘钳制为 4、读回 4。
    RemoveQuietly(path);
    SystemSettings written;
    written.msaaSamples = 8;
    SaveSystemSettings(path, written);
    EXPECT_EQ(LoadSystemSettings(path).msaaSamples, 8);

    written.msaaSamples = 5;
    SaveSystemSettings(path, written);
    EXPECT_EQ(LoadSystemSettings(path).msaaSamples, 4);

    RemoveQuietly(path);
}

