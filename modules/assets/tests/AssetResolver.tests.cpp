/**
 * @file AssetResolver.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/AssetResolver: names to ids through the manifest.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetResolver.h>

#include <assets/AssetManifest.h>
#include <assets/AssetsModule.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace mir;

namespace
{
    constexpr const char *kRoot = "games/Game/assets";

    // A cooked directory whose manifest is rewritten from the entries given
    struct Cooked
    {
        std::filesystem::path dir;

        Cooked()
        {
            static std::atomic<int> counter{ 0 };
            dir = std::filesystem::temp_directory_path() /
                  ("mir_resolver_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
        }

        ~Cooked()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::vector<AssetManifestSourceEntry> &entries) const
        {
            REQUIRE(WriteBytes(dir / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    AssetManifestSourceEntry Entry(const char *id, const char *logical)
    {
        return { { MakeAssetId(id), 1, 1 }, "blob", logical, {} };
    }
}

TEST_CASE("A path resolves inside the game's assets folder", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("games/Game/assets/scripts/spin.lua", "games/Game/assets/scripts/spin.lua") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);

    CHECK(resolver.Resolve("scripts/spin.lua") == MakeAssetId("games/Game/assets/scripts/spin.lua"));
    // a path is normalized the way the cooker normalizes it
    CHECK(resolver.Resolve("scripts/../scripts/./spin.lua") == MakeAssetId("games/Game/assets/scripts/spin.lua"));
    CHECK(resolver.Resolve("scripts\\spin.lua") == MakeAssetId("games/Game/assets/scripts/spin.lua"));
}

TEST_CASE("A path that is not valid, or has no asset, is null and says why", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("games/Game/assets/a.txt", "games/Game/assets/a.txt") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);

    std::string error;
    CHECK(resolver.Resolve("../outside.txt", &error).IsNull());
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK(resolver.Resolve("missing.txt", &error).IsNull());
    CHECK(error == "no asset has the path 'games/Game/assets/missing.txt'");

    error.clear();
    CHECK(resolver.Resolve("", &error).IsNull());
    CHECK_FALSE(error.empty());

    // the error is optional
    CHECK(resolver.Resolve("missing.txt").IsNull());
}

TEST_CASE("A sub-asset with no path cannot be looked up", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("games/Game/assets/unnamed", "") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);

    CHECK(resolver.Resolve("unnamed").IsNull());
    CHECK(resolver.ResolveFull("").IsNull());
}

TEST_CASE("A full path resolves for engine assets", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("assets/ui/panel.png", "assets/ui/panel.png") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);

    CHECK(resolver.ResolveFull("assets/ui/panel.png") == MakeAssetId("assets/ui/panel.png"));
    CHECK(resolver.ResolveFull("assets/ui/missing.png").IsNull());
}

TEST_CASE("A lookup after a reload sees the new manifest", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("games/Game/assets/old.txt", "games/Game/assets/old.txt") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);
    REQUIRE_FALSE(resolver.Resolve("old.txt").IsNull());

    cooked.Write({ Entry("games/Game/assets/new.txt", "games/Game/assets/new.txt") });
    assets.BeginReload();
    assets.CommitReload();

    CHECK(resolver.Resolve("old.txt").IsNull());
    CHECK(resolver.Resolve("new.txt") == MakeAssetId("games/Game/assets/new.txt"));
}

TEST_CASE("PathOf gives the full path, or the id in hex", "[assets][resolver]")
{
    Cooked cooked;
    cooked.Write({ Entry("games/Game/assets/named.txt", "games/Game/assets/named.txt"),
        Entry("games/Game/assets/unnamed", "") });
    AssetsModule assets(cooked.dir);
    const AssetResolver resolver(assets, kRoot);

    CHECK(resolver.PathOf(MakeAssetId("games/Game/assets/named.txt")) == "games/Game/assets/named.txt");

    const AssetId unnamed = MakeAssetId("games/Game/assets/unnamed");
    CHECK(resolver.PathOf(unnamed).size() == 16);
    CHECK(resolver.PathOf(AssetId{ 0xABCDull }) == "000000000000abcd");
}
