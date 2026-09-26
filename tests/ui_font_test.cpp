// T16：UI 字体解析逻辑的单元测试。
//
// 覆盖三件事：扩展名过滤、按优先级取第一个存在的候选、候选表本身合法。
// 文件系统查询被注入为假实现，因此测试不依赖本机是否装有 CJK 字体，也不读写磁盘。

#include "ui_font.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

namespace {

using vx::IsSupportedFontExtension;
using vx::kSystemFontCandidateCount;
using vx::kSystemFontCandidates;
using vx::SelectUiFont;
using vx::UiFontCandidate;
using vx::UiFontSelection;

/// 注入用的"存在"判定：只把 `g_existingPath` 指向的路径视为存在。
const char* g_existingPath = nullptr;

bool TestExists(const char* path) {
    return g_existingPath != nullptr && path != nullptr && std::strcmp(path, g_existingPath) == 0;
}

}  // namespace

// 只接受受支持的字体扩展名，且大小写不敏感。
TEST(UiFont, SupportedExtensions) {
    EXPECT_TRUE(IsSupportedFontExtension("C:/Windows/Fonts/msyh.ttc"));
    EXPECT_TRUE(IsSupportedFontExtension("C:/Windows/Fonts/msyh.TTC"));
    EXPECT_TRUE(IsSupportedFontExtension("/usr/share/fonts/x/wqy-microhei.ttf"));
    EXPECT_TRUE(IsSupportedFontExtension("/System/Library/Fonts/PingFang.otf"));
    EXPECT_FALSE(IsSupportedFontExtension("C:/Windows/Fonts/readme.txt"));
    EXPECT_FALSE(IsSupportedFontExtension("font_without_extension"));
    EXPECT_FALSE(IsSupportedFontExtension(""));
    EXPECT_FALSE(IsSupportedFontExtension(nullptr));
}

// 按顺序返回第一个存在的候选。
TEST(UiFont, PicksFirstExistingCandidate) {
    const UiFontCandidate candidates[] = {
        { "a.ttc", 0 },
        { "b.ttf", 1 },
        { "c.otf", 0 },
    };

    g_existingPath = nullptr;
    EXPECT_FALSE(SelectUiFont(candidates, 3, &TestExists).cjkAvailable) << "都不存在时应回退到无 CJK 字体";

    g_existingPath = "b.ttf";
    const UiFontSelection second = SelectUiFont(candidates, 3, &TestExists);
    ASSERT_TRUE(second.cjkAvailable);
    EXPECT_EQ(second.path, "b.ttf");
    EXPECT_EQ(second.fontIndex, 1U);
    EXPECT_FALSE(second.bundled);

    // 靠前的候选存在时，优先返回靠前的（优先级由表顺序决定）。
    g_existingPath = "a.ttc";
    const UiFontSelection first = SelectUiFont(candidates, 3, &TestExists);
    ASSERT_TRUE(first.cjkAvailable);
    EXPECT_EQ(first.path, "a.ttc");

    g_existingPath = nullptr;
}

// 空阈值 / 空表 / 空指针等防御路径不得崩溃、不得误判为可用。
TEST(UiFont, DefensiveInputs) {
    g_existingPath = nullptr;
    EXPECT_FALSE(SelectUiFont(nullptr, 3, &TestExists).cjkAvailable);
    EXPECT_FALSE(SelectUiFont(kSystemFontCandidates, 0, &TestExists).cjkAvailable);
    EXPECT_FALSE(SelectUiFont(kSystemFontCandidates, kSystemFontCandidateCount, nullptr).cjkAvailable);

    const UiFontCandidate withNullPath[] = { { nullptr, 0 } };
    EXPECT_FALSE(SelectUiFont(withNullPath, 1, [](const char*) { return true; }).cjkAvailable);
}

// 系统候选表必须非空、路径合法，且按平台裁剪后确实指向 `.ttf` / `.ttc` 之类的字体文件。
TEST(UiFont, SystemCandidateTableIsSane) {
    ASSERT_GT(kSystemFontCandidateCount, 0U);
    for (std::size_t i = 0; i < kSystemFontCandidateCount; ++i) {
        ASSERT_NE(kSystemFontCandidates[i].path, nullptr) << "候选 #" << i << " 路径为空指针";
        EXPECT_TRUE(IsSupportedFontExtension(kSystemFontCandidates[i].path))
            << "候选 #" << i << " 不是受支持的字体扩展名：" << kSystemFontCandidates[i].path;
    }
}
