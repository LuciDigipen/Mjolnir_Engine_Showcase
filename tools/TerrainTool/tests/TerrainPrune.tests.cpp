/**
 * @file TerrainPrune.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainPrune.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainPrune.h>
#include <TerrainPublish.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    // A game directory with one asset's releases; publishes do not prune.
    struct PruneTree
    {
        ScratchDir scratch;
        fs::path gameDir = scratch.path / "games" / "G";
        fs::path releases = TerrainReleasesRoot(gameDir);
        AssetId asset = MakeAssetId(ExampleAssetPath());
        fs::path assetDir = TerrainAssetReleasesDir(releases, asset);

        explicit PruneTree(std::string_view tag) : scratch(tag) {}

        TerrainReleaseId Publish(std::string_view name, uint64_t nonce) const
        {
            const fs::path candidate = scratch.path / std::string(name);
            WriteCraftedCandidate(candidate,
                                  [](auto &)
                                  {
                                  },
                                  nonce);
            TerrainPublishRequest request;
            request.candidateDir = candidate;
            request.releasesRoot = releases;
            const TerrainReport report = RunTerrainPublish(request);
            for (const TerrainDiagnostic &d : report.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            REQUIRE(report.Ok());
            return *report.publish->current;
        }

        TerrainReport Prune() const
        {
            TerrainPruneRequest request;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.gameDir = gameDir;
            TerrainReport report = RunTerrainPrune(request);
            for (const TerrainDiagnostic &d : report.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            for (const TerrainCheck &c : report.checks)
                UNSCOPED_INFO(c.name << ": " << CheckStatusName(c.status) << " " << c.detail);
            return report;
        }
    };

    std::string DetailOf(const TerrainReport &report, std::string_view name)
    {
        for (const TerrainCheck &c : report.checks)
            if (c.name == name)
                return c.detail;
        return {};
    }

    uint64_t StoredBytes(const fs::path &releases, AssetId asset, TerrainReleaseId release)
    {
        for (const TerrainStoredRelease &stored : ListTerrainReleases(releases, asset))
            if (stored.release == release)
                return stored.bytes;
        return 0;
    }
}

TEST_CASE("Prune deletes every release but current and stale staging directories", "[terraintool][prune]")
{
    PruneTree tree("prune_all");
    const TerrainReleaseId a = tree.Publish("a", 0xA);
    const TerrainReleaseId b = tree.Publish("b", 0xB);
    REQUIRE(a != b);
    REQUIRE(ReleaseDirNames(tree.assetDir).size() == 2);
    const uint64_t bytes = StoredBytes(tree.releases, tree.asset, a);
    REQUIRE(bytes > 0);

    const fs::path partial = tree.assetDir / (TerrainIdHex(0xC) + ".partial");
    fs::create_directories(partial);
    REQUIRE(WriteText(partial / kTerrainIndexFileName, "stale"));

    const TerrainReport report = tree.Prune();
    REQUIRE(report.Ok());
    CHECK(report.Status(check::kArguments) == CheckStatus::Passed);
    CHECK(report.Status(check::kLock) == CheckStatus::Passed);
    CHECK(report.Status(check::kPrune) == CheckStatus::Passed);
    CHECK(ReleaseDirNames(tree.assetDir) == std::vector<std::string>{TerrainIdHex(b.value)});
    CHECK_FALSE(fs::exists(TerrainReleaseDir(tree.releases, tree.asset, a)));
    CHECK_FALSE(fs::exists(partial));
    CHECK(PeekTerrainCurrent(tree.releases, tree.asset) == b);

    const std::string detail = DetailOf(report, check::kPrune);
    CHECK(detail.find(TerrainIdHex(a.value)) != std::string::npos);
    CHECK(detail.find(TerrainIdHex(b.value) + " (current)") != std::string::npos);
    CHECK(detail.find("freed " + std::to_string(bytes) + " bytes") != std::string::npos);
    CHECK(detail.find(".partial") != std::string::npos);

    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(current);
    CHECK(current.value->VerifyAllPacks().code == TerrainPackErrorCode::None);

    // The next prune finds nothing to delete.
    const TerrainReport again = tree.Prune();
    REQUIRE(again.Ok());
    CHECK(DetailOf(again, check::kPrune).find("deleted nothing") != std::string::npos);
}

TEST_CASE("Prune keeps a release an open handle holds until the handle is gone", "[terraintool][prune][lock]")
{
    PruneTree tree("prune_held");
    const TerrainReleaseId a = tree.Publish("a", 0xA);
    const TerrainReleaseId b = tree.Publish("b", 0xB);
    REQUIRE(a != b);

    TerrainResult<TerrainReleaseHandle> held = OpenTerrainRelease(tree.releases, tree.asset, a);
    REQUIRE(held);
    const TerrainReport kept = tree.Prune();
    REQUIRE(kept.Ok());
    CHECK(kept.Status(check::kPrune) == CheckStatus::Passed);
    CHECK(DetailOf(kept, check::kPrune).find("(in use)") != std::string::npos);
    CHECK(fs::exists(TerrainReleaseDir(tree.releases, tree.asset, a)));
    CHECK(ReleaseDirNames(tree.assetDir).size() == 2);
    CHECK_FALSE(ReadAllTiles(*held.value).empty());
    held.value.reset();

    const TerrainReport next = tree.Prune();
    REQUIRE(next.Ok());
    CHECK(next.Status(check::kPrune) == CheckStatus::Passed);
    CHECK_FALSE(fs::exists(TerrainReleaseDir(tree.releases, tree.asset, a)));
    CHECK(ReleaseDirNames(tree.assetDir) == std::vector<std::string>{TerrainIdHex(b.value)});
}

TEST_CASE("Prune removes nothing without a readable current release", "[terraintool][prune]")
{
    PruneTree tree("prune_no_current");
    tree.Publish("a", 0xA);
    tree.Publish("b", 0xB);
    REQUIRE(fs::remove(TerrainCurrentPointerPath(tree.releases, tree.asset)));

    const TerrainReport report = tree.Prune();
    REQUIRE(report.Ok());
    CHECK(report.Status(check::kPrune) == CheckStatus::Passed);
    CHECK(ReleaseDirNames(tree.assetDir).size() == 2);
    CHECK(DetailOf(report, check::kPrune).find("no current release") != std::string::npos);
}
