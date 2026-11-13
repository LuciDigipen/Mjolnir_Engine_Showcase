/**
 * @file TerrainBakeWeights.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool material weight bakes, sync, validation and packaging.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainBake.h>
#include <TerrainFinalSurface.h>
#include <TerrainMaterial.h>
#include <TerrainPackage.h>
#include <TerrainSync.h>
#include <TerrainValidate.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
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

    TerrainReport ValidateRelease(const fs::path &dir, const TerrainRecipe &recipe)
    {
        TerrainReport report("validate", {check::kCandidate, check::kFormat, check::kIndex, check::kPacks,
                                          check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                                          check::kCorners, check::kRegions, check::kIdentity});
        ValidateTerrainCandidate(dir, {&recipe, false, false}, report);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        return report;
    }

    void Reseal(std::vector<std::byte> &index)
    {
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));
    }

    using PayloadKey = std::tuple<int32_t, int32_t, uint32_t, uint32_t, uint32_t>;

    std::map<PayloadKey, std::vector<std::byte>> TilePayloads(const TerrainReleaseHandle &handle)
    {
        std::map<PayloadKey, std::vector<std::byte>> payloads;
        for (const TerrainTileRecord &record : handle.Index().tiles)
        {
            const TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(record);
            REQUIRE(bytes);
            payloads.emplace(PayloadKey{record.tile.x, record.tile.z, record.layer,
                                        static_cast<uint32_t>(record.kind), record.level},
                             std::move(*bytes.value));
        }
        for (const TerrainTileRecord &record : handle.Index().regions)
        {
            const TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(record);
            REQUIRE(bytes);
            payloads.emplace(PayloadKey{record.tile.x, record.tile.z, record.layer,
                                        static_cast<uint32_t>(record.kind), record.level},
                             std::move(*bytes.value));
        }
        return payloads;
    }

    std::vector<std::vector<std::byte>> WeightPayloads(const TerrainReleaseHandle &handle)
    {
        std::vector<std::vector<std::byte>> payloads;
        const TerrainIndex &index = handle.Index();
        for (const TerrainWeightRecord &record : index.weights)
        {
            const std::string pack = TerrainWeightPackFileName(index.weightPacks[record.pack].chunk);
            const std::optional<std::vector<std::byte>> bytes =
                handle.Source().ReadRange(pack, record.offset, static_cast<size_t>(record.length));
            REQUIRE(bytes);
            payloads.push_back(std::move(*bytes));
        }
        return payloads;
    }

    struct WeightTree
    {
        ScratchDir scratch;
        fs::path recipeDir = scratch.path / "assets" / "terrain";
        fs::path recipe = WriteMaterialRecipe(recipeDir, Materials());
        fs::path gameDir = scratch.path / "game";
        fs::path releases = TerrainReleasesRoot(gameDir);
        AssetId asset = MakeAssetId(ExampleAssetPath());

        explicit WeightTree(std::string_view tag) : scratch(tag) {}

        TerrainSyncRequest Sync(std::string_view build = "build") const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.gameDir = gameDir;
            request.candidateDir = scratch.path / std::string(build) / "terrain" / "relief-2x2";
            return request;
        }

        TerrainBakeRequest Bake(const fs::path &out, std::optional<uint64_t> nonce = kNonce) const
        {
            TerrainBakeRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.candidateDir = out;
            request.options.nonce = nonce;
            return request;
        }
    };

    TerrainReport Sync(const TerrainSyncRequest &request)
    {
        TerrainReport report = RunTerrainSync(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        for (const TerrainCheck &c : report.checks)
            UNSCOPED_INFO(c.name << ": " << CheckStatusName(c.status) << " " << c.detail);
        return report;
    }

    size_t SourceTileCount(const TerrainIndex &index)
    {
        return static_cast<size_t>(std::count_if(index.tiles.begin(), index.tiles.end(),
                                                 [](const TerrainTileRecord &record)
                                                 {
                                                     return record.source;
                                                 }));
    }
}

TEST_CASE("A recipe with materials publishes rule weights and edit layers in every source tile",
          "[terraintool][weights]")
{
    WeightTree tree("weights_bake");
    TerrainBakeRequest request = tree.Bake(tree.scratch.path / "candidate");
    const TerrainReport report = RunTerrainBake(request);
    for (const TerrainDiagnostic &d : report.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(report.Ok());

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(request.candidateDir));
    REQUIRE(opened);
    const TerrainIndex &index = opened.value->Index();
    const TerrainRecipe recipe = RecipeOf(tree.recipe);
    REQUIRE_FALSE(recipe.materials.empty());

    CHECK(index.formatMinor == kTerrainWeightsFormatMinor);
    CHECK(index.weightLayers == Names{"base", "sand"});
    CHECK(index.weightsFingerprint == FingerprintWeights(recipe, FingerprintRecipe(recipe)).value);
    CHECK(index.weights.size() == SourceTileCount(index));
    for (const TerrainTileRecord &record : index.tiles)
        if (record.source)
            CHECK(index.FindWeight(record.tile) != nullptr);
    CHECK(opened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);

    const TileCoord tile{-1, -1};
    const TerrainWeightRecord *record = index.FindWeight(tile);
    REQUIRE(record);
    const std::string pack = TerrainWeightPackFileName(index.weightPacks[record->pack].chunk);
    const std::optional<std::vector<std::byte>> bytes =
        opened.value->Source().ReadRange(pack, record->offset, static_cast<size_t>(record->length));
    REQUIRE(bytes);
    const TerrainResult<TerrainDecodedWeights> decoded = DecodeTerrainWeightPayload(index, *record, *bytes);
    REQUIRE(decoded);
    const TerrainWeightTile expanded = ExpandTerrainWeights(*decoded.value);

    std::vector<TileHeights> sources;
    for (const TerrainTileRecord &source : index.tiles)
        if (source.source && source.kind == TerrainPayloadKind::Height)
        {
            const TerrainResult<TileHeights> read = opened.value->ReadTileRecord(source);
            REQUIRE(read);
            sources.push_back(std::move(*read.value));
        }
    std::map<std::pair<int32_t, int32_t>, const TileHeights *> byTile;
    for (const TileHeights &source : sources)
        byTile[{source.Tile().z, source.Tile().x}] = &source;
    const TerrainFinalSurface surface{recipe, byTile};
    const std::optional<TerrainWeightTile> expected = EvaluateTerrainWeights(recipe, surface, tile);
    REQUIRE(expected);
    REQUIRE(expanded.layers == expected->layers);
    for (size_t layer = 0; layer < expected->layers.size(); ++layer)
        for (int32_t k = 0; k <= expected->intervals; ++k)
            for (int32_t i = 0; i <= expected->intervals; ++i)
                CHECK(std::abs(expanded.At(layer, i, k) - expected->At(layer, i, k)) <= 1.0 / 255.0 + 1e-6);
}

TEST_CASE("A recipe without materials keeps its releases free of weight payloads", "[terraintool][weights]")
{
    WeightTree tree("weights_none");
    const fs::path plain = tree.scratch.path / "plain";
    const TerrainBakeRequest plainRequest = ExampleRequest(plain, kNonce);
    const TerrainReport plainReport = RunTerrainBake(plainRequest);
    for (const TerrainDiagnostic &d : plainReport.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(plainReport.Ok());

    const fs::path weighted = tree.scratch.path / "weighted";
    const TerrainReport weightedReport = RunTerrainBake(tree.Bake(weighted, kNonce));
    REQUIRE(weightedReport.Ok());

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(plain));
    REQUIRE(opened);
    const TerrainIndex &index = opened.value->Index();
    CHECK(index.formatMinor == kTerrainFormatMinor);
    CHECK(index.weights.empty());
    CHECK(index.weightLayers.empty());
    CHECK(index.weightPacks.empty());
    CHECK(index.weightsFingerprint == 0);
    for (const std::string &name : FileNames(plain))
        CHECK(name.ends_with(".wpack") == false);

    const std::map<std::string, std::vector<std::byte>> plainFiles = Snapshot(plain);
    const std::map<std::string, std::vector<std::byte>> weightedFiles = Snapshot(weighted);
    size_t packs = 0;
    for (const auto &[name, bytes] : plainFiles)
        if (name.ends_with(".tpack"))
        {
            ++packs;
            REQUIRE(weightedFiles.contains(name));
            CHECK(bytes == weightedFiles.at(name));
        }
    CHECK(packs > 0);
    CHECK(std::any_of(weightedFiles.begin(), weightedFiles.end(),
                      [](const auto &file)
                      {
                          return file.first.ends_with(".wpack");
                      }));
}

TEST_CASE("A weight-only change reuses the height payloads and publishes a new release",
          "[terraintool][weights][sync]")
{
    WeightTree tree("weights_sync");
    const TerrainReport first = Sync(tree.Sync());
    REQUIRE(first.Ok());
    REQUIRE(first.publish->published);
    const TerrainReleaseId r1 = *first.publish->current;
    const TerrainResult<TerrainReleaseHandle> h1 = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(h1);
    const std::map<PayloadKey, std::vector<std::byte>> heightPayloads = TilePayloads(*h1.value);
    const std::vector<std::vector<std::byte>> weightPayloads = WeightPayloads(*h1.value);
    const uint64_t heightsHash = h1.value->Index().heightsHash;
    const uint64_t weightsFingerprint = h1.value->Index().weightsFingerprint;

    SetSandMinimum(tree.recipe, 12.0);
    const TerrainReport second = Sync(tree.Sync());
    REQUIRE(second.Ok());
    CHECK(second.publish->published);
    CHECK(second.publish->previous == r1);
    const TerrainReleaseId r2 = *second.publish->current;
    CHECK(r2 != r1);
    const TerrainResult<TerrainReleaseHandle> h2 = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(h2);
    CHECK(h2.value->Index().heightsHash == heightsHash);
    CHECK(h2.value->Index().weightsFingerprint != weightsFingerprint);
    CHECK(h2.value->Index().weightLayers == Names{"base", "sand"});
    CHECK(WeightPayloads(*h2.value) != weightPayloads);
    CHECK(TilePayloads(*h2.value) == heightPayloads);

    const TerrainReport settled = Sync(tree.Sync());
    REQUIRE(settled.Ok());
    CHECK_FALSE(settled.publish->published);
    CHECK(settled.publish->current == r2);

    const TerrainLayout layout = h2.value->Layout();
    TerrainEditLayer layer = AdditiveEditLayer("paint");
    TerrainEditTile paint;
    paint.tile = {-1, -1};
    paint.intervals = layout.IntervalsPerTile();
    const size_t count = static_cast<size_t>(paint.intervals + 1) * static_cast<size_t>(paint.intervals + 1);
    paint.weights.push_back({"sand", std::vector<float>(count, 1.0f)});
    layer.tiles.emplace(paint.tile, std::move(paint));
    WriteEditLayers(tree.recipe, std::span(&layer, 1));

    const std::vector<std::vector<std::byte>> paintedWeights = WeightPayloads(*h2.value);
    const TerrainReport third = Sync(tree.Sync());
    REQUIRE(third.Ok());
    CHECK(third.publish->published);
    const TerrainResult<TerrainReleaseHandle> h3 = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(h3);
    CHECK(h3.value->Index().heightsHash == heightsHash);
    CHECK(h3.value->Index().weightsFingerprint != h2.value->Index().weightsFingerprint);
    CHECK(WeightPayloads(*h3.value) != paintedWeights);
    CHECK(TilePayloads(*h3.value) == heightPayloads);

    TerrainEditLayer unknown = AdditiveEditLayer("unknown");
    TerrainEditTile channel;
    channel.tile = {-1, -1};
    channel.intervals = layout.IntervalsPerTile();
    channel.weights.push_back({"nope", std::vector<float>(count, 1.0f)});
    unknown.tiles.emplace(channel.tile, std::move(channel));
    WriteEditLayers(tree.recipe, std::span(&unknown, 1));

    const TerrainReport fourth = Sync(tree.Sync());
    REQUIRE(fourth.Ok());
    CHECK(fourth.publish->published);
    size_t warnings = 0;
    for (const TerrainDiagnostic &warning : fourth.warnings)
        if (warning.check == check::kEncode && warning.field == "editWeights")
        {
            ++warnings;
            CHECK(warning.message.find("nope") != std::string::npos);
        }
    CHECK(warnings == 1);
}

TEST_CASE("A weight-only bake with the same release identity keeps every height pack byte for byte",
          "[terraintool][weights][incremental]")
{
    WeightTree tree("weights_packs");
    const fs::path firstDir = tree.scratch.path / "first";
    const TerrainReport first = RunTerrainBake(tree.Bake(firstDir));
    REQUIRE(first.Ok());
    REQUIRE(first.release);

    SetSandMinimum(tree.recipe, 12.0);
    const fs::path secondDir = tree.scratch.path / "second";
    const TerrainReport second = RunTerrainBake(tree.Bake(secondDir));
    REQUIRE(second.Ok());
    REQUIRE(second.release);
    CHECK(second.release->release == first.release->release);
    CHECK(second.heightsHash == first.heightsHash);

    const std::map<std::string, std::vector<std::byte>> firstFiles = Snapshot(firstDir);
    const std::map<std::string, std::vector<std::byte>> secondFiles = Snapshot(secondDir);
    size_t packs = 0;
    for (const auto &[name, bytes] : firstFiles)
        if (name.ends_with(".tpack"))
        {
            ++packs;
            REQUIRE(secondFiles.contains(name));
            CHECK(secondFiles.at(name) == bytes);
        }
    size_t weightPacks = 0;
    size_t changedWeightPacks = 0;
    for (const auto &[name, bytes] : firstFiles)
        if (name.ends_with(".wpack"))
        {
            ++weightPacks;
            REQUIRE(secondFiles.contains(name));
            if (secondFiles.at(name) != bytes)
                ++changedWeightPacks;
        }
    CHECK(packs > 0);
    CHECK(weightPacks > 0);
    CHECK(changedWeightPacks > 0);
    CHECK(secondFiles.at(std::string(kTerrainIndexFileName)) != firstFiles.at(std::string(kTerrainIndexFileName)));
}

TEST_CASE("Validate and package handle weight packs", "[terraintool][weights][validate][package]")
{
    WeightTree tree("weights_validate");
    const TerrainReport synced = Sync(tree.Sync());
    REQUIRE(synced.Ok());
    REQUIRE(synced.publish->current);
    const TerrainReleaseId release = *synced.publish->current;
    const fs::path releaseDir = TerrainReleaseDir(tree.releases, tree.asset, release);
    const TerrainRecipe recipe = RecipeOf(tree.recipe);

    const TerrainReport valid = ValidateRelease(releaseDir, recipe);
    CHECK(valid.Ok());
    CHECK(valid.Status(check::kPacks) == CheckStatus::Passed);
    CHECK(valid.Status(check::kIdentity) == CheckStatus::Passed);

    const std::map<std::string, std::vector<std::byte>> files = Snapshot(releaseDir);
    auto copy = [&](std::string_view tag)
    {
        const fs::path dir = tree.scratch.path / std::string(tag);
        fs::create_directories(dir);
        for (const auto &[name, bytes] : files)
            WriteFile(dir / name, bytes);
        return dir;
    };

    SECTION("a corrupt weight pack fails the packs check by name")
    {
        const fs::path dir = copy("corrupt");
        const TerrainResult<TerrainReleaseHandle> opened =
            TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(dir));
        REQUIRE(opened);
        const TerrainIndex &index = opened.value->Index();
        REQUIRE_FALSE(index.weights.empty());
        const TerrainWeightRecord &record = index.weights.front();
        const std::string pack = TerrainWeightPackFileName(index.weightPacks[record.pack].chunk);
        std::vector<std::byte> bytes = ReadFile(dir / pack);
        bytes[static_cast<size_t>(record.offset)] ^= std::byte{0x01};
        WriteFile(dir / pack, bytes);

        const TerrainReport report = ValidateRelease(dir, recipe);
        CHECK(report.Status(check::kPacks) == CheckStatus::Failed);
        const std::vector<TerrainDiagnostic> diagnostics = DiagnosticsOf(report, check::kPacks);
        REQUIRE_FALSE(diagnostics.empty());
        CHECK(diagnostics.front().message.find(".wpack") != std::string::npos);
    }

    SECTION("a weight layer table that differs from the recipe fails identity")
    {
        const fs::path dir = copy("layers");
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(index);
        REQUIRE(parsed);
        const TerrainIndex &read = *parsed.value;
        const size_t layerTable = kTerrainWeightsIndexHeaderSize + read.packs.size() * kTerrainPackRecordSize +
                                  read.tiles.size() * kTerrainTileRecordSize +
                                  read.layers.size() * kTerrainLayerRecordSize +
                                  read.regions.size() * kTerrainTileRecordSize +
                                  read.weightPacks.size() * kTerrainPackRecordSize +
                                  read.weights.size() * kTerrainWeightRecordSize;
        const std::string sand = "sand";
        const auto at = std::search(index.begin() + layerTable, index.end(), sand.begin(), sand.end(),
                                    [](std::byte byte, char character)
                                    {
                                        return byte == static_cast<std::byte>(character);
                                    });
        REQUIRE(at != index.end());
        const std::string rock = "rock";
        std::transform(rock.begin(), rock.end(), at,
                       [](char character)
                       {
                           return static_cast<std::byte>(character);
                       });
        Reseal(index);
        WriteFile(dir / kTerrainIndexFileName, index);

        const TerrainReport report = ValidateRelease(dir, recipe);
        bool named = false;
        for (const TerrainDiagnostic &diagnostic : DiagnosticsOf(report, check::kIdentity))
            named = named || diagnostic.field == "weight.layers";
        CHECK(report.Status(check::kIdentity) == CheckStatus::Failed);
        CHECK(named);
    }

    SECTION("a differing weight fingerprint fails identity")
    {
        const fs::path dir = copy("fingerprint");
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(index);
        REQUIRE(parsed);
        StoreU64(index, terrainformat::index::kWeightsFingerprint, parsed.value->weightsFingerprint ^ 1);
        Reseal(index);
        WriteFile(dir / kTerrainIndexFileName, index);

        const TerrainReport report = ValidateRelease(dir, recipe);
        bool named = false;
        for (const TerrainDiagnostic &diagnostic : DiagnosticsOf(report, check::kIdentity))
            named = named || diagnostic.field == "weight.fingerprint";
        CHECK(report.Status(check::kIdentity) == CheckStatus::Failed);
        CHECK(named);
    }

    SECTION("package copies the weight packs and the copy verifies")
    {
        const fs::path outGame = tree.scratch.path / "packaged-game";
        TerrainPackageRequest request;
        request.idRoot = std::string(kIdRoot);
        request.sourcePath = std::string(kSourcePath);
        request.gameDir = tree.gameDir;
        request.outDir = outGame;
        const TerrainReport packaged = RunTerrainPackage(request);
        for (const TerrainDiagnostic &d : packaged.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(packaged.Ok());
        CHECK(packaged.publish->published);

        const fs::path outReleases = TerrainReleasesRoot(outGame);
        const fs::path copied = TerrainReleaseDir(outReleases, tree.asset, release);
        size_t weightPacks = 0;
        for (const std::string &name : FileNames(copied))
            if (name.ends_with(".wpack"))
                ++weightPacks;
        CHECK(weightPacks > 0);

        const TerrainResult<TerrainReleaseHandle> opened = OpenCurrentTerrainRelease(outReleases, tree.asset);
        REQUIRE(opened);
        CHECK(opened.value->Info().release == release);
        CHECK(opened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
        CHECK(ValidateRelease(copied, recipe).Ok());
        CHECK(ReadFile(copied / kTerrainIndexFileName) == files.at(std::string(kTerrainIndexFileName)));
    }
}
