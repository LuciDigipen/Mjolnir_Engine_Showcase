/**
 * @file AssetsModule.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/AssetsModule: startup load and the reload swap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetsModule.h>
#include <core/log/SetupError.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    struct CookedDir
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("mir_assets_module_" + std::to_string(mir::CurrentProcessId()));

        CookedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path, ec);
        }

        ~CookedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void WriteManifest(std::span<const std::byte> bytes) const
        {
            std::ofstream out(path / mir::kAssetManifestFileName, std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    };

    const std::byte kJunk[] = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
}

TEST_CASE("A build with no manifest starts with an empty cache", "[assets][module]")
{
    const CookedDir dir;
    mir::AssetsModule assets(dir.path);
    CHECK_FALSE(assets.HadManifest());
}

TEST_CASE("A corrupt manifest fails startup as build output", "[assets][module]")
{
    const CookedDir dir;
    dir.WriteManifest(kJunk);

    try
    {
        mir::AssetsModule assets(dir.path);
        FAIL("a corrupt manifest was accepted");
    }
    catch (const mir::SetupError &error)
    {
        CHECK(error.Category() == mir::ErrorCategory::BuildOutput);
    }
}

TEST_CASE("A reload swaps the cache in place", "[assets][module]")
{
    const CookedDir dir;
    dir.WriteManifest(mir::BuildAssetManifestBlob({}));
    mir::AssetsModule assets(dir.path);
    REQUIRE(assets.HadManifest());

    mir::AssetCache *before = &assets.Cache();
    assets.BeginReload();
    assets.Candidate();
    assets.CommitReload();

    CHECK(&assets.Cache() == before);
}

TEST_CASE("A reload with a corrupt manifest throws and keeps the cache", "[assets][module]")
{
    const CookedDir dir;
    dir.WriteManifest(mir::BuildAssetManifestBlob({}));
    mir::AssetsModule assets(dir.path);
    mir::AssetCache *before = &assets.Cache();

    dir.WriteManifest(kJunk);
    CHECK_THROWS_AS(assets.BeginReload(), std::runtime_error);
    assets.AbortReload();

    CHECK(&assets.Cache() == before);
}

TEST_CASE("The reload epoch counts committed reloads only", "[assets][module]")
{
    const CookedDir dir;
    dir.WriteManifest(mir::BuildAssetManifestBlob({}));
    mir::AssetsModule assets(dir.path);
    CHECK(assets.ReloadEpoch() == 0);

    assets.BeginReload();
    CHECK(assets.ReloadEpoch() == 0);
    assets.AbortReload();
    CHECK(assets.ReloadEpoch() == 0);

    assets.BeginReload();
    assets.CommitReload();
    CHECK(assets.ReloadEpoch() == 1);

    assets.BeginReload();
    assets.CommitReload();
    CHECK(assets.ReloadEpoch() == 2);
}
