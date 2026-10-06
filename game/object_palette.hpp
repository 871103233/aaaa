#pragma once

#include "object/object_layer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace vx {

/// 选择器里的一个**仓库（类别）**：名称 + 该类别下的类型（指针指向 `ObjectTable::types`，生命周期随表）。
struct PaletteCategory {
    std::string                    name;
    std::vector<const ObjectType*> types;
};

/// 按 `category` 把类型表分组（`F2` 选择器的**一级列表**）。
///
/// **确定性**（红线 7）：类别顺序 = 类型表中**首次出现**顺序；类别内 = 类型表顺序。只统计实际存在的类型
/// （不出现空类别）。`category` 由配置显式给出（缺省 `misc`），**不按文件名 / 形态推断**（ADR 0032 决策二）。
[[nodiscard]] inline std::vector<PaletteCategory> BuildPalette(const ObjectTable& table) {
    std::vector<PaletteCategory> categories;
    for (const ObjectType& type : table.types) {
        auto found = categories.end();
        for (auto it = categories.begin(); it != categories.end(); ++it) {
            if (it->name == type.category) {
                found = it;
                break;
            }
        }
        if (found == categories.end()) {
            PaletteCategory category;
            category.name = type.category;
            category.types.push_back(&type);
            categories.push_back(std::move(category));
        } else {
            found->types.push_back(&type);
        }
    }
    return categories;
}

/// 选择器状态（纯数据；面板与键盘导航共用的**唯一**状态）。
struct PaletteState {
    std::size_t categoryIndex = 0;  ///< 一级：当前仓库
    std::size_t typeIndex     = 0;  ///< 二级：当前模型（相对当前仓库）
};

/// 把状态规整到合法范围（**纯函数**）：类别越界钳到末项；某类别为空 ⇒ 该类别不应存在；
/// 二级越界钳到末项。空列表 ⇒ 归零。
[[nodiscard]] inline PaletteState ClampPaletteState(PaletteState state,
                                                    const std::vector<PaletteCategory>& categories) {
    if (categories.empty()) {
        return PaletteState {};
    }
    if (state.categoryIndex >= categories.size()) {
        state.categoryIndex = categories.size() - 1U;
    }
    const std::size_t typeCount = categories[state.categoryIndex].types.size();
    if (typeCount == 0U) {
        state.typeIndex = 0U;
    } else if (state.typeIndex >= typeCount) {
        state.typeIndex = typeCount - 1U;
    }
    return state;
}

/// 取当前选中的类型；无类别 / 越界 ⇒ `nullptr`。
[[nodiscard]] inline const ObjectType* SelectedPaletteType(const PaletteState& state,
                                                           const std::vector<PaletteCategory>& categories) noexcept {
    if (state.categoryIndex >= categories.size()) {
        return nullptr;
    }
    const PaletteCategory& category = categories[state.categoryIndex];
    if (state.typeIndex >= category.types.size()) {
        return nullptr;
    }
    return category.types[state.typeIndex];
}

}  // namespace vx
