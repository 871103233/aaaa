// T37 延后破坏队列的纯逻辑单测（`game/destruction_queue.hpp`）。
//
// 判据（本文件钉死的语义）：
//   ① 入队即**排序 + 去重**（重复 / 乱序输入不改变队列内容）；
//   ② 取用顺序**确定**：先全部"重网格 + 上传"（按种类 → 坐标升序），再全部"重建碰撞体"；
//      这两点合起来保证"分帧处理"与"当帧一次做完"结果逐位相同（红线 7 / 11）；
//   ③ 排空后 `Empty()` 为真、`PendingUnits()` 归零；再次入队能正确复用（游标复位）。

#include "destruction_queue.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using vx::BlockCoord;
using vx::PendingDestruction;
using vx::TileCoord;

[[nodiscard]] std::vector<PendingDestruction::UnitKind> DrainKinds(PendingDestruction& queue) {
    std::vector<PendingDestruction::UnitKind> kinds;
    PendingDestruction::Unit                  unit;
    while (queue.TakeNext(unit)) {
        kinds.push_back(unit.kind);
    }
    return kinds;
}

}  // namespace

// ① 重复与乱序输入 ⇒ 队列内容确定（去重后升序）。
TEST(DestructionQueue, MergeSortsAndDeduplicates) {
    PendingDestruction queue;
    queue.MergeVolumeBlocks({ BlockCoord { 1, 2, 3 }, BlockCoord { 0, 0, 0 }, BlockCoord { 1, 2, 3 } });

    EXPECT_EQ(queue.PendingUnits(), 4U) << "2 个唯一块 × 2 个阶段";
    EXPECT_FALSE(queue.Empty());

    PendingDestruction::Unit unit;
    ASSERT_TRUE(queue.TakeNext(unit));
    EXPECT_EQ(unit.kind, PendingDestruction::UnitKind::VolumeRemesh);
    EXPECT_EQ(unit.block, (BlockCoord { 0, 0, 0 })) << "重网格阶段按坐标升序";
    ASSERT_TRUE(queue.TakeNext(unit));
    EXPECT_EQ(unit.kind, PendingDestruction::UnitKind::VolumeRemesh);
    EXPECT_EQ(unit.block, (BlockCoord { 1, 2, 3 }));
}

// ② 阶段顺序：重网格 + 上传**全部做完**，才开始重建碰撞体（避免"物理已通、画面还没洞"）。
TEST(DestructionQueue, RemeshStagePrecedesCollisionStage) {
    PendingDestruction queue;
    queue.MergeVolumeBlocks({ BlockCoord { 0, 0, 0 } });
    queue.MergeTiles({ TileCoord { 1, 1 } });

    const std::vector<PendingDestruction::UnitKind> kinds = DrainKinds(queue);
    ASSERT_EQ(kinds.size(), 4U);
    EXPECT_EQ(kinds[0], PendingDestruction::UnitKind::VolumeRemesh);
    EXPECT_EQ(kinds[1], PendingDestruction::UnitKind::TileRemesh);
    EXPECT_EQ(kinds[2], PendingDestruction::UnitKind::VolumeCollision);
    EXPECT_EQ(kinds[3], PendingDestruction::UnitKind::TileCollision);
}

// ② 同一输入两次运行 ⇒ 取用序列完全一致（确定性）。
TEST(DestructionQueue, TakeOrderIsDeterministic) {
    // 把取用序列压成 `int` 序列再比较（避免依赖坐标类型的 `operator==`）。
    const auto runOnce = []() {
        PendingDestruction queue;
        queue.MergeVolumeBlocks({ BlockCoord { 2, 0, 0 }, BlockCoord { 0, 0, 2 } });
        queue.MergeTiles({ TileCoord { 0, 1 }, TileCoord { 0, 1 } });

        std::vector<int>                  sequence;
        PendingDestruction::Unit          unit;
        while (queue.TakeNext(unit)) {
            sequence.push_back(static_cast<int>(unit.kind));
            sequence.push_back(unit.block.x);
            sequence.push_back(unit.block.y);
            sequence.push_back(unit.block.z);
            sequence.push_back(unit.tile.x);
            sequence.push_back(unit.tile.z);
        }
        return sequence;
    };

    EXPECT_EQ(runOnce(), runOnce());
}

// ③ 排空后可复用：`Empty()` / `PendingUnits()` 归零，下一批入队从干净状态开始。
TEST(DestructionQueue, DrainedQueueCanBeReused) {
    PendingDestruction queue;
    queue.MergeVolumeBlocks({ BlockCoord { 5, 5, 5 } });
    (void)DrainKinds(queue);

    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.PendingUnits(), 0U);

    PendingDestruction::Unit unit;
    EXPECT_FALSE(queue.TakeNext(unit)) << "空队列取用必须返回 false";

    queue.MergeTiles({ TileCoord { 7, 7 } });
    EXPECT_EQ(queue.PendingUnits(), 2U) << "复用后只应包含新入队的单位";
    const std::vector<PendingDestruction::UnitKind> kinds = DrainKinds(queue);
    ASSERT_EQ(kinds.size(), 2U);
    EXPECT_EQ(kinds[0], PendingDestruction::UnitKind::TileRemesh);
    EXPECT_EQ(kinds[1], PendingDestruction::UnitKind::TileCollision);
}
