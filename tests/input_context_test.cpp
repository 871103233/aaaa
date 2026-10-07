// V0.11 / A6：**输入上下文栈 + 动作归属表**的矩阵单测（[ADR 0040](../../docs/adr/0040-input-context-stack-and-action-ownership-table.md)）。
//
// 这组用例替代"靠人读 `game/main.cpp` 核对按键归属"：把「面板状态 × 模式状态 × 动作」的期望**钉死**在这里。
// 其中「光球仅自由活动」与「移动 / 飞行三模式同源」是 SKILL《三种模式与输入隔离》硬规则 1 / 5 的可判定判据。

#include "input_context.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

using vx::ActionId;
using vx::EscTarget;
using vx::InputContext;
using vx::InputContextState;
using vx::InputOwner;
using vx::OwnerOf;

/// 构造一个上下文状态（C++17：不用指派初始化器）。
constexpr InputContextState MakeState(bool build = false, bool modify = false, bool gizmoDragging = false,
                                      bool objectPaletteOpen = false, bool portalMenuOpen = false,
                                      bool systemPanelOpen = false) noexcept {
    return InputContextState { build, modify, gizmoDragging, objectPaletteOpen, portalMenuOpen, systemPanelOpen };
}

const InputContextState kFreeRoam = MakeState();
const InputContextState kBuild    = MakeState(/*build=*/true);
const InputContextState kModify   = MakeState(/*build=*/false, /*modify=*/true);
const InputContextState kModifyDrag = MakeState(/*build=*/false, /*modify=*/true, /*gizmoDragging=*/true);
const InputContextState kModal    = MakeState(false, false, false, false, false, /*systemPanelOpen=*/true);

/// 三种玩法模式（不含模态面板）：用于"三模式同源"的通用断言。
const std::array<InputContextState, 4> kGameplayContexts { kFreeRoam, kBuild, kModify, kModifyDrag };

}  // namespace

// ---- 一、上下文选择（栈顶）----

TEST(InputContext, CurrentContextPriorityAndMutualExclusion) {
    EXPECT_EQ(vx::CurrentContext(kFreeRoam), InputContext::FreeRoam);
    EXPECT_EQ(vx::CurrentContext(kBuild), InputContext::Build);
    EXPECT_EQ(vx::CurrentContext(kModify), InputContext::Modify);
    EXPECT_EQ(vx::CurrentContext(kModifyDrag), InputContext::ModifyDrag);
    EXPECT_EQ(vx::CurrentContext(kModal), InputContext::Modal);
    // 模态面板是**栈顶**：即使同时处于修改模式，也以模态为准（防御性；运行时两者互斥）。
    EXPECT_EQ(vx::CurrentContext(MakeState(false, true, false, false, /*portalMenuOpen=*/true)), InputContext::Modal);
}

TEST(InputContext, AnyBlockingPanelCountsAsModalButReadOnlyOverlaysDoNot) {
    EXPECT_FALSE(vx::ModalPanelOpen(kFreeRoam)) << "F1 调试面板 / 常驻 HUD 是只读叠加层，不进上下文状态";
    EXPECT_TRUE(vx::ModalPanelOpen(MakeState(false, false, false, /*objectPaletteOpen=*/true)));
    EXPECT_TRUE(vx::ModalPanelOpen(MakeState(false, false, false, false, /*portalMenuOpen=*/true)));
    EXPECT_TRUE(vx::ModalPanelOpen(MakeState(false, false, false, false, false, /*systemPanelOpen=*/true)));
}

// ---- 二、自由活动（base）----

TEST(InputContext, FreeRoamOwnsFireballInteractCameraAndMovement) {
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::Attack), InputOwner::Free) << "左键 = 发射光球";
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::Interact), InputOwner::Free) << "E = 传送门交互";
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::LookX), InputOwner::Free) << "鼠标位移 = 转相机";
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::LookY), InputOwner::Free);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::MoveForward), InputOwner::Free);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::Jump), InputOwner::Free);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::ToggleFly), InputOwner::Free);
}

TEST(InputContext, FreeRoamLeavesPlacementKeysInert) {
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementRotateLeft), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementRotateRight), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementLandingMode), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementDarkenDown), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementDarkenUp), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kFreeRoam, ActionId::PlacementRemove), InputOwner::Inactive);
}

// ---- 三、建造模式 ----

TEST(InputContext, BuildOwnsPlacementAndSwallowsInteract) {
    EXPECT_EQ(OwnerOf(kBuild, ActionId::Attack), InputOwner::Build) << "左键 = 放下（不发光球）";
    EXPECT_EQ(OwnerOf(kBuild, ActionId::Interact), InputOwner::Inactive) << "E 让位：不触发传送门交互";
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementRotateLeft), InputOwner::Build);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementRotateRight), InputOwner::Build);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementLandingMode), InputOwner::Build);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementDarkenDown), InputOwner::Build);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementDarkenUp), InputOwner::Build);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::PlacementRemove), InputOwner::Build);
    // 相机与移动**穿透到 base**（建造模式下仍能转视角 / 走动）。
    EXPECT_EQ(OwnerOf(kBuild, ActionId::LookX), InputOwner::Free);
    EXPECT_EQ(OwnerOf(kBuild, ActionId::MoveForward), InputOwner::Free);
}

// ---- 四、修改模式（拖动与否的唯一差别 = 鼠标位移归谁）----

TEST(InputContext, ModifyOwnsAttackAndKeepsCameraOnBase) {
    EXPECT_EQ(OwnerOf(kModify, ActionId::Attack), InputOwner::Modify) << "左键 = 选中 / 拖动（不发光球）";
    EXPECT_EQ(OwnerOf(kModify, ActionId::Interact), InputOwner::Inactive) << "E 让位";
    EXPECT_EQ(OwnerOf(kModify, ActionId::PlacementLandingMode), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kModify, ActionId::PlacementDarkenDown), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kModify, ActionId::PlacementDarkenUp), InputOwner::Inactive);
    EXPECT_EQ(OwnerOf(kModify, ActionId::PlacementRemove), InputOwner::Modify);
    EXPECT_EQ(OwnerOf(kModify, ActionId::LookX), InputOwner::Free) << "未拖动 ⇒ 位移仍归相机";
}

TEST(InputContext, ModifyDragMovesLookToTheGizmo) {
    EXPECT_EQ(OwnerOf(kModifyDrag, ActionId::LookX), InputOwner::Modify) << "拖动中 ⇒ 位移归 gizmo，相机不转";
    EXPECT_EQ(OwnerOf(kModifyDrag, ActionId::LookY), InputOwner::Modify);
    EXPECT_EQ(OwnerOf(kModifyDrag, ActionId::Attack), InputOwner::Modify);
    EXPECT_EQ(OwnerOf(kModifyDrag, ActionId::MoveForward), InputOwner::Free) << "拖动中仍可走动";
}

// ---- 五、模态面板：玩法全量阻断，面板级热键不受影响 ----

TEST(InputContext, ModalBlocksAllGameplayInput) {
    const std::array<ActionId, 12> gameplay {
        ActionId::Attack,      ActionId::Interact,     ActionId::LookX,     ActionId::LookY,
        ActionId::MoveForward, ActionId::MoveBackward, ActionId::MoveLeft,  ActionId::MoveRight,
        ActionId::Jump,        ActionId::Sprint,       ActionId::ToggleFly, ActionId::FlyDown,
    };
    for (const ActionId action : gameplay) {
        EXPECT_EQ(OwnerOf(kModal, action), InputOwner::Blocked)
            << "模态面板期间玩法输入绝不泄漏到世界（action=" << static_cast<int>(action) << "）";
    }
    EXPECT_EQ(OwnerOf(kModal, ActionId::PlacementRotateLeft), InputOwner::Blocked);
    EXPECT_EQ(OwnerOf(kModal, ActionId::PlacementLandingMode), InputOwner::Blocked);
    EXPECT_EQ(OwnerOf(kModal, ActionId::PlacementRemove), InputOwner::Blocked);
    EXPECT_EQ(OwnerOf(kModal, ActionId::PlacementRepeatLast), InputOwner::Blocked) << "F3 在模态期间不生效";
}

TEST(InputContext, PanelLevelHotkeysSurviveModalPanels) {
    const std::array<ActionId, 10> hotkeys {
        ActionId::ToggleDebugPanel,          ActionId::PickPlacement,               ActionId::PlacementSave,
        ActionId::PlacementUndo,             ActionId::PlacementRedo,               ActionId::PlacementModifierCtrl,
        ActionId::PlacementToggleRotateHold, ActionId::PlacementToggleNeighborSnap, ActionId::PlacementToggleGridSnap,
        ActionId::ToggleSystemPanel,
    };
    for (const ActionId action : hotkeys) {
        EXPECT_EQ(OwnerOf(kModal, action), InputOwner::AlwaysOn)
            << "面板级热键在任意上下文（含模态）都生效（action=" << static_cast<int>(action) << "）";
        for (const InputContextState& state : kGameplayContexts) {
            EXPECT_EQ(OwnerOf(state, action), InputOwner::AlwaysOn);
        }
    }
}

TEST(InputContext, PaletteConfirmIsPanelInternalInEveryContext) {
    for (const InputContextState& state : kGameplayContexts) {
        EXPECT_EQ(OwnerOf(state, ActionId::PaletteConfirm), InputOwner::PanelInternal);
    }
    EXPECT_EQ(OwnerOf(kModal, ActionId::PaletteConfirm), InputOwner::PanelInternal);
}

// ---- 六、结构性不变量（SKILL 硬规则 1 / 5 的可判定判据）----

TEST(InputContext, FireballIsFreeRoamOnlyInEveryOtherContext) {
    for (const InputContextState& state : kGameplayContexts) {
        if (vx::CurrentContext(state) == InputContext::FreeRoam) {
            EXPECT_EQ(OwnerOf(state, ActionId::Attack), InputOwner::Free);
        } else {
            EXPECT_NE(OwnerOf(state, ActionId::Attack), InputOwner::Free)
                << "**光球仅自由活动可用**（SKILL 硬规则 1）—— 建造 / 修改模式必须夺走 `Attack`";
        }
    }
    EXPECT_NE(OwnerOf(kModal, ActionId::Attack), InputOwner::Free);
}

TEST(InputContext, MovementJumpSprintAndFlyAreSharedByAllGameplayModes) {
    const std::array<ActionId, 8> shared {
        ActionId::MoveForward, ActionId::MoveBackward, ActionId::MoveLeft,  ActionId::MoveRight,
        ActionId::Jump,        ActionId::Sprint,       ActionId::ToggleFly, ActionId::FlyDown,
    };
    for (const InputContextState& state : kGameplayContexts) {
        for (const ActionId action : shared) {
            EXPECT_EQ(OwnerOf(state, action), InputOwner::Free)
                << "移动 / 跳跃 / 冲刺 / 飞行在三种模式下**逐位同源**（穿透复用 base）；"
                << "context=" << static_cast<int>(vx::CurrentContext(state)) << " action=" << static_cast<int>(action);
        }
    }
}

TEST(InputContext, ScanningEveryContextAndActionYieldsNoUnexpectedOwner) {
    // 全矩阵扫描：只验两条不变量（取值合法 + 模态无 Free）；逐格期望值由上面的用例钉死。
    for (std::size_t c = 0; c < vx::kInputContextCount; ++c) {
        const InputContext context = static_cast<InputContext>(c);
        InputContextState state {};
        switch (context) {
            case InputContext::FreeRoam: break;
            case InputContext::Build: state.build = true; break;
            case InputContext::Modify: state.modify = true; break;
            case InputContext::ModifyDrag: state.modify = true; state.gizmoDragging = true; break;
            case InputContext::Modal: state.systemPanelOpen = true; break;
            case InputContext::Count: continue;
        }
        for (std::size_t a = 0; a < vx::kActionCount; ++a) {
            const InputOwner owner = OwnerOf(state, static_cast<ActionId>(a));
            EXPECT_LE(static_cast<int>(owner), static_cast<int>(InputOwner::PanelInternal));
            if (context == InputContext::Modal) {
                EXPECT_NE(owner, InputOwner::Free) << "模态上下文不得存在 Free 归属（action=" << a << "）";
            }
        }
    }
}

// ---- 七、`Esc` 栈顶弹出 ----

TEST(InputContext, EscPopsTopOfStackInOrder) {
    EXPECT_EQ(vx::EscPopTarget(kFreeRoam), EscTarget::ToggleSystemPanel);
    EXPECT_EQ(vx::EscPopTarget(kBuild), EscTarget::ExitBuild);
    EXPECT_EQ(vx::EscPopTarget(kModify), EscTarget::ExitModify);
    EXPECT_EQ(vx::EscPopTarget(MakeState(false, false, false, false, /*portalMenuOpen=*/true)), EscTarget::ClosePortalMenu);
    EXPECT_EQ(vx::EscPopTarget(MakeState(false, false, false, /*objectPaletteOpen=*/true)), EscTarget::CloseObjectPalette);
}

TEST(InputContext, EscPopsPaletteBeforeModeWhenBothPresent) {
    // 运行期可达的组合：修改模式中按 `F2` 打开选择器（`F2` 只退建造、不退修改）⇒ 先关选择器，再退修改。
    EXPECT_EQ(vx::EscPopTarget(MakeState(false, true, false, /*objectPaletteOpen=*/true)), EscTarget::CloseObjectPalette);
    EXPECT_EQ(vx::EscPopTarget(kModify), EscTarget::ExitModify);
}
