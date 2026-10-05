// T85：测试模式解析（**纯函数**）单测 —— 启动参数 → F1 面板横幅的输入。
//   - 默认人工测试、无项；
//   - `--auto-test` ⇒ 自动（**优先于**人工项）；
//   - `--manual-test=a;b;c` ⇒ 人工 + 逐项（丢弃空项、可累加、按字节保 UTF-8）；
//   - `--` 前缀识别为开关（**绝不能**当成位置参数路径）。

#include "test_mode.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using vx::IsOptionArgument;
using vx::ParseTestModeFromArguments;
using vx::TestMode;
using vx::TestModeInfo;

}  // namespace

TEST(TestMode, DefaultsToManualWithoutItems) {
    EXPECT_EQ(ParseTestModeFromArguments({}).mode, TestMode::Manual);
    EXPECT_EQ(ParseTestModeFromArguments({ "voxel_game.exe" }).mode, TestMode::Manual);
    EXPECT_TRUE(ParseTestModeFromArguments({ "voxel_game.exe" }).manualItems.empty());
}

TEST(TestMode, AutoFlagSelectsAuto) {
    EXPECT_EQ(ParseTestModeFromArguments({ "voxel_game.exe", "--auto-test" }).mode, TestMode::Auto);
    // 未知参数不影响判定。
    EXPECT_EQ(ParseTestModeFromArguments({ "voxel_game.exe", "--unknown", "--auto-test" }).mode, TestMode::Auto);
}

TEST(TestMode, AutoWinsOverManualItems) {
    const TestModeInfo info = ParseTestModeFromArguments({ "exe", "--manual-test=a;b", "--auto-test" });
    EXPECT_EQ(info.mode, TestMode::Auto);
}

TEST(TestMode, ManualItemsSplitOnSemicolonAndDropEmpties) {
    const TestModeInfo info = ParseTestModeFromArguments({ "exe", "--manual-test=a;b;c" });
    ASSERT_EQ(info.manualItems.size(), 3U);
    EXPECT_EQ(info.manualItems[0], "a");
    EXPECT_EQ(info.manualItems[1], "b");
    EXPECT_EQ(info.manualItems[2], "c");

    // 空项（连续 / 首 / 尾分号）被丢弃。
    const TestModeInfo sparse = ParseTestModeFromArguments({ "exe", "--manual-test=;a;;b;" });
    ASSERT_EQ(sparse.manualItems.size(), 2U);
    EXPECT_EQ(sparse.manualItems[0], "a");
    EXPECT_EQ(sparse.manualItems[1], "b");

    // 空值 ⇒ 无项。
    EXPECT_TRUE(ParseTestModeFromArguments({ "exe", "--manual-test=" }).manualItems.empty());

    // 多次给出 ⇒ 累加（保持出现顺序）。
    const TestModeInfo twice = ParseTestModeFromArguments({ "exe", "--manual-test=a", "--manual-test=b;c" });
    ASSERT_EQ(twice.manualItems.size(), 3U);
    EXPECT_EQ(twice.manualItems[0], "a");
    EXPECT_EQ(twice.manualItems[2], "c");
}

TEST(TestMode, ManualItemsPreserveUtf8Bytes) {
    // 纯函数按字节切分；UTF-8 多字节序列里不含 ';'(0x3B)，故中文原样保留。
    const TestModeInfo info = ParseTestModeFromArguments({ "exe", "--manual-test=脚底贴地;跑动无滑步" });
    ASSERT_EQ(info.manualItems.size(), 2U);
    EXPECT_EQ(info.manualItems[0], "脚底贴地");
    EXPECT_EQ(info.manualItems[1], "跑动无滑步");
}

TEST(TestMode, OptionArgumentRecognisesDoubleDashPrefix) {
    EXPECT_TRUE(IsOptionArgument("--auto-test"));
    EXPECT_TRUE(IsOptionArgument("--manual-test=x"));
    EXPECT_TRUE(IsOptionArgument("--"));
    EXPECT_FALSE(IsOptionArgument("-x"));
    EXPECT_FALSE(IsOptionArgument("shaders"));
    EXPECT_FALSE(IsOptionArgument(""));
}
