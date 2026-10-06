#pragma once

#include <cstddef>
#include <cstdint>

namespace vx {

/// 世界指纹的**初始值**（FNV-1a 64 位偏移基准）。
[[nodiscard]] inline constexpr std::uint64_t FingerprintBegin() noexcept {
    return 14695981039346656037ULL;
}

/// 把一段原始字节混入指纹（FNV-1a 64 位）。
///
/// **顺序敏感**：调换字节顺序会得到不同结果（这正是"世界指纹要能区分不同内容"的前提）。
/// `bytes == 0` 时返回原值（不触碰 `data`，允许 `data == nullptr`）。
[[nodiscard]] inline std::uint64_t FingerprintMixBytes(std::uint64_t hash, const void* data,
                                                       std::size_t bytes) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    const auto*             bytesPtr = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= static_cast<std::uint64_t>(bytesPtr[i]);
        hash *= kPrime;
    }
    return hash;
}

/// 混入一个 64 位整数（按其 8 字节原样）。
[[nodiscard]] inline std::uint64_t FingerprintMixU64(std::uint64_t hash, std::uint64_t value) noexcept {
    return FingerprintMixBytes(hash, &value, sizeof(value));
}

/// 一个 tile 的**指纹输入视图**（不拥有数据，只在调用期间被读取）。
///
/// 前置条件：`count > 0` 时 `heights` 指向至少 `count` 个 `int16` 定点高度（`count == 0` 时可为空）。
struct FingerprintTileView {
    int                 tileX = 0;
    int                 tileZ = 0;
    const std::int16_t* heights = nullptr;  ///< 定点高度数组首地址（本项目 = `TerrainTile::heights.data()`）
    std::size_t         count = 0;          ///< 高度采样个数（本项目 = `kTerrainTileVertexCount²` = 65×65）
};

/// **世界指纹**（纯函数、确定性，红线 7）：`种子 + 按给定顺序的 tile（坐标 + 全部定点高度）` → 一个 64 位整数。
///
/// 用途（V2c / `docs/plans/v0.5.md` §1.10）：证明"**同种子切回同一世界 ⇒ 结果逐位相同**" ——
/// `--switch-test` 冒烟只能证明"没崩"，无法证明"切回来还是同一个世界"；指纹给红线 7 一个**可判定**的数字判据。
///
/// 约定：
///   - **顺序敏感**：调用方须按**确定的 tile 顺序**传入（本项目 = `TerrainWorld::ResidentTiles()` 的**升序**，
///     故结果**与流式到达顺序无关**）；
///   - **与 LOD 无关**：传入的是 tile 的**全分辨率高度**（LOD 只改网格，不改高度）⇒ 结果与玩家所见环带无关；
///   - **空集合**：得到"仅种子 + 计数 0"的稳定结果（不特判、不抛）。
[[nodiscard]] inline std::uint64_t ComputeTerrainFingerprint(std::uint64_t seed,
                                                             const FingerprintTileView* tiles,
                                                             std::size_t tileCount) noexcept {
    std::uint64_t hash = FingerprintBegin();
    hash               = FingerprintMixU64(hash, seed);
    hash               = FingerprintMixU64(hash, static_cast<std::uint64_t>(tileCount));
    for (std::size_t i = 0; i < tileCount; ++i) {
        const FingerprintTileView& tile = tiles[i];
        // 坐标按 int64 混入（负坐标不截断）；高度按 int16 原始字节混入（逐位，不做浮点换算）。
        hash = FingerprintMixU64(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(tile.tileX)));
        hash = FingerprintMixU64(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(tile.tileZ)));
        hash = FingerprintMixBytes(hash, tile.heights, tile.count * sizeof(std::int16_t));
    }
    return hash;
}

}  // namespace vx
