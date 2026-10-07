// 世界定义一致性哈希（V0.10 / S6；[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策六）。
//
// 覆盖：① **确定性**（同输入同值）；② **敏感性**（改任一**参与**字段 ⇒ 值变）；
// ③ **不误伤**（改 `climate` 这类"不参与地形生成"的字段 ⇒ 生成参数哈希**不变**）；
// ④ 名称**长度前缀**编码无歧义（("ab","c") ≠ ("a","bc")）。

#include "dig/dig_region.hpp"
#include "generation/terrain_params.hpp"
#include "save/world_save.hpp"  // kFnv1a64OffsetBasis（空输入基值，用于"非空"断言）

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using vx::DigRegion;
using vx::DigRegionContentHash;
using vx::DigRegionTable;
using vx::TerrainGenerationParams;
using vx::TerrainParamsContentHash;

[[nodiscard]] DigRegion MakeRegion(const char* name, bool diggable, int priority, int minBlock, int maxBlock) {
    DigRegion region;
    region.name     = name;
    region.diggable = diggable;
    region.priority = priority;
    region.blockMin.x = minBlock;
    region.blockMin.y = minBlock;
    region.blockMin.z = minBlock;
    region.blockMax.x = maxBlock;
    region.blockMax.y = maxBlock;
    region.blockMax.z = maxBlock;
    return region;
}

// ---------------------- 地形生成参数内容哈希 ----------------------

TEST(WorldDefinitionHash, TerrainParamsIsDeterministic) {
    const TerrainGenerationParams params = TerrainGenerationParams::Default();
    EXPECT_EQ(TerrainParamsContentHash(params), TerrainParamsContentHash(params));
    // 已经喂入域标签 + 字段 ⇒ 一定不是"空输入"的偏移基值（非空证据）。
    EXPECT_NE(TerrainParamsContentHash(params), vx::kFnv1a64OffsetBasis);
}

TEST(WorldDefinitionHash, TerrainParamsSensitiveToGenerationFields) {
    const TerrainGenerationParams base     = TerrainGenerationParams::Default();
    const std::uint64_t          baseline = TerrainParamsContentHash(base);

    TerrainGenerationParams changed = base;
    changed.baseFrequency += 0.001F;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "三层噪声频率应参与";

    changed                   = base;
    changed.landform.enabled  = !base.landform.enabled;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "地貌分区开关应参与";

    changed                                    = base;
    changed.landform.mountainsAmplitudeScale += 0.1F;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "地貌分区数值应参与";

    changed               = base;
    changed.caves.enabled = true;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "洞穴参数应参与";

    changed              = base;
    changed.river.maxNodes += 1;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "河流参数应参与";

    changed                  = base;
    changed.overhang.seedChannel += 1;
    EXPECT_NE(TerrainParamsContentHash(changed), baseline) << "悬垂参数应参与";
}

/// **关键语义**：`climate` 不参与任何地形生成 ⇒ 只改气候**不得**改变生成参数哈希
/// （否则"改气候 ⇒ 误拒有效档"，见 `plans/v0.10.md` §3.「S6 动手前评价」）。
TEST(WorldDefinitionHash, TerrainParamsIgnoresClimate) {
    const TerrainGenerationParams base        = TerrainGenerationParams::Default();
    TerrainGenerationParams       climateOnly = base;
    climateOnly.climate.temperatureFrequency += 0.0005F;
    climateOnly.climate.humidityFrequency -= 0.0005F;
    climateOnly.climate.temperatureSeedChannel += 1;
    climateOnly.climate.humiditySeedChannel += 1;
    EXPECT_EQ(TerrainParamsContentHash(climateOnly), TerrainParamsContentHash(base));
}

// ---------------------- 可挖区域表内容哈希 ----------------------

TEST(WorldDefinitionHash, DigRegionIsDeterministicAndSensitive) {
    const DigRegionTable table = DigRegionTable::FromRegions(
        { MakeRegion("cave", true, 1, 0, 3), MakeRegion("sealed_core", false, 5, 1, 2) }, 24, 8);
    const std::uint64_t baseline = DigRegionContentHash(table);
    EXPECT_EQ(DigRegionContentHash(table), baseline) << "同输入应同值";

    const DigRegionTable bigger = DigRegionTable::FromRegions(
        { MakeRegion("cave", true, 1, 0, 4), MakeRegion("sealed_core", false, 5, 1, 2) }, 24, 8);
    EXPECT_NE(DigRegionContentHash(bigger), baseline) << "块包围盒应参与";

    const DigRegionTable flipped = DigRegionTable::FromRegions(
        { MakeRegion("cave", false, 1, 0, 3), MakeRegion("sealed_core", false, 5, 1, 2) }, 24, 8);
    EXPECT_NE(DigRegionContentHash(flipped), baseline) << "diggable 应参与";

    const DigRegionTable reprioritized = DigRegionTable::FromRegions(
        { MakeRegion("cave", true, 9, 0, 3), MakeRegion("sealed_core", false, 5, 1, 2) }, 24, 8);
    EXPECT_NE(DigRegionContentHash(reprioritized), baseline) << "priority 应参与";

    const DigRegionTable rebanded = DigRegionTable::FromRegions(
        { MakeRegion("cave", true, 1, 0, 3), MakeRegion("sealed_core", false, 5, 1, 2) }, 32, 8);
    EXPECT_NE(DigRegionContentHash(rebanded), baseline) << "竖向带宽应参与";

    const DigRegionTable renamed = DigRegionTable::FromRegions(
        { MakeRegion("cave2", true, 1, 0, 3), MakeRegion("sealed_core", false, 5, 1, 2) }, 24, 8);
    EXPECT_NE(DigRegionContentHash(renamed), baseline) << "名称应参与";
}

/// 名称用**长度前缀**编码 ⇒ `("ab","c")` 与 `("a","bc")` 必须得到不同哈希（拼接无歧义）。
TEST(WorldDefinitionHash, DigRegionNameEncodingIsUnambiguous) {
    const DigRegionTable lhs =
        DigRegionTable::FromRegions({ MakeRegion("ab", true, 0, 0, 1), MakeRegion("c", true, 0, 0, 1) });
    const DigRegionTable rhs =
        DigRegionTable::FromRegions({ MakeRegion("a", true, 0, 0, 1), MakeRegion("bc", true, 0, 0, 1) });
    EXPECT_NE(DigRegionContentHash(lhs), DigRegionContentHash(rhs));
}

}  // namespace
