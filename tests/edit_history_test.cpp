// 编辑操作撤销 / 重做（V0.11 / I3）的**纯逻辑**测试（[`plans/v0.11.md`](../docs/plans/v0.11.md) I3；ADR 0038 决策四"撤销重做"）。
//
// 判据（可判定）：
//   1. 每次操作的 `ApplyEditCommand` / `RevertEditCommand` **严格互逆** ⇒ 撤销后编辑层**逐字段回到初始**；
//   2. 连续 32 次放置 ⇒ 全部撤销后回到初始；再全部重做 ⇒ 与撤销前逐字段一致；
//   3. 保存后清栈（栈与文件一致）/ 新操作清空重做栈 / 容量上限丢最旧；
//   4. 删除分流两种情形（删本层条目 vs 记 `[[remove]]`）与建筑删除（含清掉的变暗覆盖）都可逆。

#include "object/edit_history.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using vx::EditCommand;
using vx::EditDarkeningTarget;
using vx::EditHistory;
using vx::EditOpKind;
using vx::ObjectBuilding;
using vx::ObjectBuildingDarkening;
using vx::ObjectBuildingPiece;
using vx::ObjectBuildingRemoval;
using vx::ObjectPlacement;
using vx::ObjectRemoval;
using vx::ObjectTable;

ObjectPlacement Placement(const std::string& typeId, float x, float z, float yaw = 0.0F) {
    ObjectPlacement p;
    p.typeId     = typeId;
    p.x          = x;
    p.y          = 1.0F;
    p.z          = z;
    p.yawDegrees = yaw;
    return p;
}

ObjectBuilding Building(const std::string& id, float x, float z) {
    ObjectBuilding b;
    b.id         = id;
    b.x          = x;
    b.z          = z;
    b.yawDegrees = 90.0F;
    ObjectBuildingPiece piece;
    piece.typeId = "kit_wall";
    piece.offsetY = 3.0F;
    b.pieces.push_back(piece);
    return b;
}

/// 编辑层五个可变向量的**逐字段**比较（判据"逐字段回到初始"）。
bool EditLayerEquals(const ObjectTable& a, const ObjectTable& b) {
    if (a.placements.size() != b.placements.size() || a.removals.size() != b.removals.size() ||
        a.buildings.size() != b.buildings.size() || a.buildingRemovals.size() != b.buildingRemovals.size() ||
        a.buildingDarkenings.size() != b.buildingDarkenings.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.placements.size(); ++i) {
        const ObjectPlacement& p = a.placements[i];
        const ObjectPlacement& q = b.placements[i];
        if (p.typeId != q.typeId || p.x != q.x || p.y != q.y || p.z != q.z || p.yawDegrees != q.yawDegrees) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.removals.size(); ++i) {
        const ObjectRemoval& p = a.removals[i];
        const ObjectRemoval& q = b.removals[i];
        if (p.typeId != q.typeId || p.x != q.x || p.z != q.z || p.tolerance != q.tolerance) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.buildings.size(); ++i) {
        const ObjectBuilding& p = a.buildings[i];
        const ObjectBuilding& q = b.buildings[i];
        if (p.id != q.id || p.x != q.x || p.z != q.z || p.yawDegrees != q.yawDegrees ||
            p.landingMode != q.landingMode || p.interiorDarkening != q.interiorDarkening ||
            p.pieces.size() != q.pieces.size()) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.buildingRemovals.size(); ++i) {
        if (a.buildingRemovals[i].buildingId != b.buildingRemovals[i].buildingId) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.buildingDarkenings.size(); ++i) {
        const ObjectBuildingDarkening& p = a.buildingDarkenings[i];
        const ObjectBuildingDarkening& q = b.buildingDarkenings[i];
        if (p.buildingId != q.buildingId || p.darkening != q.darkening) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(EditHistory, PlaceObjectApplyAndRevertAreInverse) {
    ObjectTable edit;
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind            = EditOpKind::PlaceObject;
    command.placement       = Placement("rock", 12.0F, -3.0F);
    command.objectIndex     = edit.placements.size();
    command.runtimeObjectId = 7;

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.placements.size(), 1U);
    EXPECT_EQ(edit.placements[0].typeId, "rock");

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, ThirtyTwoPlacementsUndoAllReturnToInitialThenRedoAll) {
    ObjectTable edit;
    edit.placements.push_back(Placement("seed", 0.0F, 0.0F));  // 非空初始（判据更严格）
    const ObjectTable initial = edit;

    EditHistory history;
    for (int i = 0; i < 32; ++i) {
        EditCommand command;
        command.kind        = EditOpKind::PlaceObject;
        command.placement   = Placement("rock", 10.0F + static_cast<float>(i), 20.0F);
        command.objectIndex = edit.placements.size();
        history.Record(command);
        vx::ApplyEditCommand(edit, command);
    }
    ASSERT_EQ(history.UndoCount(), 32U);
    const ObjectTable afterPlacing = edit;

    // 全部撤销 ⇒ 逐字段回到初始
    while (history.CanUndo()) {
        vx::RevertEditCommand(edit, history.UndoCommand());
        history.CommitUndo();
    }
    EXPECT_TRUE(EditLayerEquals(edit, initial));
    EXPECT_EQ(history.UndoCount(), 0U);
    EXPECT_EQ(history.RedoCount(), 32U);

    // 全部重做 ⇒ 与撤销前逐字段一致
    while (history.CanRedo()) {
        vx::ApplyEditCommand(edit, history.RedoCommand());
        history.CommitRedo();
    }
    EXPECT_TRUE(EditLayerEquals(edit, afterPlacing));
    EXPECT_EQ(history.UndoCount(), 32U);
    EXPECT_EQ(history.RedoCount(), 0U);
}

TEST(EditHistory, NewRecordClearsRedoStack) {
    EditHistory history;
    EditCommand first;
    first.kind        = EditOpKind::PlaceObject;
    first.placement   = Placement("a", 1.0F, 1.0F);
    first.objectIndex = 0;
    history.Record(first);
    history.CommitUndo();  // 模拟"撤销了一次"
    ASSERT_TRUE(history.CanRedo());

    EditCommand second;
    second.kind        = EditOpKind::PlaceObject;
    second.placement   = Placement("b", 2.0F, 2.0F);
    second.objectIndex = 0;
    history.Record(second);

    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.UndoCount(), 1U);
}

TEST(EditHistory, CapacityDropsOldestCommands) {
    EditHistory history(4);
    for (int i = 1; i <= 6; ++i) {
        EditCommand command;
        command.kind        = EditOpKind::PlaceObject;
        command.placement   = Placement("rock", static_cast<float>(i), 0.0F);
        command.objectIndex = 0;
        history.Record(command);
    }
    ASSERT_EQ(history.UndoCount(), 4U);
    // 保留的是最后 4 条（3,4,5,6）；栈顶 = 最新（6）。
    EXPECT_FLOAT_EQ(history.UndoCommand().placement.x, 6.0F);
    history.CommitUndo();
    EXPECT_FLOAT_EQ(history.UndoCommand().placement.x, 5.0F);
    history.CommitUndo();
    EXPECT_FLOAT_EQ(history.UndoCommand().placement.x, 4.0F);
    history.CommitUndo();
    EXPECT_FLOAT_EQ(history.UndoCommand().placement.x, 3.0F);
    history.CommitUndo();
    EXPECT_FALSE(history.CanUndo());
}

TEST(EditHistory, ClearEmptiesBothStacks) {
    EditHistory history;
    EditCommand command;
    command.kind        = EditOpKind::PlaceObject;
    command.placement   = Placement("a", 0.0F, 0.0F);
    command.objectIndex = 0;
    history.Record(command);
    history.CommitUndo();
    history.Clear();
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
}

TEST(EditHistory, RemoveObjectFromEditLayerIsInverse) {
    ObjectTable edit;
    edit.placements.push_back(Placement("rock", 3.0F, 4.0F));
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                = EditOpKind::RemoveObject;
    command.placement           = edit.placements[0];
    command.objectFromEditLayer = true;
    command.objectIndex         = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_TRUE(edit.placements.empty());
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, RemoveObjectFromBaseAppendsRemovalAndIsInverse) {
    ObjectTable edit;  // 基座物件不在本层 ⇒ 记一条 [[remove]]
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                = EditOpKind::RemoveObject;
    command.placement           = Placement("tree", 8.0F, 9.0F);
    command.objectFromEditLayer = false;
    command.objectIndex         = edit.removals.size();
    command.removal.typeId      = "tree";
    command.removal.x           = 8.0F;
    command.removal.z           = 9.0F;
    command.removal.tolerance   = 0.5F;

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.removals.size(), 1U);
    EXPECT_EQ(edit.removals[0].typeId, "tree");
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, PlaceBuildingApplyAndRevertAreInverse) {
    ObjectTable edit;
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                  = EditOpKind::PlaceBuilding;
    command.building              = Building("hut#1", 5.0F, 6.0F);
    command.buildingFromEditLayer = true;
    command.buildingIndex         = edit.buildings.size();

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.buildings.size(), 1U);
    EXPECT_EQ(edit.buildings[0].id, "hut#1");
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, RemoveBuildingFromEditLayerIsInverse) {
    ObjectTable edit;
    edit.buildings.push_back(Building("hut#1", 5.0F, 6.0F));
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                  = EditOpKind::RemoveBuilding;
    command.buildingId            = "hut#1";
    command.building              = edit.buildings[0];
    command.buildingFromEditLayer = true;
    command.buildingIndex         = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_TRUE(edit.buildings.empty());
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, RemoveBaseBuildingReinsertsErasedDarkeningOverrides) {
    ObjectTable edit;
    ObjectBuildingDarkening keep;
    keep.buildingId = "other";
    keep.darkening  = 0.3F;
    ObjectBuildingDarkening target;
    target.buildingId = "hut#1";
    target.darkening  = 0.7F;
    edit.buildingDarkenings.push_back(keep);
    edit.buildingDarkenings.push_back(target);
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                  = EditOpKind::RemoveBuilding;
    command.buildingId            = "hut#1";
    command.building              = Building("hut#1", 5.0F, 6.0F);
    command.buildingFromEditLayer = false;
    command.buildingIndex         = edit.buildingRemovals.size();
    command.erasedDarkenings.emplace_back(1U, edit.buildingDarkenings[1]);

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.buildingRemovals.size(), 1U);
    ASSERT_EQ(edit.buildingDarkenings.size(), 1U);
    EXPECT_EQ(edit.buildingDarkenings[0].buildingId, "other");

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, SetDarkeningOnEditLayerBuildingIsInverse) {
    ObjectTable edit;
    ObjectBuilding building = Building("hut#1", 0.0F, 0.0F);
    building.interiorDarkening = 0.2F;
    edit.buildings.push_back(building);
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind            = EditOpKind::SetBuildingDarkening;
    command.buildingId      = "hut#1";
    command.darkeningBefore = 0.2F;
    command.darkeningAfter  = 0.6F;
    command.darkeningTarget = EditDarkeningTarget::EditLayerBuilding;
    command.darkeningIndex  = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_FLOAT_EQ(edit.buildings[0].interiorDarkening, 0.6F);
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, SetDarkeningOnExistingOverrideIsInverse) {
    ObjectTable edit;
    ObjectBuildingDarkening override;
    override.buildingId = "hut#1";
    override.darkening  = 0.4F;
    edit.buildingDarkenings.push_back(override);
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind            = EditOpKind::SetBuildingDarkening;
    command.buildingId      = "hut#1";
    command.darkeningBefore = 0.4F;
    command.darkeningAfter  = 0.9F;
    command.darkeningTarget = EditDarkeningTarget::ExistingOverride;
    command.darkeningIndex  = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_FLOAT_EQ(edit.buildingDarkenings[0].darkening, 0.9F);
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, SetDarkeningNewOverrideInsertsAndRevertErases) {
    ObjectTable edit;
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind            = EditOpKind::SetBuildingDarkening;
    command.buildingId      = "hut#1";
    command.darkeningBefore = 0.45F;
    command.darkeningAfter  = 0.8F;
    command.darkeningTarget = EditDarkeningTarget::NewOverride;
    command.darkeningIndex  = edit.buildingDarkenings.size();

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.buildingDarkenings.size(), 1U);
    EXPECT_EQ(edit.buildingDarkenings[0].buildingId, "hut#1");
    EXPECT_FLOAT_EQ(edit.buildingDarkenings[0].darkening, 0.8F);

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, MixedOperationsUndoInReverseOrderReturnsToInitial) {
    ObjectTable edit;
    edit.placements.push_back(Placement("seed", 0.0F, 0.0F));
    const ObjectTable initial = edit;

    EditHistory history;

    // 1) 放置单件
    EditCommand place;
    place.kind        = EditOpKind::PlaceObject;
    place.placement   = Placement("rock", 4.0F, 5.0F);
    place.objectIndex = edit.placements.size();
    history.Record(place);
    vx::ApplyEditCommand(edit, place);

    // 2) 放置成套建筑
    EditCommand build;
    build.kind                  = EditOpKind::PlaceBuilding;
    build.building              = Building("hut#1", 9.0F, 9.0F);
    build.buildingFromEditLayer = true;
    build.buildingIndex         = edit.buildings.size();
    history.Record(build);
    vx::ApplyEditCommand(edit, build);

    // 3) 改该建筑变暗（本层建筑字段）
    EditCommand darken;
    darken.kind            = EditOpKind::SetBuildingDarkening;
    darken.buildingId      = "hut#1";
    darken.darkeningBefore = -1.0F;
    darken.darkeningAfter  = 0.5F;
    darken.darkeningTarget = EditDarkeningTarget::EditLayerBuilding;
    darken.darkeningIndex  = edit.buildings.size() - 1;
    history.Record(darken);
    vx::ApplyEditCommand(edit, darken);

    ASSERT_EQ(edit.placements.size(), initial.placements.size() + 1);
    ASSERT_EQ(edit.buildings.size(), 1U);
    ASSERT_FLOAT_EQ(edit.buildings[0].interiorDarkening, 0.5F);

    while (history.CanUndo()) {
        vx::RevertEditCommand(edit, history.UndoCommand());
        history.CommitUndo();
    }
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, MoveObjectInEditLayerIsInverse) {
    ObjectTable edit;
    edit.placements.push_back(Placement("rock", 1.0F, 2.0F));
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                = EditOpKind::MoveObject;
    command.moveObjectFrom      = edit.placements[0];
    command.moveObjectTo        = edit.placements[0];
    command.moveObjectTo.x      = 5.0F;
    command.moveObjectTo.z      = 6.0F;
    command.moveObjectTo.yawDegrees = 45.0F;
    command.moveObjectInEditLayer = true;
    command.moveObjectIndex     = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_FLOAT_EQ(edit.placements[0].x, 5.0F);
    EXPECT_FLOAT_EQ(edit.placements[0].yawDegrees, 45.0F);

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, MoveObjectFromBaseAppendsRemovalAndPlacement) {
    ObjectTable edit;  // 基座物件不在本层 ⇒ "记 [[remove]] + 落新 [[placement]]"
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                  = EditOpKind::MoveObject;
    command.moveObjectFrom        = Placement("tree", 2.0F, 3.0F);
    command.moveObjectTo          = Placement("tree", 7.0F, 8.0F);
    command.moveObjectInEditLayer = false;
    command.moveObjectRemoval.typeId    = "tree";
    command.moveObjectRemoval.x         = 2.0F;
    command.moveObjectRemoval.z         = 3.0F;
    command.moveObjectRemoval.tolerance = 0.5F;
    command.moveObjectRemovalIndex      = edit.removals.size();
    command.moveObjectPlacementIndex    = edit.placements.size();

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.removals.size(), 1U);
    ASSERT_EQ(edit.placements.size(), 1U);
    EXPECT_FLOAT_EQ(edit.placements[0].x, 7.0F);

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, MoveBuildingInEditLayerIsInverse) {
    ObjectTable edit;
    edit.buildings.push_back(Building("hut#1", 1.0F, 1.0F));
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                     = EditOpKind::MoveBuilding;
    command.moveBuildingFrom         = edit.buildings[0];
    command.moveBuildingTo           = edit.buildings[0];
    command.moveBuildingTo.x         = 9.0F;
    command.moveBuildingTo.z         = 9.0F;
    command.moveBuildingTo.yawDegrees = 30.0F;
    command.moveBuildingInEditLayer  = true;
    command.moveBuildingIndex        = 0;

    vx::ApplyEditCommand(edit, command);
    EXPECT_FLOAT_EQ(edit.buildings[0].x, 9.0F);
    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}

TEST(EditHistory, MoveBaseBuildingIsInverse) {
    ObjectTable edit;
    ObjectBuildingDarkening override;
    override.buildingId = "hut#1";
    override.darkening  = 0.6F;
    edit.buildingDarkenings.push_back(override);
    const ObjectTable initial = edit;

    EditCommand command;
    command.kind                      = EditOpKind::MoveBuilding;
    command.moveBuildingFrom          = Building("hut#1", 1.0F, 1.0F);
    command.moveBuildingTo            = Building("hut#1#2", 5.0F, 5.0F);  // 基座分支 ⇒ **新 id**
    command.moveBuildingInEditLayer   = false;
    command.moveBuildingRemovalIndex  = 0;
    command.moveBuildingPlacementIndex = 0;
    command.moveErasedDarkenings.emplace_back(0U, edit.buildingDarkenings[0]);

    vx::ApplyEditCommand(edit, command);
    ASSERT_EQ(edit.buildingRemovals.size(), 1U);
    ASSERT_EQ(edit.buildings.size(), 1U);
    EXPECT_EQ(edit.buildingRemovals[0].buildingId, "hut#1");
    EXPECT_EQ(edit.buildings[0].id, "hut#1#2");
    EXPECT_TRUE(edit.buildingDarkenings.empty());  // 旧 id 的覆盖被清掉

    vx::RevertEditCommand(edit, command);
    EXPECT_TRUE(EditLayerEquals(edit, initial));
}
