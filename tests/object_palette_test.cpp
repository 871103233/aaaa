// 物件选择器纯逻辑单测（V0.5 E3；ADR 0032 决策二）：按 `category` 分组（顺序 = 配置首次出现顺序）、
// 选择状态规整、当前选中类型。见 docs/plans/v0.5.md §1.19。

#include "object_palette.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vx::BuildPalette;
using vx::ClampPaletteState;
using vx::ObjectAssetKind;
using vx::ObjectTable;
using vx::ObjectType;
using vx::PaletteCategory;
using vx::PaletteState;
using vx::SelectedPaletteType;

/// 造一条类型（只填分组关心的字段：id / kind / category）。
ObjectType MakeType(std::string id, std::string category, ObjectAssetKind kind = ObjectAssetKind::Stone) {
    ObjectType type;
    type.id       = std::move(id);
    type.kind     = kind;
    type.category = std::move(category);
    return type;
}

TEST(BuildPalette, GroupsByCategoryInFirstAppearanceOrder) {
    ObjectTable table;
    table.types.push_back(MakeType("tree_a", "vegetation"));
    table.types.push_back(MakeType("rock_a", "rock"));
    table.types.push_back(MakeType("tree_b", "vegetation"));
    table.types.push_back(MakeType("crate", "prop"));

    const std::vector<PaletteCategory> categories = BuildPalette(table);
    ASSERT_EQ(categories.size(), 3U);
    EXPECT_EQ(categories[0].name, "vegetation");  // 顺序 = 首次出现顺序（确定性）
    EXPECT_EQ(categories[1].name, "rock");
    EXPECT_EQ(categories[2].name, "prop");

    ASSERT_EQ(categories[0].types.size(), 2U);
    EXPECT_EQ(categories[0].types[0]->id, "tree_a");  // 类别内 = 类型表顺序
    EXPECT_EQ(categories[0].types[1]->id, "tree_b");
    ASSERT_EQ(categories[1].types.size(), 1U);
    EXPECT_EQ(categories[1].types[0]->id, "rock_a");
}

TEST(BuildPalette, EmptyTableYieldsNoCategory) {
    const ObjectTable                    table;
    const std::vector<PaletteCategory>   categories = BuildPalette(table);
    EXPECT_TRUE(categories.empty());  // 不产生空类别
}

TEST(ClampPaletteState, ClampsOutOfRangeIndices) {
    ObjectTable table;
    table.types.push_back(MakeType("a", "vegetation"));
    table.types.push_back(MakeType("b", "rock"));
    const std::vector<PaletteCategory> categories = BuildPalette(table);

    PaletteState state;
    state.categoryIndex = 99U;
    state.typeIndex     = 99U;
    const PaletteState clamped = ClampPaletteState(state, categories);
    EXPECT_EQ(clamped.categoryIndex, 1U);
    EXPECT_EQ(clamped.typeIndex, 0U);  // 越界钳到末项（该类别只有 1 项 ⇒ 0）

    EXPECT_EQ(ClampPaletteState(PaletteState {}, {}).categoryIndex, 0U);  // 空列表 ⇒ 归零，不越界
}

TEST(SelectedPaletteType, ReturnsCurrentSelectionOrNull) {
    ObjectTable table;
    table.types.push_back(MakeType("tree", "vegetation"));
    table.types.push_back(MakeType("rock", "rock"));
    const std::vector<PaletteCategory> categories = BuildPalette(table);

    PaletteState state;
    state.categoryIndex = 1U;  // rock
    state.typeIndex     = 0U;
    const ObjectType* selected = SelectedPaletteType(state, categories);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id, "rock");

    state.categoryIndex = 5U;  // 越界 ⇒ nullptr（不崩）
    EXPECT_EQ(SelectedPaletteType(state, categories), nullptr);
    state.categoryIndex = 0U;
    state.typeIndex     = 5U;
    EXPECT_EQ(SelectedPaletteType(state, categories), nullptr);
}

}  // namespace
