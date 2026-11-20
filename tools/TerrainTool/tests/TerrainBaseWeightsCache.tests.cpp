/**
 * @file TerrainBaseWeightsCache.tests.cpp
 * @author Jackey Cen
 * @brief Tests for sync's rule weights cache and the commands that must ignore it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainBake.h>
#include <TerrainBaseCache.h>
#include <TerrainPackage.h>
#include <TerrainPrune.h>
#include <TerrainSync.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainBaseWeights.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainWeights.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;
using Names = std::vector<std::string>;

namespace
{
    Json Materials()
    {
        Json sand = Json{{"name", "sand"},
                         {"rules", Json{{"height", Json{{"min", 10.0}, {"max", 40.0}, {"blend", 8.0}}}}}};
        Json materials = Json::object();
        materials["layers"] = Json::array({Json{{"name", "base"}}, sand});
        return materials;
    }

    fs::path WriteMaterialRecipe(const fs::path &dir, const Json &materials)
    {
        const std::optional<std::string> text = ReadText(ExampleRecipePath());
        REQUIRE(text);
        Json json = Json::parse(*text);
        json["materials"] = materials;
        fs::create_directories(dir);
        const fs::path path = dir / "relief-2x2.terrain.json";
        REQUIRE(WriteText(path, json.dump(2)));
        return path;
    }

    void SetSandMinimum(const fs::path &recipe, double minimum)
    {
        const std::optional<std::string> text = ReadText(recipe);
        REQUIRE(text);
        Json json = Json::parse(*text);
        json["materials"]["layers"][1]["rules"]["height"]["min"] = minimum;
        REQUIRE(WriteText(recipe, json.dump(2)));
    }

    TerrainRecipe RecipeOf(const fs::path &path)
    {
        RecipeParseResult parsed = LoadTerrainRecipe(path);
        REQUIRE(parsed.recipe);
        return *parsed.recipe;
    }

    struct WeightCacheTree
    {
        ScratchDir scratch;
        fs::path recipeDir = scratch.path / "assets" / "terrain";
        fs::path recipe = WriteMaterialRecipe(recipeDir, Materials());
        fs::path gameDir = scratch.path / "game";
        fs::path releases = TerrainReleasesRoot(gameDir);
        AssetId asset = MakeAssetId(ExampleAssetPath());

        explicit WeightCacheTree(std::string_view tag) : scratch(tag) {}

        TerrainSyncRequest Request(std::string_view build = "build") const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.gameDir = gameDir;
            request.candidateDir = scratch.path / std::string(build) / "terrain" / "relief-2x2";
            return request;
        }

        TerrainReport Sync(std::string_view build = "build") const
        {
            TerrainReport report = RunTerrainSync(Request(build));
            for (const TerrainDiagnostic &d : report.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            for (const TerrainDiagnostic &w : report.warnings)
                UNSCOPED_INFO(FormatDiagnostic(w));
            for (const TerrainCheck &c : report.checks)
                UNSCOPED_INFO(c.name << ": " << CheckStatusName(c.status) << " " << c.detail);
            return report;
        }

        fs::path BaseRoot() const
        {
            return TerrainBaseWeightsRoot(releases, asset);
        }

        fs::path KeyDir(TerrainSourceFingerprint key) const
        {
            return TerrainBaseWeightsDir(releases, asset, key);
        }
    };

    // A layer that paints one material over the whole first tile without moving any height.
    TerrainEditLayer PaintLayer(const TerrainLayout &layout, float value)
    {
        TerrainEditLayer layer = AdditiveEditLayer("paint");
        TerrainEditTile tile;
        tile.tile = layout.Tiles().min;
        tile.intervals = layout.IntervalsPerTile();
        const size_t count = static_cast<size_t>(tile.intervals + 1) * static_cast<size_t>(tile.intervals + 1);
        tile.weights.push_back({"sand", std::vector<float>(count, value)});
        layer.tiles.emplace(tile.tile, std::move(tile));
        return layer;
    }

    std::vector<std::string> LayersOf(const TerrainBaseWeights &base)
    {
        return {base.Layers().begin(), base.Layers().end()};
    }
}

TEST_CASE("Sync writes the bake's rule weights before the weight channels", "[terraintool][sync][baseweights]")
{
    WeightCacheTree tree("baseweights_write");
    const TerrainRecipe recipe = RecipeOf(tree.recipe);
    const TerrainEditLayer paint = PaintLayer(recipe.layout, 1.0f);
    WriteEditLayers(tree.recipe, std::span(&paint, 1));

    const TerrainReport report = tree.Sync();
    REQUIRE(report.Ok());
    REQUIRE(report.publish->published);
    CHECK(report.Status(check::kBaseWeights) == CheckStatus::Passed);

    const TerrainBaseWeightsResult<TerrainBaseWeights> base = TerrainBaseWeights::Open(tree.releases, tree.asset);
    INFO(base.error.message);
    REQUIRE(base);
    CHECK(base.value->Key() == BaseWeightsFingerprint(recipe));
    CHECK(LayersOf(*base.value) == Names{"base", "sand"});
    CHECK(fs::is_regular_file(tree.KeyDir(base.value->Key()) / kTerrainBaseWeightsIndexFileName));
    CHECK(fs::is_regular_file(tree.KeyDir(base.value->Key()) / kTerrainBaseWeightsPackFileName));

    TerrainReport scratchReport("rule weights", {});
    std::optional<std::vector<TileHeights>> finalHeights = BakeFinalHeights(recipe, scratchReport);
    REQUIRE(finalHeights);
    std::optional<std::vector<TerrainWeightTile>> expected = BakeRuleWeights(recipe, *finalHeights, scratchReport);
    REQUIRE(expected);
    for (const TerrainWeightTile &rule : *expected)
    {
        const TerrainBaseWeightsResult<TerrainWeightTile> read = base.value->ReadTile(rule.tile);
        INFO(read.error.message);
        REQUIRE(read);
        REQUIRE(read.value->layers == rule.layers);
        REQUIRE(read.value->weights.size() == rule.weights.size());
        for (size_t s = 0; s < rule.weights.size(); ++s)
            CHECK(std::abs(read.value->weights[s] - rule.weights[s]) <= 1.0 / 255.0 + 1e-6);
    }

    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(current);
    const TerrainResult<TerrainDecodedWeights> painted = current.value->ReadWeights(recipe.layout.Tiles().min);
    REQUIRE(painted);
    const TerrainWeightTile composited = ExpandTerrainWeights(*painted.value);
    const TerrainBaseWeightsResult<TerrainWeightTile> rule = base.value->ReadTile(recipe.layout.Tiles().min);
    REQUIRE(rule);
    const auto sand = std::find(composited.layers.begin(), composited.layers.end(), "sand");
    REQUIRE(sand != composited.layers.end());
    const size_t sandLayer = static_cast<size_t>(sand - composited.layers.begin());
    bool repainted = false;
    for (int32_t k = 0; k <= rule.value->intervals && !repainted; ++k)
        for (int32_t i = 0; i <= rule.value->intervals && !repainted; ++i)
            repainted = composited.At(sandLayer, i, k) > 1.0f - 1.0f / 255.0f &&
                        rule.value->At(1, i, k) < 1.0f - 1.0f / 255.0f;
    CHECK(repainted);
}

TEST_CASE("Painting keeps the base weights key and a recipe change replaces the cache",
          "[terraintool][sync][baseweights]")
{
    WeightCacheTree tree("baseweights_key");
    const TerrainRecipe recipe = RecipeOf(tree.recipe);
    TerrainEditLayer paint = PaintLayer(recipe.layout, 1.0f);
    WriteEditLayers(tree.recipe, std::span(&paint, 1));
    REQUIRE(tree.Sync().Ok());

    const TerrainBaseWeightsResult<TerrainBaseWeights> first = TerrainBaseWeights::Open(tree.releases, tree.asset);
    REQUIRE(first);
    const TerrainSourceFingerprint key = first.value->Key();
    const fs::path keyDir = tree.KeyDir(key);
    REQUIRE(WriteText(keyDir / "marker.txt", "keep"));

    paint = PaintLayer(recipe.layout, 0.5f);
    WriteEditLayers(tree.recipe, std::span(&paint, 1));
    const TerrainReport painted = tree.Sync();
    REQUIRE(painted.Ok());
    CHECK(painted.publish->published);
    CHECK(painted.Status(check::kBaseWeights) == CheckStatus::Passed);
    CHECK(ReadText(keyDir / "marker.txt") == "keep");
    const TerrainBaseWeightsResult<TerrainBaseWeights> kept = TerrainBaseWeights::Open(tree.releases, tree.asset);
    REQUIRE(kept);
    CHECK(kept.value->Key() == key);

    SetSandMinimum(tree.recipe, 12.0);
    const TerrainReport ruled = tree.Sync();
    REQUIRE(ruled.Ok());
    CHECK(ruled.publish->published);
    const TerrainBaseWeightsResult<TerrainBaseWeights> next = TerrainBaseWeights::Open(tree.releases, tree.asset);
    REQUIRE(next);
    CHECK(next.value->Key() != key);
    CHECK_FALSE(fs::exists(keyDir));
    const TerrainSourceFingerprint ruleKey = next.value->Key();

    TerrainEditLayer raise = AdditiveEditLayer("raise", 1);
    SetTerrainEditSample(raise, recipe.layout, {recipe.layout.TileFirstSample(0) + 1, recipe.layout.TileFirstSample(0) + 1},
                         4.0f);
    const TerrainEditLayer layers[] = {paint, raise};
    WriteEditLayers(tree.recipe, layers);
    const TerrainReport raised = tree.Sync();
    REQUIRE(raised.Ok());
    CHECK(raised.publish->published);
    const TerrainBaseWeightsResult<TerrainBaseWeights> heightChanged =
        TerrainBaseWeights::Open(tree.releases, tree.asset);
    REQUIRE(heightChanged);
    CHECK(heightChanged.value->Key() != ruleKey);
    CHECK_FALSE(fs::exists(tree.KeyDir(ruleKey)));

    const TerrainSourceFingerprint finalKey = heightChanged.value->Key();
    fs::remove_all(tree.BaseRoot());
    const TerrainReport rebuilt = tree.Sync();
    REQUIRE(rebuilt.Ok());
    CHECK_FALSE(rebuilt.publish->published);
    CHECK(rebuilt.Status(check::kBaseWeights) == CheckStatus::Passed);
    const TerrainBaseWeightsResult<TerrainBaseWeights> rewritten = TerrainBaseWeights::Open(tree.releases, tree.asset);
    INFO(rewritten.error.message);
    REQUIRE(rewritten);
    CHECK(rewritten.value->Key() == finalKey);
    const TerrainBaseWeightsResult<TerrainWeightTile> tile = rewritten.value->ReadTile(recipe.layout.Tiles().min);
    REQUIRE(tile);
}

TEST_CASE("A recipe without materials writes no base weights", "[terraintool][sync][baseweights]")
{
    WeightCacheTree tree("baseweights_none");
    REQUIRE(fs::copy_file(ExampleRecipePath(), tree.recipe, fs::copy_options::overwrite_existing));

    const TerrainReport report = tree.Sync();
    REQUIRE(report.Ok());
    CHECK(report.publish->published);
    CHECK(report.Status(check::kBaseWeights) == CheckStatus::Skipped);
    CHECK_FALSE(fs::exists(tree.BaseRoot()));
}

TEST_CASE("Prune and package ignore the base weights cache", "[terraintool][sync][baseweights][prune]")
{
    WeightCacheTree tree("baseweights_ignore");
    REQUIRE(tree.Sync().Ok());
    const TerrainBaseWeightsResult<TerrainBaseWeights> base = TerrainBaseWeights::Open(tree.releases, tree.asset);
    REQUIRE(base);
    const fs::path keyDir = tree.KeyDir(base.value->Key());
    CHECK(ListTerrainReleases(tree.releases, tree.asset).size() == 1);

    TerrainPruneRequest prune;
    prune.idRoot = std::string(kIdRoot);
    prune.sourcePath = std::string(kSourcePath);
    prune.gameDir = tree.gameDir;
    const TerrainReport pruned = RunTerrainPrune(prune);
    for (const TerrainDiagnostic &d : pruned.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(pruned.Ok());
    CHECK(fs::is_regular_file(TerrainBaseWeightsPointerPath(tree.releases, tree.asset)));
    CHECK(fs::is_regular_file(keyDir / kTerrainBaseWeightsIndexFileName));
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
    CHECK_FALSE(fs::exists(outAssetDir / std::string(kTerrainBaseWeightsDirName)));
    CHECK(Snapshot(tree.releases) == sourceTree);
}
