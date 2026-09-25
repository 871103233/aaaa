#pragma once

#include <array>
#include <cstdint>

// 体素世界的基础类型与常量。
//
// 这里是若干「架构决定」的唯一落地点，改动前请先读 docs/tech-plan-v1.3.md §4.1：
//   - 区块尺寸与 Section 划分（存储/网格/光照的最小单元）
//   - 区块状态机及其推进顺序
//   - 跨区块边界依赖的唯一判据（can_build_mesh）
//   - 世界坐标使用整数类型（绝不用 float 承担世界定位）

namespace vx {

// ---------------------------------------------------------------
// 尺寸常量（见 §4.1）
// ---------------------------------------------------------------
inline constexpr int kSectionSize      = 16;                                // Section 边长（16³）
inline constexpr int kSectionsPerChunk = 24;                                // 384 / 16
inline constexpr int kChunkHeight      = kSectionSize * kSectionsPerChunk;  // 384
inline constexpr int kChunkArea        = kSectionSize * kSectionSize;       // 256 方块/层
inline constexpr int kSectionVolume    = kSectionSize * kSectionSize * kSectionSize;

/// 方块 ID。调色板 + 位打包存储，故这里只保证语义宽度，不保证内存布局。
using BlockId = std::uint16_t;

inline constexpr BlockId kBlockAir = 0;

// ---------------------------------------------------------------
// 坐标
// ---------------------------------------------------------------

/// 区块坐标（单位：区块）
struct ChunkPos {
    std::int32_t x = 0;
    std::int32_t z = 0;
};

/// 方块坐标（单位：方块；整数，禁止用 float）
struct BlockPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

// ---------------------------------------------------------------
// 区块状态机（禁止跳步；见技能规范红线 12）
//
//   Empty → Generating → Generated → Lit → Meshed → Ready
//
// 枚举值的顺序即推进顺序，测试会断言其严格递增。
// ---------------------------------------------------------------
enum class ChunkState : std::uint8_t {
    Empty = 0,
    Generating,
    Generated,
    Lit,
    Meshed,
    Ready,
};

/// 四个水平邻居，顺序固定为 -X、+X、-Z、+Z。
using NeighborStates = std::array<ChunkState, 4>;

[[nodiscard]] constexpr auto to_index(ChunkState state) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(state);
}

/// 网格构建的前置条件：自身已达 Generated，且 4 个水平邻居均达 Lit。
///
/// 这是「跨区块边界依赖」的唯一定义处 —— 任何网格化路径都必须经过本判据，
/// 不得绕过（否则会出现接缝破面或光照错位）。
[[nodiscard]] constexpr bool can_build_mesh(ChunkState self, const NeighborStates& neighbors) noexcept {
    if (to_index(self) < to_index(ChunkState::Generated)) {
        return false;
    }
    for (const ChunkState neighbor : neighbors) {
        if (to_index(neighbor) < to_index(ChunkState::Lit)) {
            return false;
        }
    }
    return true;
}

}  // namespace vx
