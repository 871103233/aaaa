// T16：UI 标签缝的单元测试。
//
// 这一组测试是"面板上永远不会出现缺字 `?`"的可执行证据，也是"新增标签不得绕开标签缝"的强制门禁：
//   1. `cjkFontAvailable = false` 时，**每一个**标签都必须是纯 ASCII（逐项断言，不靠肉眼）；
//   2. 中文 / 英文两张表长度与枚举一致且无空项，中文表里确实存在中文（不是误填英文）；
//   3. `UiText` 按标志返回对应语言的表项；
//   4. 两个面板的源码里**不得**把含非 ASCII 的字符串字面量直接交给 ImGui 的标签函数。

#include "ui_text.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using vx::IsAsciiOnly;
using vx::kUiLabelCount;
using vx::kUiLabelsChinese;
using vx::kUiLabelsEnglish;
using vx::UiLabel;
using vx::UiText;

/// 读取整个文本文件（二进制模式，保留原始字节）；失败返回空串。
[[nodiscard]] std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool IsIdentifierByte(unsigned char byte) {
    return std::isalnum(byte) != 0 || byte == static_cast<unsigned char>('_');
}

/// 判断字节是否为空白（仅处理 ASCII 空白，足够覆盖源码排版）。
[[nodiscard]] bool IsSpaceByte(unsigned char byte) {
    return byte == static_cast<unsigned char>(' ') || byte == static_cast<unsigned char>('\t') ||
           byte == static_cast<unsigned char>('\r') || byte == static_cast<unsigned char>('\n');
}

}  // namespace

// 关键约束：无 CJK 字体时的英文回退表必须整表纯 ASCII——这是"不会出现 `?`"的根本保证。
TEST(UiText, EnglishFallbackIsPureAsciiForEveryLabel) {
    for (int index = 0; index < kUiLabelCount; ++index) {
        const char* text = UiText(static_cast<UiLabel>(index), /*cjk=*/false);
        ASSERT_NE(text, nullptr) << "标签 #" << index << " 的英文项为空指针";
        EXPECT_FALSE(std::string(text).empty()) << "标签 #" << index << " 的英文项为空串";
        EXPECT_TRUE(IsAsciiOnly(text)) << "标签 #" << index << " 的英文回退含非 ASCII 字符：" << text;
    }
}

// 两张表必须与枚举等长且无空项：枚举加了标签却漏填表 → 这里立刻失败。
TEST(UiText, BothTablesMatchEnumAndAreComplete) {
    EXPECT_EQ(kUiLabelCount, static_cast<int>(kUiLabelsEnglish.size()));
    EXPECT_EQ(kUiLabelCount, static_cast<int>(kUiLabelsChinese.size()));

    int translatedCount = 0;
    for (int index = 0; index < kUiLabelCount; ++index) {
        const char* english = UiText(static_cast<UiLabel>(index), false);
        const char* chinese = UiText(static_cast<UiLabel>(index), true);
        ASSERT_NE(english, nullptr) << "标签 #" << index << " 的英文项为空指针";
        ASSERT_NE(chinese, nullptr) << "标签 #" << index << " 的中文项为空指针";
        EXPECT_FALSE(std::string(english).empty()) << "标签 #" << index << " 的英文项为空串";
        EXPECT_FALSE(std::string(chinese).empty()) << "标签 #" << index << " 的中文项为空串";

        if (IsAsciiOnly(chinese)) {
            // 纯格式串（如 "%d / 100"）允许中英一致；否则就是误把英文填进了中文表。
            EXPECT_STREQ(english, chinese) << "标签 #" << index << " 的中文项既非中文、又与英文不一致";
        } else {
            ++translatedCount;
        }
    }
    EXPECT_GT(translatedCount, 0) << "中文表里必须确实存在中文条目，防止整列误填成英文";
}

// 缝的分支：同一标签按标志返回不同语言，且都非空。
TEST(UiText, FlagSelectsLanguage) {
    const char* english = UiText(UiLabel::QuitGame, false);
    const char* chinese = UiText(UiLabel::QuitGame, true);
    EXPECT_STREQ(english, kUiLabelsEnglish[static_cast<std::size_t>(UiLabel::QuitGame)]);
    EXPECT_STREQ(chinese, kUiLabelsChinese[static_cast<std::size_t>(UiLabel::QuitGame)]);
    EXPECT_STRNE(english, chinese);

    // 越界索引返回空串（防御性），不得崩溃。
    EXPECT_STREQ(UiText(static_cast<UiLabel>(kUiLabelCount), true), "");
    EXPECT_STREQ(UiText(static_cast<UiLabel>(-1), false), "");
}

// `IsAsciiOnly` 的边界行为。
TEST(UiText, AsciiPredicateBoundaries) {
    EXPECT_TRUE(IsAsciiOnly(nullptr));
    EXPECT_TRUE(IsAsciiOnly(""));
    EXPECT_TRUE(IsAsciiOnly("Frame time: 1.23 ms"));
    EXPECT_FALSE(IsAsciiOnly("帧时间"));
    EXPECT_FALSE(IsAsciiOnly("ok ok 中"));
    EXPECT_FALSE(IsAsciiOnly("°"));
}

// 反绕缝门禁：面板源码里不得把非 ASCII 字符串字面量直接交给 ImGui 的标签函数。
//
// 只要有人写下 `ImGui::TextUnformatted("中文")` 这类写法，本用例立刻失败；正确做法是走 `UiText`。
TEST(UiText, PanelsNeverPassNonAsciiLiteralsToImGui) {
    const char* const relativePaths[] = { "game/debug_overlay.cpp", "game/system_panel.cpp" };

    for (const char* relativePath : relativePaths) {
        const std::filesystem::path path = std::filesystem::path(VOXEL_SOURCE_DIR) / relativePath;
        const std::string           text = ReadWholeFile(path);
        ASSERT_FALSE(text.empty()) << "无法读取面板源码：" << path.string();

        std::size_t position = 0;
        while ((position = text.find("ImGui::", position)) != std::string::npos) {
            std::size_t cursor = position + 7;  // 跳过 "ImGui::"

            const std::size_t identifierBegin = cursor;
            while (cursor < text.size() && IsIdentifierByte(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor == identifierBegin) {
                position = cursor;
                continue;
            }

            while (cursor < text.size() && IsSpaceByte(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor >= text.size() || text[cursor] != '(') {
                position = cursor;
                continue;
            }
            ++cursor;
            while (cursor < text.size() && IsSpaceByte(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor + 3U <= text.size() && text.compare(cursor, 3, "u8\"") == 0) {
                cursor += 2;  // 跳到引号
            }

            if (cursor < text.size() && text[cursor] == '"') {
                ++cursor;
                std::string literal;
                while (cursor < text.size() && text[cursor] != '"') {
                    if (text[cursor] == '\\' && cursor + 1U < text.size()) {
                        literal.push_back(text[cursor]);
                        ++cursor;
                    }
                    literal.push_back(text[cursor]);
                    ++cursor;
                }
                EXPECT_TRUE(IsAsciiOnly(literal.c_str()))
                    << relativePath << " 里 ImGui 调用的字符串字面量不是纯 ASCII（应改用 UiText）：" << literal;
            }
            position = cursor;
        }
    }
}
