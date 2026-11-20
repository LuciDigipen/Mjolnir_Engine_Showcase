/**
 * @file TerrainBake.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainBake.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainBake.h>
#include <TerrainCandidate.h>
#include <TerrainGenerator.h>
#include <TerrainValidate.h>

#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    // Final = source in P1, so this follows the generator's pinned hash.
    constexpr uint64_t kExampleHeightsHash = 0x4dcd42e8001dec6eull;

    void CheckAllPassed(const TerrainReport &report)
    {
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        for (const TerrainCheck &c : report.checks)
        {
            INFO(c.name << ": " << c.detail);
            CHECK(c.status == CheckStatus::Passed);
        }
        CHECK(report.Ok());
    }
}

TEST_CASE("Bake final heights are the generator's source heights, tile by tile", "[terraintool][bake]")
{
    const TerrainRecipe recipe = LoadExample();
    TerrainReport report("bake", {check::kGenerate});
    const std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
    REQUIRE(tiles);
    CHECK(report.Status(check::kGenerate) == CheckStatus::Passed);

    const TerrainGenerator generator(recipe);
    const std::vector<TileCoord> order{{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
    REQUIRE(tiles->size() == order.size());
    for (size_t t = 0; t < order.size(); ++t)
    {
        const TileHeights &tile = (*tiles)[t];
        CHECK(tile.Tile() == order[t]);
        CHECK(tile.IntervalsPerTile() == 64);
        std::vector<float> source(TileSampleCount(recipe.layout));
        REQUIRE(generator.FillTile(order[t], source).Ok());
        CHECK(std::equal(source.begin(), source.end(), tile.Samples().begin(), tile.Samples().end(),
                         [](float a, float b)
                         {
                             return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
                         }));
    }
    CHECK(TerrainHeightsHash(recipe.layout, *tiles) == kExampleHeightsHash);
}

TEST_CASE("A bake mints its release id once from the final heights and a nonce", "[terraintool][bake]")
{
    const TerrainRecipe recipe = LoadExample();
    const AssetId asset = MakeAssetId(ExampleAssetPath());
    const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
    TerrainReport report("bake", {check::kGenerate, check::kEncode});
    auto bake = [&](std::optional<uint64_t> nonce)
    {
        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        REQUIRE(tiles);
        std::optional<TerrainBakedRelease> baked =
            EncodeBakedRelease(asset, fingerprint, recipe.layout, std::move(*tiles), {nonce, {}}, report);
        REQUIRE(baked);
        return std::move(*baked);
    };

    const TerrainBakedRelease a = bake(kNonce);
    const TerrainBakedRelease b = bake(kNonce);
    const TerrainBakedRelease c = bake(std::nullopt);
    const TerrainBakedRelease d = bake(std::nullopt);
    CHECK(report.Ok());

    CHECK(a.info.release == MintTerrainReleaseId(asset, fingerprint, a.encoded.heightsHash, kNonce));
    CHECK(a.info.asset == asset);
    CHECK(a.info.source == fingerprint);
    CHECK(a.info.format == kTerrainFormatVersion);
    CHECK(a.encoded.index == b.encoded.index);
    REQUIRE(a.encoded.packs.size() == b.encoded.packs.size());
    for (size_t p = 0; p < a.encoded.packs.size(); ++p)
        CHECK(a.encoded.packs[p].bytes == b.encoded.packs[p].bytes);
    CHECK(c.encoded.heightsHash == a.encoded.heightsHash);
    CHECK(d.encoded.heightsHash == a.encoded.heightsHash);
    CHECK(c.info.release != a.info.release);
    CHECK(c.info.release != d.info.release);

    const TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(c.encoded));
    REQUIRE(opened);
    CHECK(opened.value->Info() == c.info);
    CHECK(opened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
    // Tiles straddling the origin fall in four chunks: one pack per tile.
    CHECK(a.encoded.packs.size() == 4);
}

TEST_CASE("A bake writes exactly the release files and a manifest, then nothing else", "[terraintool][bake]")
{
    ScratchDir scratch("bake");
    const fs::path out = scratch.path / "candidate";
    const TerrainReport report = RunTerrainBake(ExampleRequest(out));
    CheckAllPassed(report);

    CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    CHECK(FileNames(out) == std::vector<std::string>{"candidate.json", "chunk_-1_-1.tpack", "chunk_-1_0.tpack",
                                                     "chunk_0_-1.tpack", "chunk_0_0.tpack", "release.tindex"});

    REQUIRE(report.release);
    CHECK(report.release->asset == MakeAssetId(ExampleAssetPath()));
    CHECK(report.assetPath == ExampleAssetPath());
    CHECK(report.nonce == kNonce);
    CHECK(report.surfaceVersion == kTerrainSurfaceVersion);
    CHECK(report.candidateDir == out);
    REQUIRE(report.tiles.size() == 24);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(),
                        [](const auto &t)
                        {
                            return t.source;
                        }) == 4);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(),
                        [](const auto &t)
                        {
                            return t.kind == 1;
                        }) == 8);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(),
                        [](const auto &t)
                        {
                            return t.kind == 2;
                        }) == 8);
    for (const TerrainTileSummary &tile : report.tiles)
        CHECK(tile.readable);
    REQUIRE(report.files.size() == 5);
    CHECK(report.files[0].name == "release.tindex");

    const std::optional<std::string> text = ReadText(out / kCandidateManifestFileName);
    REQUIRE(text);
    const ManifestParseResult manifest = ParseCandidateManifest(*text);
    REQUIRE(manifest.manifest);
    CHECK(manifest.manifest->release == *report.release);
    CHECK(manifest.manifest->assetPath == ExampleAssetPath());
    CHECK(manifest.manifest->heightsHash == report.heightsHash);
    CHECK(manifest.manifest->surfaceVersion == kTerrainSurfaceVersion);
    REQUIRE(manifest.manifest->files.size() == report.files.size());
    for (size_t f = 0; f < report.files.size(); ++f)
    {
        CHECK(manifest.manifest->files[f].name == report.files[f].name);
        CHECK(manifest.manifest->files[f].bytes == report.files[f].bytes);
        CHECK(manifest.manifest->files[f].checksum == report.files[f].checksum);
    }

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(out));
    REQUIRE(opened);
    CHECK(opened.value->Info() == *report.release);
    CHECK(opened.value->Index().heightsHash == report.heightsHash);
    std::vector<TileHeights> sources;
    for (const auto &record : opened.value->Index().tiles)
        if (record.source)
        {
            auto tile = opened.value->ReadTileRecord(record);
            REQUIRE(tile);
            sources.push_back(std::move(*tile.value));
        }
    CHECK(TerrainHeightsHash(LoadExample().layout, sources) == kExampleHeightsHash);
}

TEST_CASE("A bake refuses an existing non-empty directory and leaves it as it was", "[terraintool][bake]")
{
    ScratchDir scratch("bake_refuse");
    const fs::path out = scratch.path / "candidate";
    fs::create_directories(out);
    const std::vector<std::byte> keep{std::byte{1}, std::byte{2}};
    WriteFile(out / "keep.bin", keep);

    const TerrainReport refused = RunTerrainBake(ExampleRequest(out));
    CHECK_FALSE(refused.Ok());
    const std::vector<TerrainDiagnostic> args = DiagnosticsOf(refused, check::kArguments);
    REQUIRE(args.size() == 1);
    CHECK(args[0].field == "--out");
    CHECK(refused.Status(check::kRecipe) == CheckStatus::Skipped);
    CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    CHECK(FileNames(out) == std::vector<std::string>{"keep.bin"});
    CHECK(ReadFile(out / "keep.bin") == keep);

    SECTION("an empty directory is filled")
    {
        fs::remove(out / "keep.bin");
        CheckAllPassed(RunTerrainBake(ExampleRequest(out)));
        CHECK(fs::exists(out / kCandidateManifestFileName));
        CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    }
    SECTION("a second bake into a finished candidate is refused")
    {
        const fs::path second = scratch.path / "second";
        CheckAllPassed(RunTerrainBake(ExampleRequest(second)));
        const auto before = Snapshot(second);
        CHECK_FALSE(RunTerrainBake(ExampleRequest(second, std::nullopt)).Ok());
        CHECK(Snapshot(second) == before);
    }
}

TEST_CASE("A bake under a non-ASCII path writes UTF-8 paths and leaves no partial directory", "[terraintool][bake]")
{
    ScratchDir scratch("bake_utf8");
    // CJK and Latin-1 letters: no ANSI code page holds both, and neither is ASCII.
    const fs::path dir = scratch.path / fs::path(u8"\u5730\u5f62-\u00fc");
    fs::create_directories(dir);
    const fs::path recipe = dir / fs::path(u8"relief-\u00e9.terrain.json");
    fs::copy_file(ExampleRecipePath(), recipe);
    const fs::path out = dir / fs::path(u8"\u5019\u9009");

    TerrainBakeRequest request = ExampleRequest(out);
    request.recipePath = recipe;
    const TerrainReport report = RunTerrainBake(request);
    CheckAllPassed(report);
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 2);

    const std::optional<std::string> text = ReadText(out / kCandidateManifestFileName);
    REQUIRE(text);
    const ManifestParseResult manifest = ParseCandidateManifest(*text);
    REQUIRE(manifest.manifest);
    CHECK(manifest.manifest->recipePath == PathUtf8(recipe));
    const Json json = Json::parse(TerrainReportJson(report));
    CHECK(json.at("candidate").get<std::string>() == PathUtf8(out));
    CHECK(json.at("recipe").at("path").get<std::string>() == PathUtf8(recipe));
}

TEST_CASE("An invalid recipe or asset path fails before anything is written", "[terraintool][bake]")
{
    ScratchDir scratch("bake_invalid");
    const fs::path out = scratch.path / "candidate";

    SECTION("recipe fields")
    {
        std::ifstream in(ExampleRecipePath());
        Json recipe = Json::parse(in);
        recipe["parameters"]["broad"]["amplitude"] = -1.0;
        recipe.erase("seed");
        const fs::path path = scratch.path / "bad.terrain.json";
        REQUIRE(WriteText(path, recipe.dump(2)));
        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = path;

        const TerrainReport report = RunTerrainBake(request);
        CHECK_FALSE(report.Ok());
        std::vector<std::string> fields;
        for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kRecipe))
            fields.push_back(d.field);
        std::sort(fields.begin(), fields.end());
        CHECK(fields == std::vector<std::string>{"parameters.broad.amplitude", "seed"});
        CHECK(report.Status(check::kGenerate) == CheckStatus::Skipped);
        CHECK(report.Status(check::kWrite) == CheckStatus::Skipped);
    }
    SECTION("missing recipe file")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = scratch.path / "absent.terrain.json";
        const TerrainReport report = RunTerrainBake(request);
        CHECK(report.Status(check::kRecipe) == CheckStatus::Failed);
    }
    SECTION("asset path that escapes its root")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.sourcePath = "../outside.terrain.json";
        const std::vector<TerrainDiagnostic> args = DiagnosticsOf(RunTerrainBake(request), check::kArguments);
        REQUIRE(args.size() == 1);
        CHECK(args[0].field == "--source-path");
    }
    SECTION("absolute id root")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.idRoot = "C:/games/G/assets";
        const std::vector<TerrainDiagnostic> args = DiagnosticsOf(RunTerrainBake(request), check::kArguments);
        REQUIRE(args.size() == 1);
        CHECK(args[0].field == "--id-root");
    }
    CHECK_FALSE(fs::exists(out));
    for (const std::string &name : FileNames(scratch.path))
        CHECK(name.find(".partial") == std::string::npos);
}

TEST_CASE("Edit layers are composited into the final heights before anything is derived",
          "[terraintool][bake][edits]")
{
    ScratchDir scratch("bake_edits");
    const fs::path recipePath = CopyExampleRecipe(scratch.path);
    const TerrainRecipe plain = LoadExample();
    const TerrainLayout &layout = plain.layout;

    const std::vector<std::pair<GlobalSampleIndex, float>> edits{
        {{layout.TileFirstSample(-1) + 5, layout.TileFirstSample(-1) + 7}, 3.5f},
        {{layout.TileFirstSample(0), layout.TileFirstSample(-1) + 3}, -2.25f},
        {{layout.TileFirstSample(0), layout.TileFirstSample(0)}, 1.75f}};
    TerrainEditLayer layer = AdditiveEditLayer("touch-up");
    for (const auto &[sample, value] : edits)
        SetTerrainEditSample(layer, layout, sample, value);
    REQUIRE(layer.tiles.size() == 4);
    WriteEditLayers(recipePath, std::span(&layer, 1));

    const RecipeParseResult loaded = LoadTerrainRecipe(recipePath);
    for (const RecipeIssue &issue : loaded.issues)
        UNSCOPED_INFO(issue.field << ": " << issue.message);
    REQUIRE(loaded.recipe);
    CHECK(loaded.recipe->editStackHash != 0);

    const fs::path out = scratch.path / "candidate";
    TerrainBakeRequest request = ExampleRequest(out);
    request.recipePath = recipePath;
    const TerrainReport report = RunTerrainBake(request);
    CheckAllPassed(report);
    CHECK(report.Status(check::kEdges) == CheckStatus::Passed);
    CHECK(report.Status(check::kCorners) == CheckStatus::Passed);
    REQUIRE(report.recipeFingerprint);
    CHECK(*report.recipeFingerprint != FingerprintRecipe(plain));
    for (const TerrainCheck &c : report.checks)
        if (c.name == check::kRecipe)
            CHECK(c.detail.find("edits:") != std::string::npos);

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(out));
    REQUIRE(opened);
    std::vector<TileHeights> baked;
    for (const TerrainTileRecord &record : opened.value->Index().tiles)
        if (record.source)
        {
            TerrainResult<TileHeights> tile = opened.value->ReadTileRecord(record);
            REQUIRE(tile);
            baked.push_back(std::move(*tile.value));
        }
    REQUIRE(baked.size() == 4);

    TerrainReport plainReport("plain", {check::kGenerate});
    std::optional<std::vector<TileHeights>> plainTiles = BakeFinalHeights(plain, plainReport);
    REQUIRE(plainTiles);
    for (const TileHeights &tile : baked)
    {
        const TileHeights &base = TileAt(*plainTiles, tile.Tile());
        const int64_t firstX = layout.TileFirstSample(tile.Tile().x);
        const int64_t firstZ = layout.TileFirstSample(tile.Tile().z);
        const int32_t n = tile.IntervalsPerTile();
        for (int32_t k = 0; k <= n; ++k)
            for (int32_t i = 0; i <= n; ++i)
            {
                float expected = base.At(i, k);
                for (const auto &[sample, value] : edits)
                    if (sample == GlobalSampleIndex{firstX + i, firstZ + k})
                        expected += value;
                CHECK(std::bit_cast<uint32_t>(tile.At(i, k)) == std::bit_cast<uint32_t>(expected));
            }
    }

    TerrainValidateRequest validate;
    validate.candidateDir = out;
    validate.recipePath = recipePath;
    const TerrainReport validated = RunTerrainValidate(validate);
    for (const TerrainDiagnostic &d : validated.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    CHECK(validated.Status(check::kEdges) == CheckStatus::Passed);
    CHECK(validated.Status(check::kCorners) == CheckStatus::Passed);
    CHECK(validated.Ok());
}

TEST_CASE("A layers directory with no enabled tile bakes exactly as the unedited recipe",
          "[terraintool][bake][edits]")
{
    ScratchDir scratch("bake_edits_disabled");
    const fs::path recipePath = CopyExampleRecipe(scratch.path);
    const TerrainRecipe plain = LoadExample();
    TerrainEditLayer layer = AdditiveEditLayer("off", 0, false);
    SetTerrainEditSample(layer, plain.layout,
                         {plain.layout.TileFirstSample(-1) + 6, plain.layout.TileFirstSample(-1) + 6}, 5.0f);
    REQUIRE_FALSE(layer.tiles.empty());
    WriteEditLayers(recipePath, std::span(&layer, 1));

    TerrainBakeRequest request = ExampleRequest(scratch.path / "candidate");
    request.recipePath = recipePath;
    const TerrainReport report = RunTerrainBake(request);
    CheckAllPassed(report);
    const TerrainReport plainReport = RunTerrainBake(ExampleRequest(scratch.path / "plain"));
    CheckAllPassed(plainReport);
    CHECK(report.recipeFingerprint == plainReport.recipeFingerprint);
    CHECK(report.heightsHash == plainReport.heightsHash);
    REQUIRE(report.release);
    REQUIRE(plainReport.release);
    CHECK(report.release->release == plainReport.release->release);
}

TEST_CASE("A bad edit layer store fails the recipe check and writes nothing", "[terraintool][bake][edits]")
{
    ScratchDir scratch("bake_edits_refuse");
    const fs::path out = scratch.path / "candidate";

    SECTION("a corrupt tile file")
    {
        const fs::path recipePath = CopyExampleRecipe(scratch.path);
        const TerrainRecipe plain = LoadExample();
        TerrainEditLayer layer = AdditiveEditLayer("touch-up");
        SetTerrainEditSample(layer, plain.layout,
                             {plain.layout.TileFirstSample(-1) + 3, plain.layout.TileFirstSample(-1) + 3}, 1.0f);
        WriteEditLayers(recipePath, std::span(&layer, 1));
        const fs::path tileFile = TerrainEditLayersDir(recipePath) /
                                  TerrainEditLayerDirName(layer.info.sequence) / TerrainEditTileFileName({-1, -1});
        const std::vector<std::byte> junk{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        WriteFile(tileFile, junk);

        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = recipePath;
        const TerrainReport report = RunTerrainBake(request);
        const std::vector<TerrainDiagnostic> issues = DiagnosticsOf(report, check::kRecipe);
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].field == "edits");
        CHECK(issues[0].message.find("tile_-1_-1.tedit") != std::string::npos);
        CHECK(report.Status(check::kGenerate) == CheckStatus::Skipped);
        CHECK(report.Status(check::kWrite) == CheckStatus::Skipped);
    }
    SECTION("a layer tile on a coarser source tile")
    {
        const fs::path recipePath = CopyRecipeFixture("mixed-2x2.terrain.json", scratch.path / "mixed");
        const RecipeParseResult mixed = LoadTerrainRecipe(recipePath);
        for (const RecipeIssue &issue : mixed.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(mixed.recipe);
        TerrainEditLayer layer = AdditiveEditLayer("coarse");
        SetTerrainEditSample(layer, mixed.recipe->layout,
                             {mixed.recipe->layout.TileFirstSample(0) + 2,
                              mixed.recipe->layout.TileFirstSample(-1) + 2},
                             1.0f);
        WriteEditLayers(recipePath, std::span(&layer, 1));

        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = recipePath;
        const TerrainReport report = RunTerrainBake(request);
        const std::vector<TerrainDiagnostic> issues = DiagnosticsOf(report, check::kRecipe);
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].field == "edits");
        CHECK(issues[0].message.find("(0, -1)") != std::string::npos);
        CHECK(report.Status(check::kGenerate) == CheckStatus::Skipped);
        CHECK(report.Status(check::kWrite) == CheckStatus::Skipped);
    }

    CHECK_FALSE(fs::exists(out));
    for (const std::string &name : FileNames(scratch.path))
        CHECK(name.find(".partial") == std::string::npos);
}
