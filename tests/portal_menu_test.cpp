// 传送门交互菜单（V9）单测：菜单**数据模型**的组装语义（纯函数 `BuildPortalMenuModel`）。
// 见 docs/plans/v0.5.md §1.13。UI 只按模型绘制 ⇒「该显示什么 / 该给哪些动作」由本测试钉死。

#include "portal_menu.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace {

using vx::BuildPortalMenuModel;
using vx::LevelManifest;
using vx::PortalEntry;
using vx::PortalMenuModel;
using vx::UiLabel;
using vx::UiText;
using vx::WorldInstance;

[[nodiscard]] PortalEntry MakePortal(std::string target, std::string name = {}) {
    PortalEntry portal;
    portal.targetWorldId = std::move(target);
    portal.name          = std::move(name);
    return portal;
}

#ifdef VOXEL_SOURCE_DIR
[[nodiscard]] LevelManifest LoadRepoWorld(const char* id) {
    return LevelManifest::LoadFromFile(std::filesystem::path(VOXEL_SOURCE_DIR) / "assets" / "maps" /
                                       (std::string(id) + ".toml"));
}
#endif

TEST(PortalMenu, UnknownTargetOffersEnterOnlyAndFallsBackToWorldId) {
    const PortalEntry     portal = MakePortal("world_z");
    const PortalMenuModel model =
        BuildPortalMenuModel(portal, /*targetManifest=*/nullptr, /*instance=*/nullptr, /*cjk=*/true);

    EXPECT_EQ(model.targetWorldId, "world_z");
    EXPECT_EQ(model.realmName, "world_z");  // 未注册世界 ⇒ 用 id 充当显示名
    EXPECT_FALSE(model.canReset);           // 非肉鸽（未知）⇒ 不给「重置」
    EXPECT_EQ(model.generation, 0U);
}

TEST(PortalMenu, MissingPortalNameFallsBackToDefaultLabelPerLanguage) {
    const PortalEntry portal = MakePortal("world_z");

    const PortalMenuModel cjk   = BuildPortalMenuModel(portal, nullptr, nullptr, /*cjk=*/true);
    const PortalMenuModel ascii = BuildPortalMenuModel(portal, nullptr, nullptr, /*cjk=*/false);

    EXPECT_STREQ(cjk.portalName.c_str(), UiText(UiLabel::PortalDefaultName, true));
    EXPECT_STREQ(ascii.portalName.c_str(), UiText(UiLabel::PortalDefaultName, false));
    // "绝不缺字"口径：无 CJK 字体时缺省门名必须是**纯 ASCII**。
    EXPECT_TRUE(vx::IsAsciiOnly(ascii.portalName.c_str()));
}

TEST(PortalMenu, ConfiguredNameWinsOverDefault) {
    const PortalEntry     portal = MakePortal("world_z", "通往小世界的传送门");
    const PortalMenuModel model  = BuildPortalMenuModel(portal, nullptr, nullptr, /*cjk=*/true);

    EXPECT_EQ(model.portalName, "通往小世界的传送门");
    EXPECT_EQ(model.targetWorldId, "world_z");
}

#ifdef VOXEL_SOURCE_DIR
TEST(PortalMenu, RoguelikeTargetOffersResetEvenBeforeFirstInstance) {
    const LevelManifest   manifest = LoadRepoWorld("world_c");
    const PortalMenuModel model =
        BuildPortalMenuModel(MakePortal("world_c"), &manifest, /*instance=*/nullptr, /*cjk=*/true);

    // 所有者 2026-10-06：C（肉鸽）按 E ⇒ 「进入 + 重置」⇒ **即便尚未进入过**也提供「重置」。
    EXPECT_TRUE(model.canReset);
    EXPECT_EQ(model.realmName, manifest.name);
    EXPECT_EQ(model.generation, 0U);
}

TEST(PortalMenu, PremadeTargetDoesNotOfferReset) {
    const LevelManifest   manifest = LoadRepoWorld("world_b");
    const PortalMenuModel model =
        BuildPortalMenuModel(MakePortal("world_b"), &manifest, /*instance=*/nullptr, /*cjk=*/true);

    // 所有者 2026-10-06：B（预制）按 E ⇒ 只有「进入」。
    EXPECT_FALSE(model.canReset);
    EXPECT_EQ(model.realmName, manifest.name);
}

TEST(PortalMenu, ExistingInstanceExposesGenerationCount) {
    const LevelManifest   manifest = LoadRepoWorld("world_c");
    const WorldInstance   instance { "world_c", 12345ULL, 3U };
    const PortalMenuModel model = BuildPortalMenuModel(MakePortal("world_c"), &manifest, &instance, /*cjk=*/true);

    EXPECT_TRUE(model.canReset);
    EXPECT_EQ(model.generation, 3U);  // 已重置 3 次 ⇒ UI 显示为「第 4 次生成」
}
#endif

}  // namespace
