#include "chunk/chunk_types.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace {

using vx::ChunkState;
using vx::NeighborStates;

[[nodiscard]] NeighborStates all(ChunkState state) {
    return NeighborStates { state, state, state, state };
}

}  // namespace

// 状态机顺序即推进顺序；枚举值必须严格递增，否则状态比较判据整体失效。
TEST(ChunkStateMachine, StatesAreStrictlyOrdered) {
    EXPECT_LT(static_cast<int>(ChunkState::Empty), static_cast<int>(ChunkState::Generating));
    EXPECT_LT(static_cast<int>(ChunkState::Generating), static_cast<int>(ChunkState::Generated));
    EXPECT_LT(static_cast<int>(ChunkState::Generated), static_cast<int>(ChunkState::Lit));
    EXPECT_LT(static_cast<int>(ChunkState::Lit), static_cast<int>(ChunkState::Meshed));
    EXPECT_LT(static_cast<int>(ChunkState::Meshed), static_cast<int>(ChunkState::Ready));
}

// 自身未达 Generated 时，无论邻居多就绪都不能建网格。
TEST(ChunkStateMachine, RejectsWhenSelfNotGenerated) {
    EXPECT_FALSE(vx::can_build_mesh(ChunkState::Empty, all(ChunkState::Ready)));
    EXPECT_FALSE(vx::can_build_mesh(ChunkState::Generating, all(ChunkState::Ready)));
}

// 自身已 Generated，但任一水平邻居未达 Lit —— 必须拒绝（跨区块边界依赖）。
TEST(ChunkStateMachine, RejectsWhenAnyNeighborNotLit) {
    const ChunkState not_lit[] = { ChunkState::Empty, ChunkState::Generating, ChunkState::Generated };

    for (const ChunkState bad : not_lit) {
        for (std::size_t i = 0; i < 4; ++i) {
            NeighborStates neighbors = all(ChunkState::Lit);
            neighbors[i]             = bad;
            EXPECT_FALSE(vx::can_build_mesh(ChunkState::Generated, neighbors))
                << "邻居索引 " << i << " 状态为 " << static_cast<int>(bad);
        }
    }
}

// 四个邻居中只要有一个缺失（未加载），就不能建网格。
TEST(ChunkStateMachine, RejectsWhenNeighborMissing) {
    for (std::size_t i = 0; i < 4; ++i) {
        NeighborStates neighbors = all(ChunkState::Lit);
        neighbors[i]             = ChunkState::Empty;
        EXPECT_FALSE(vx::can_build_mesh(ChunkState::Generated, neighbors)) << "邻居索引 " << i;
    }
}

// 前置条件全部满足 —— 允许建网格。
TEST(ChunkStateMachine, AcceptsWhenSelfGeneratedAndAllNeighborsLit) {
    EXPECT_TRUE(vx::can_build_mesh(ChunkState::Generated, all(ChunkState::Lit)));
    EXPECT_TRUE(vx::can_build_mesh(ChunkState::Meshed, all(ChunkState::Ready)));
}

// 尺寸常量自洽（§4.1：16×16×384，Section 为 16³）。
TEST(ChunkGeometry, ConstantsAreConsistent) {
    EXPECT_EQ(vx::kChunkHeight, vx::kSectionSize * vx::kSectionsPerChunk);
    EXPECT_EQ(vx::kChunkArea, vx::kSectionSize * vx::kSectionSize);
    EXPECT_EQ(vx::kSectionVolume, vx::kSectionSize * vx::kSectionSize * vx::kSectionSize);
    EXPECT_EQ(vx::kSectionsPerChunk, 24);
}
