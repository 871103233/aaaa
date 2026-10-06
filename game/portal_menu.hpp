#pragma once

#include "generation/level_manifest.hpp"
#include "portal_interaction.hpp"
#include "ui_text.hpp"
#include "world_manager.hpp"

#include <cstdint>
#include <string>

namespace vx {

/// 传送门交互菜单上的**动作**（V9；所有者 2026-10-06："走近门按 `E` 后出菜单"）。
enum class PortalAction : std::uint8_t {
    None,    ///< 未选择（菜单仍开着）
    Enter,   ///< **进入**该门的目标世界（复用已有秘境实例）
    Reset,   ///< **重置**（销毁并重生）该肉鸽秘境 —— 只在目标为 `instance_roguelike` 时提供
    Cancel,  ///< **取消**：关掉菜单，**不切换世界**（保持"不自动切换"）
};

/// UI 层回给 game 层的**一次选择**（动作 + 目标世界）——取走后动作清零（只生效一次）。
struct PortalMenuRequest {
    PortalAction action = PortalAction::None;
    std::string  targetWorldId;
};

/// 菜单要显示的**数据**（纯数据；由 game 层组装、交给 ImGui 层只读绘制）。
///
/// 为什么不让 UI 直接查 `WorldManager`：UI 层不认识世界族与实例账本 —— 保持"**数据进来、UI 只画**"的分工，
/// 并让"该显示什么 / 该给哪些动作"**可被单测钉死**（`tests/portal_menu_test.cpp`）。
struct PortalMenuModel {
    std::string portalName;     ///< 门名（配置 `portal_name`；空 ⇒ 取缺省「神秘传送门」）
    std::string realmName;      ///< 目标秘境显示名（清单 `name`；**可含 CJK**）
    std::string targetWorldId;  ///< 目标世界 id（**纯 ASCII** ⇒ 无 CJK 字体时的回退显示）
    bool        canReset  = false;  ///< 是否提供「重置」（⇒ 目标为 `instance_roguelike`）
    std::uint32_t generation = 0;   ///< 该秘境实例**已重置次数**（0 = 尚未重置；显示为 `第 N+1 次生成`）
};

/// 组装菜单模型（**纯函数**）：目标清单可空（未注册 ⇒ 只给「进入」、不给「重置」）。
///
/// `cjk` = 是否已加载 CJK 字体（决定缺省文案取中文还是**纯 ASCII** 回退 —— 沿用"绝不缺字"口径）。
[[nodiscard]] inline PortalMenuModel BuildPortalMenuModel(const PortalEntry& portal,
                                                          const LevelManifest* targetManifest,
                                                          const WorldInstance* instance,
                                                          bool cjk) {
    PortalMenuModel model;
    model.portalName = portal.name.empty() ? UiText(UiLabel::PortalDefaultName, cjk) : portal.name;
    model.realmName = (targetManifest != nullptr) ? targetManifest->name : portal.targetWorldId;
    model.targetWorldId = portal.targetWorldId;
    // 「重置」对**任何肉鸽秘境**都提供 —— 即便尚未进入过（此时 = "重新生成一个待进入的秘境"）；
    // 预制 / 主世界不提供（`ResetInstance` / `EnsureInstance` 对非 `instance_roguelike` 也会拒绝）。
    model.canReset = (targetManifest != nullptr) &&
                     (targetManifest->family == WorldFamily::InstanceRoguelike);
    model.generation = (instance != nullptr) ? instance->generation : 0U;
    return model;
}

}  // namespace vx
