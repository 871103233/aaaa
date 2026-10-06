// V2c 世界指纹（`game/world_fingerprint.hpp`）—— 纯函数、确定性。
//
// 锁定四件事（缺一不可，见 docs/plans/v0.5.md §1.10）：
//   ① 同输入 ⇒ 同输出（可复现）；
//   ② 种子 / 高度 不同 ⇒ 不同（能区分内容）；
//   ③ **顺序敏感**（tile 顺序不同 ⇒ 不同 ⇒ 不能靠"随便某个顺序"蒙对）；
//   ④ 空集合稳定、且仍随种子变化。

#include <gtest/gtest.h>

#include "world_fingerprint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

/// 造一段可区分的定点高度（`start` 不同 ⇒ 内容不同）。
std::vector<std::int16_t> MakeHeights(std::int16_t start, std::size_t count) {
    std::vector<std::int16_t> heights(count);
    for (std::size_t i = 0; i < count; ++i) {
        heights[i] = static_cast<std::int16_t>(start + static_cast<std::int16_t>(i));
    }
    return heights;
}

constexpr std::uint64_t kSeed = 20261006ULL;

}  // namespace

TEST(WorldFingerprint, SameInputIsReproducible) {
    const std::vector<std::int16_t> heights = MakeHeights(100, 65 * 65);
    const std::array<vx::FingerprintTileView, 2> tiles { {
        vx::FingerprintTileView { 0, 0, heights.data(), heights.size() },
        vx::FingerprintTileView { 1, 0, heights.data(), heights.size() },
    } };

    const std::uint64_t first  = vx::ComputeTerrainFingerprint(kSeed, tiles.data(), tiles.size());
    const std::uint64_t second = vx::ComputeTerrainFingerprint(kSeed, tiles.data(), tiles.size());
    EXPECT_EQ(first, second);
}

TEST(WorldFingerprint, DifferentSeedDiffers) {
    const std::vector<std::int16_t> heights = MakeHeights(100, 65 * 65);
    const vx::FingerprintTileView   tile { 0, 0, heights.data(), heights.size() };

    EXPECT_NE(vx::ComputeTerrainFingerprint(kSeed, &tile, 1),
              vx::ComputeTerrainFingerprint(kSeed + 1ULL, &tile, 1));
}

TEST(WorldFingerprint, DifferentHeightsDiffer) {
    const std::vector<std::int16_t> a = MakeHeights(100, 65 * 65);
    const std::vector<std::int16_t> b = MakeHeights(101, 65 * 65);
    const vx::FingerprintTileView   tileA { 0, 0, a.data(), a.size() };
    const vx::FingerprintTileView   tileB { 0, 0, b.data(), b.size() };

    EXPECT_NE(vx::ComputeTerrainFingerprint(kSeed, &tileA, 1),
              vx::ComputeTerrainFingerprint(kSeed, &tileB, 1));
}

TEST(WorldFingerprint, DifferentTileCoordsDiffer) {
    const std::vector<std::int16_t> heights = MakeHeights(100, 65 * 65);
    const vx::FingerprintTileView   origin { 0, 0, heights.data(), heights.size() };
    const vx::FingerprintTileView   shifted { 1, 0, heights.data(), heights.size() };

    EXPECT_NE(vx::ComputeTerrainFingerprint(kSeed, &origin, 1),
              vx::ComputeTerrainFingerprint(kSeed, &shifted, 1));
}

TEST(WorldFingerprint, TileOrderIsSignificant) {
    const std::vector<std::int16_t> a = MakeHeights(100, 65 * 65);
    const std::vector<std::int16_t> b = MakeHeights(200, 65 * 65);
    const vx::FingerprintTileView   tileA { 0, 0, a.data(), a.size() };
    const vx::FingerprintTileView   tileB { 1, 0, b.data(), b.size() };

    const std::array<vx::FingerprintTileView, 2> forward { { tileA, tileB } };
    const std::array<vx::FingerprintTileView, 2> reversed { { tileB, tileA } };

    EXPECT_NE(vx::ComputeTerrainFingerprint(kSeed, forward.data(), forward.size()),
              vx::ComputeTerrainFingerprint(kSeed, reversed.data(), reversed.size()));
}

TEST(WorldFingerprint, AddingATileChangesResult) {
    const std::vector<std::int16_t> heights = MakeHeights(100, 65 * 65);
    const vx::FingerprintTileView   tile { 0, 0, heights.data(), heights.size() };

    EXPECT_NE(vx::ComputeTerrainFingerprint(kSeed, &tile, 1),
              vx::ComputeTerrainFingerprint(kSeed, &tile, 0));
}

TEST(WorldFingerprint, EmptySetIsStableAndSeedDependent) {
    const std::uint64_t empty = vx::ComputeTerrainFingerprint(kSeed, nullptr, 0);
    EXPECT_EQ(empty, vx::ComputeTerrainFingerprint(kSeed, nullptr, 0));
    EXPECT_NE(empty, vx::ComputeTerrainFingerprint(kSeed + 1ULL, nullptr, 0));
}
