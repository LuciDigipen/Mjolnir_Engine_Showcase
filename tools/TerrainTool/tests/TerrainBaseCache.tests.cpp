/**
 * @file TerrainBaseCache.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainBaseCache and sync's cache maintenance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainBaseCache.h>
#include <TerrainPackage.h>
#include <TerrainPrune.h>
#include <TerrainSync.h>

#include <TerrainGenerator.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainBaseHeights.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <bit>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    struct BaseTree
    {
        ScratchDir scratch;
        fs::path gameDir = scratch.path / "games" / "G";
        fs::path releases = TerrainReleasesRoot(gameDir);
        fs::path recipe = gameDir / "assets" / "terrain" / "relief-2x2.terrain.json";
        AssetId asset = MakeAssetId(ExampleAssetPath());
        fs::path assetDir = TerrainAssetReleasesDir(releases, asset);

        explicit BaseTree(std::string_view tag, std::string_view fixture = "relief-2x2.terrain.json")
            : scratch(tag)
        {
            fs::create_directories(recipe.parent_path());
            REQUIRE(fs::copy_file(fs::path(TERRAIN_FIXTURE_DIR) / fixture, recipe,
                                  fs::copy_options::overwrite_existing));
        }

        TerrainSyncRequest Request() const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.gameDir = gameDir;
            request.candidateDir = scratch.path / "build" / "terrain" / "G" / "relief-2x2";
            return request;
        }

        TerrainReport Sync() const
        {
            TerrainReport report = RunTerrainSync(Request());
            for (const TerrainDiagnostic &d : report.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            for (const TerrainDiagnostic &w : report.warnings)
                UNSCOPED_INFO(FormatDiagnostic(w));
            for (const TerrainCheck &c : report.checks)
                UNSCOPED_INFO(c.name << ": " << CheckStatusName(c.status) << " " << c.detail);
            return report;
        }

        void SetSeed(uint64_t seed) const
        {
            WriteSeedRecipe(recipe.parent_path(), seed, recipe.filename().string());
        }

        fs::path BaseRoot() const
        {
            return TerrainBaseHeightsRoot(releases, asset);
        }
    };

    GlobalSampleIndex EditSample(const TerrainLayout &layout)
    {
        const TileCoord tile = layout.Tiles().min;
        return {layout.TileFirstSample(tile.x) + 2, layout.TileFirstSample(tile.z) + 2};
    }
}

TEST_CASE("Sync writes the generator's source heights before edit layers to the base cache",
          "[terraintool][sync][base]")
{
    const std::string fixture = GENERATE(std::string("relief-2x2.terrain.json"), std::string("mixed-2x2.terrain.json"));
    INFO(fixture);
    BaseTree tree("base_source", fixture);

    const RecipeParseResult plain = LoadTerrainRecipe(tree.recipe);
    REQUIRE(plain.recipe);
    TerrainEditLayer layer = AdditiveEditLayer("touch-up");
    SetTerrainEditSample(layer, plain.recipe->layout, EditSample(plain.recipe->layout), 4.0f);
    WriteEditLayers(tree.recipe, std::span(&layer, 1));

    const TerrainReport report = tree.Sync();
    REQUIRE(report.Ok());
    REQUIRE(report.recipeFingerprint);
    CHECK(report.Status(check::kBase) == CheckStatus::Passed);

    const RecipeParseResult edited = LoadTerrainRecipe(tree.recipe);
    REQUIRE(edited.recipe);
    REQUIRE(edited.recipe->editStackHash != 0);
    TerrainRecipe unedited = *edited.recipe;
    unedited.editStack.reset();
    unedited.editStackHash = 0;
    const TerrainSourceFingerprint key = BaseHeightsFingerprint(*edited.recipe);
    CHECK(key == FingerprintRecipe(unedited));
    CHECK(key != *report.recipeFingerprint);

    const TerrainBaseHeightsResult<TerrainBaseHeights> base = TerrainBaseHeights::Open(tree.releases, tree.asset);
    INFO(base.error.message);
    REQUIRE(base);
    CHECK(base.value->Key() == key);

    const TerrainGenerator generator(*edited.recipe);
    const TileRange range = edited.recipe->layout.Tiles();
    for (int64_t tz = range.min.z; tz <= range.max.z; ++tz)
        for (int64_t tx = range.min.x; tx <= range.max.x; ++tx)
        {
            const TileCoord tile{int32_t(tx), int32_t(tz)};
            const std::optional<TerrainLayout> sourceLayout = edited.recipe->SourceLayout(tile);
            REQUIRE(sourceLayout);
            std::optional<TileHeights> expected = TileHeights::Create(tile, sourceLayout->IntervalsPerTile());
            REQUIRE(expected);
            const SourceFillResult filled = generator.FillTile(*sourceLayout, tile, expected->Samples());
            REQUIRE(filled.Ok());

            const TerrainBaseHeightsResult<TileHeights> read = base.value->ReadTile(tile);
            INFO(read.error.message);
            REQUIRE(read);
            REQUIRE(read.value->Samples().size() == expected->Samples().size());
            for (size_t i = 0; i < expected->Samples().size(); ++i)
                CHECK(std::bit_cast<uint32_t>(read.value->Samples()[i]) ==
                      std::bit_cast<uint32_t>(expected->Samples()[i]));
        }

    const TileCoord editedTile = edited.recipe->layout.Tiles().min;
    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(current);
    const TerrainResult<TileHeights> finalTile = current.value->ReadTile({tree.asset, editedTile});
    const TerrainBaseHeightsResult<TileHeights> baseTile = base.value->ReadTile(editedTile);
    REQUIRE(finalTile);
    REQUIRE(baseTile);
    CHECK(std::bit_cast<uint32_t>(finalTile.value->At(2, 2)) !=
          std::bit_cast<uint32_t>(baseTile.value->At(2, 2)));
}

TEST_CASE("Sync keeps the base cache across edits and rewrites it for recipe changes", "[terraintool][sync][base]")
{
    BaseTree tree("base_keep");
    const TerrainRecipe plain = LoadExample();
    TerrainEditLayer layer = AdditiveEditLayer("touch-up");
    const GlobalSampleIndex sample = EditSample(plain.layout);
    SetTerrainEditSample(layer, plain.layout, sample, 4.0f);
    WriteEditLayers(tree.recipe, std::span(&layer, 1));

    const TerrainReport first = tree.Sync();
    REQUIRE(first.Ok());
    REQUIRE(first.publish->published);
    const TerrainBaseHeightsResult<TerrainBaseHeights> base = TerrainBaseHeights::Open(tree.releases, tree.asset);
    REQUIRE(base);
    const TerrainSourceFingerprint key = base.value->Key();
    const fs::path keyDir = TerrainBaseHeightsDir(tree.releases, tree.asset, key);
    REQUIRE(WriteText(keyDir / "marker.txt", "keep"));

    SetTerrainEditSample(layer, plain.layout, sample, 9.0f);
    const TerrainEditTile &changedTile = layer.tiles.at(TileCoord{-1, -1});
    const TerrainResult<std::vector<std::byte>> bytes =
        EncodeTerrainEditTile(changedTile, layer.info.heightBlend);
    REQUIRE(bytes);
    WriteFile(TerrainEditLayersDir(tree.recipe) / TerrainEditLayerDirName(layer.info.sequence) /
                  TerrainEditTileFileName(changedTile.tile),
              *bytes.value);

    const TerrainReport edited = tree.Sync();
    REQUIRE(edited.Ok());
    CHECK(edited.publish->published);
    CHECK(edited.Status(check::kBase) == CheckStatus::Passed);
    CHECK(ReadText(keyDir / "marker.txt") == "keep");
    const TerrainBaseHeightsResult<TerrainBaseHeights> kept = TerrainBaseHeights::Open(tree.releases, tree.asset);
    REQUIRE(kept);
    CHECK(kept.value->Key() == key);

    tree.SetSeed(1234);
    const TerrainReport reseeded = tree.Sync();
    REQUIRE(reseeded.Ok());
    CHECK(reseeded.publish->published);
    const TerrainBaseHeightsResult<TerrainBaseHeights> next = TerrainBaseHeights::Open(tree.releases, tree.asset);
    REQUIRE(next);
    CHECK(next.value->Key() != key);
    CHECK_FALSE(fs::exists(keyDir));

    fs::remove_all(tree.BaseRoot());
    const TerrainReport rebuilt = tree.Sync();
    REQUIRE(rebuilt.Ok());
    CHECK_FALSE(rebuilt.publish->published);
    CHECK(rebuilt.Status(check::kBase) == CheckStatus::Passed);
    CHECK(rebuilt.Status(check::kPointer) == CheckStatus::Skipped);
    const TerrainBaseHeightsResult<TerrainBaseHeights> rewritten =
        TerrainBaseHeights::Open(tree.releases, tree.asset);
    INFO(rewritten.error.message);
    REQUIRE(rewritten);
    CHECK(rewritten.value->Key() == next.value->Key());
    const TerrainBaseHeightsResult<TileHeights> tile = rewritten.value->ReadTile({-1, -1});
    REQUIRE(tile);
}

TEST_CASE("A base cache write failure is a warning and never fails sync", "[terraintool][sync][base]")
{
    BaseTree tree("base_warning");
    fs::create_directories(tree.assetDir);
    REQUIRE(WriteText(tree.assetDir / std::string(kTerrainBaseHeightsDirName), "not a directory"));

    const TerrainReport report = tree.Sync();
    REQUIRE(report.Ok());
    CHECK(report.publish->published);
    CHECK(report.Status(check::kBase) == CheckStatus::Skipped);
    REQUIRE_FALSE(report.warnings.empty());
    CHECK(report.warnings.front().check == check::kBase);
    CHECK(report.warnings.front().message.find("not written") != std::string::npos);
}

TEST_CASE("Prune and package ignore the base cache", "[terraintool][sync][base][prune]")
{
    BaseTree tree("base_ignore");
    REQUIRE(tree.Sync().Ok());
    const TerrainBaseHeightsResult<TerrainBaseHeights> base = TerrainBaseHeights::Open(tree.releases, tree.asset);
    REQUIRE(base);
    const TerrainSourceFingerprint key = base.value->Key();
    const fs::path keyDir = TerrainBaseHeightsDir(tree.releases, tree.asset, key);
    const std::vector<TerrainStoredRelease> listed = ListTerrainReleases(tree.releases, tree.asset);
    CHECK(listed.size() == 1);

    TerrainPruneRequest prune;
    prune.idRoot = std::string(kIdRoot);
    prune.sourcePath = std::string(kSourcePath);
    prune.gameDir = tree.gameDir;
    const TerrainReport pruned = RunTerrainPrune(prune);
    for (const TerrainDiagnostic &d : pruned.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(pruned.Ok());
    CHECK(fs::is_regular_file(TerrainBaseHeightsPointerPath(tree.releases, tree.asset)));
    CHECK(fs::is_regular_file(keyDir / kTerrainIndexFileName));
    CHECK(ListTerrainReleases(tree.releases, tree.asset).size() == 1);

    const auto sourceTree = Snapshot(tree.releases);
    const fs::path outDir = tree.scratch.path / "build" / "games" / "G";
    TerrainPackageRequest package;
    package.idRoot = std::string(kIdRoot);
    package.sourcePath = std::string(kSourcePath);
    package.gameDir = tree.gameDir;
    package.outDir = outDir;
    package.lockWait = std::chrono::milliseconds(0);
    const TerrainReport packaged = RunTerrainPackage(package);
    for (const TerrainDiagnostic &d : packaged.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(packaged.Ok());
    const fs::path outAssetDir = TerrainAssetReleasesDir(TerrainReleasesRoot(outDir), tree.asset);
    CHECK_FALSE(fs::exists(outAssetDir / std::string(kTerrainBaseHeightsDirName)));
    CHECK(Snapshot(tree.releases) == sourceTree);
}
