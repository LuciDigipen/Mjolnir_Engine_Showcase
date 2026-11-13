/**
 * @file TerrainIncremental.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainIncremental.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainIncremental.h>
#include <TerrainSync.h>
#include <TerrainValidate.h>

#include <assets/AssetPath.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    enum class EditCase
    {
        Interior,
        SharedEdge,
        FourTileCorner
    };

    struct FixtureTree
    {
        ScratchDir scratch;
        std::string fixture;
        fs::path recipeDir;
        fs::path recipe;
        std::string sourcePath;
        AssetId asset;
        fs::path gameDir;
        fs::path releases;

        FixtureTree(std::string_view tag, std::string fixtureName)
            : scratch(tag), fixture(std::move(fixtureName)), recipeDir(scratch.path / "assets" / "terrain"),
              recipe(CopyRecipeFixture(fixture, recipeDir)), sourcePath("terrain/" + fixture),
              asset(MakeAssetId(MakeLogicalAssetPath(kIdRoot, sourcePath).path)), gameDir(scratch.path / "game"),
              releases(TerrainReleasesRoot(gameDir))
        {
        }

        TerrainBakeRequest Request(const fs::path &out, std::optional<uint64_t> nonce) const
        {
            TerrainBakeRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = sourcePath;
            request.candidateDir = out;
            request.options.nonce = nonce;
            return request;
        }

        TerrainSyncRequest Sync(const fs::path &candidate, std::optional<uint64_t> nonce) const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = sourcePath;
            request.gameDir = gameDir;
            request.candidateDir = candidate;
            request.options.nonce = nonce;
            return request;
        }
    };

    std::string CheckDetail(const TerrainReport &report, std::string_view name)
    {
        for (const TerrainCheck &check : report.checks)
            if (check.name == name)
                return check.detail;
        return {};
    }

    fs::path PublishRelease(const fs::path &candidate, const fs::path &releases, AssetId asset,
                            TerrainReleaseId release)
    {
        const fs::path dir = TerrainReleaseDir(releases, asset, release);
        fs::create_directories(dir);
        for (const fs::directory_entry &entry : fs::directory_iterator(candidate))
            if (entry.is_regular_file() && entry.path().filename() != kCandidateManifestFileName)
                fs::copy_file(entry.path(), dir / entry.path().filename(), fs::copy_options::overwrite_existing);
        REQUIRE(WriteText(TerrainCurrentPointerPath(releases, asset), EncodeTerrainCurrentPointer(release)));
        return dir;
    }

    std::map<std::string, std::vector<std::byte>> ReleaseFiles(const fs::path &dir)
    {
        std::map<std::string, std::vector<std::byte>> files = Snapshot(dir);
        files.erase(std::string(kCandidateManifestFileName));
        return files;
    }

    void SetTileSamples(TerrainEditLayer &layer, const TerrainRecipe &recipe, TileCoord tile,
                        std::initializer_list<std::pair<int32_t, int32_t>> points, float value)
    {
        const TerrainLayout layout = *recipe.SourceLayout(tile);
        TerrainEditTile &edit = layer.tiles[tile];
        edit.tile = tile;
        edit.intervals = layout.IntervalsPerTile();
        edit.values.assign(size_t(edit.intervals + 1) * size_t(edit.intervals + 1), 0.0f);
        for (const auto &[i, k] : points)
            edit.values[size_t(k) * size_t(edit.intervals + 1) + size_t(i)] = value;
    }

    TerrainEditLayer MakeEdit(const TerrainRecipe &recipe, bool mixed, EditCase edit)
    {
        TerrainEditLayer layer = AdditiveEditLayer("edit");
        if (!mixed)
        {
            const TerrainLayout &layout = recipe.layout;
            const int32_t n = layout.IntervalsPerTile();
            const int32_t first = layout.TileFirstSample(-1);
            GlobalSampleIndex sample = {first + n / 2, first + n / 2};
            if (edit == EditCase::SharedEdge)
                sample = {layout.TileFirstSample(0), first + n / 2};
            else if (edit == EditCase::FourTileCorner)
                sample = {layout.TileFirstSample(0), layout.TileFirstSample(0)};
            SetTerrainEditSample(layer, layout, sample, 4.0f);
            return layer;
        }

        if (edit == EditCase::Interior)
            SetTileSamples(layer, recipe, {-1, -1}, {{32, 32}}, 4.0f);
        else if (edit == EditCase::SharedEdge)
            SetTileSamples(layer, recipe, {-1, -1}, {{256, 200}}, 4.0f);
        else
            SetTileSamples(layer, recipe, {-1, -1}, {{256, 256}, {255, 255}}, 4.0f);
        return layer;
    }

    TerrainBakeRequest WithBase(TerrainBakeRequest request, const fs::path &releases, AssetId asset)
    {
        TerrainBaseRelease base;
        base.releasesRoot = releases;
        base.asset = asset;
        request.base = base;
        return request;
    }

    TerrainRecipe RecipeOf(const fs::path &path)
    {
        RecipeParseResult parsed = LoadTerrainRecipe(path);
        REQUIRE(parsed.recipe);
        return *parsed.recipe;
    }

    void PatchIndexAndReseal(const fs::path &dir, size_t offset, uint32_t value)
    {
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        StoreU32(index, offset, value);
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));
        WriteFile(dir / kTerrainIndexFileName, index);
    }

    // Reseals a derived payload so only the payloads check can notice the change.
    void PatchPayloadSealed(const fs::path &dir, TileCoord tile, uint32_t layer, TerrainPayloadKind kind)
    {
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(index);
        REQUIRE(parsed);
        const TerrainIndex &idx = *parsed.value;
        const TerrainTileRecord *found = idx.Find(tile, layer, kind);
        REQUIRE(found);
        const size_t record = size_t(found - idx.tiles.data());
        const TerrainTileRecord &target = idx.tiles[record];
        const std::string pack = TerrainPackFileName(idx.packs[target.pack].chunk);

        std::vector<std::byte> bytes = ReadFile(dir / pack);
        bytes[size_t(target.offset)] ^= std::byte{0x01};
        const uint64_t payload = Fnv1a64(
            std::span<const std::byte>(bytes).subspan(size_t(target.offset), size_t(target.length)));

        const size_t tileTable = kTerrainIndexHeaderSize + idx.packs.size() * kTerrainPackRecordSize;
        StoreU64(index, tileTable + record * kTerrainTileRecordSize + 32, payload);
        StoreU64(index, kTerrainIndexHeaderSize + target.pack * kTerrainPackRecordSize + 24, Fnv1a64(bytes));
        std::vector<TerrainTileRecord> records = idx.tiles;
        records[record].checksum = payload;
        StoreU64(index, 112, TerrainRecordsHeightsHash(records, idx.regions));
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));

        WriteFile(dir / pack, bytes);
        WriteFile(dir / kTerrainIndexFileName, index);
    }
}

TEST_CASE("Incremental bakes after Edit Layer changes are byte-identical to full bakes",
          "[terraintool][incremental]")
{
    for (const std::string fixture : {"relief-2x2.terrain.json", "mixed-2x2.terrain.json"})
    {
        INFO("fixture " << fixture);
        for (const EditCase edit : {EditCase::Interior, EditCase::SharedEdge, EditCase::FourTileCorner})
        {
            INFO("edit " << int(edit));
            FixtureTree tree("incremental_identity", fixture);
            const TerrainRecipe recipe = RecipeOf(tree.recipe);
            const bool mixed = fixture == "mixed-2x2.terrain.json";

            TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
            const TerrainReport baked = RunTerrainBake(base);
            for (const TerrainDiagnostic &d : baked.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            REQUIRE(baked.Ok());
            REQUIRE(baked.release);
            PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);

            const TerrainEditLayer layer = MakeEdit(recipe, mixed, edit);
            WriteEditLayers(tree.recipe, std::span(&layer, 1));

            TerrainBakeRequest incremental =
                WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
            const TerrainReport incrementalReport = RunTerrainBake(incremental);
            for (const TerrainDiagnostic &d : incrementalReport.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            REQUIRE(incrementalReport.Ok());

            const TerrainReport fullReport = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
            REQUIRE(fullReport.Ok());

            CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
            const std::string detail = CheckDetail(incrementalReport, check::kEncode);
            CHECK(detail.find("incremental") != std::string::npos);
            CHECK(detail.find("0 changed") == std::string::npos);
            CHECK(detail.find("rederived") != std::string::npos);
            CHECK(detail.find("reused") != std::string::npos);
            CHECK(CheckDetail(fullReport, check::kEncode).find("full bake") != std::string::npos);
        }
    }
}

TEST_CASE("An unchanged base supplies every payload and still matches a full bake", "[terraintool][incremental]")
{
    for (const std::string fixture : {"relief-2x2.terrain.json", "mixed-2x2.terrain.json"})
    {
        INFO("fixture " << fixture);
        FixtureTree tree("incremental_reuse", fixture);
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        const TerrainReport baked = RunTerrainBake(base);
        REQUIRE(baked.Ok());
        REQUIRE(baked.release);
        PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);

        TerrainBakeRequest incremental =
            WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
        const TerrainReport report = RunTerrainBake(incremental);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(report.Ok());
        const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
        REQUIRE(full.Ok());
        CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
        const std::string detail = CheckDetail(report, check::kEncode);
        CHECK(detail.find("0 changed") != std::string::npos);
        CHECK(detail.find("0 tiles rederived") != std::string::npos);
        CHECK(detail.find("4 reused") != std::string::npos);
    }
}

TEST_CASE("A weight-only Edit Layer reuses every height payload and keeps the release bytes",
          "[terraintool][incremental][weights]")
{
    FixtureTree tree("incremental_weights", "relief-2x2.terrain.json");
    const TerrainRecipe plain = RecipeOf(tree.recipe);

    TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
    const TerrainReport baked = RunTerrainBake(base);
    for (const TerrainDiagnostic &d : baked.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(baked.Ok());
    REQUIRE(baked.release);
    const fs::path baseDir = PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);

    TerrainEditLayer layer = AdditiveEditLayer("weights");
    TerrainEditTile tile;
    tile.tile = {-1, -1};
    tile.intervals = plain.layout.IntervalsPerTile();
    const size_t count = static_cast<size_t>(tile.intervals + 1) * static_cast<size_t>(tile.intervals + 1);
    tile.weights.push_back({"sand", std::vector<float>(count, 1.0f)});
    layer.tiles.emplace(tile.tile, std::move(tile));
    WriteEditLayers(tree.recipe, std::span(&layer, 1));

    const TerrainRecipe weighted = RecipeOf(tree.recipe);
    CHECK(weighted.editStackHash == 0);
    CHECK(weighted.editWeightStackHash != 0);
    CHECK(FingerprintRecipe(weighted) == FingerprintRecipe(plain));
    CHECK(FingerprintWeights(weighted, FingerprintRecipe(weighted)) !=
          FingerprintWeights(plain, FingerprintRecipe(plain)));

    TerrainBakeRequest incremental =
        WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
    const TerrainReport report = RunTerrainBake(incremental);
    for (const TerrainDiagnostic &d : report.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(report.Ok());
    const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
    REQUIRE(full.Ok());
    CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
    CHECK(ReleaseFiles(incremental.candidateDir) == ReleaseFiles(baseDir));
    const std::string detail = CheckDetail(report, check::kEncode);
    CHECK(detail.find("0 changed") != std::string::npos);
    CHECK(detail.find("0 tiles rederived") != std::string::npos);
    CHECK(detail.find("4 reused") != std::string::npos);
}

TEST_CASE("An unusable base falls back to a full bake with the reason in the report", "[terraintool][incremental]")
{
    const fs::path mixedRecipe = fs::path(TERRAIN_FIXTURE_DIR) / "mixed-2x2.terrain.json";
    const std::optional<std::string> mixedText = ReadText(mixedRecipe);
    REQUIRE(mixedText);

    auto fallback = [](FixtureTree &tree, const TerrainBakeRequest &base, const std::string &reason) {
        const TerrainReport baked = RunTerrainBake(base);
        for (const TerrainDiagnostic &d : baked.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(baked.Ok());
        REQUIRE(baked.release);
        PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);

        TerrainBakeRequest incremental =
            WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
        const TerrainReport report = RunTerrainBake(incremental);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(report.Ok());
        const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
        REQUIRE(full.Ok());
        CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
        const std::string detail = CheckDetail(report, check::kEncode);
        CHECK(detail.find("full bake because") != std::string::npos);
        CHECK(detail.find(reason) != std::string::npos);
    };

    SECTION("layout differs")
    {
        FixtureTree tree("incremental_layout", "relief-2x2.terrain.json");
        const fs::path baseRecipe = tree.recipeDir / "mixed-2x2.terrain.json";
        REQUIRE(fs::copy_file(mixedRecipe, baseRecipe));
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        base.recipePath = baseRecipe;
        fallback(tree, base, "layout");
    }
    SECTION("layer list differs")
    {
        FixtureTree tree("incremental_layers", "mixed-2x2.terrain.json");
        nlohmann::json json = nlohmann::json::parse(*mixedText);
        json["precision"]["tiles"][1]["spacing"] = 16.0;
        const fs::path baseRecipe = tree.scratch.path / "base-recipe" / "mixed-2x2.terrain.json";
        fs::create_directories(baseRecipe.parent_path());
        REQUIRE(WriteText(baseRecipe, json.dump(2)));
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        base.recipePath = baseRecipe;
        fallback(tree, base, "layer list");
    }
    SECTION("source spacing differs")
    {
        FixtureTree tree("incremental_sources", "mixed-2x2.terrain.json");
        nlohmann::json json = nlohmann::json::parse(*mixedText);
        json["precision"]["tiles"][2]["spacing"] = 4.0;
        const fs::path baseRecipe = tree.scratch.path / "base-recipe" / "mixed-2x2.terrain.json";
        fs::create_directories(baseRecipe.parent_path());
        REQUIRE(WriteText(baseRecipe, json.dump(2)));
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        base.recipePath = baseRecipe;
        fallback(tree, base, "source tiles or spacings");
    }
    SECTION("Region grid differs")
    {
        FixtureTree tree("incremental_regions", "mixed-2x2.terrain.json");
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        base.options.regions = {8, kTerrainRegionCells};
        fallback(tree, base, "Region grid");
        const TerrainResult<TerrainReleaseHandle> baseRelease =
            OpenCurrentTerrainRelease(tree.releases, tree.asset);
        const TerrainResult<TerrainReleaseHandle> fullRelease =
            TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(tree.scratch.path / "full"));
        REQUIRE(baseRelease);
        REQUIRE(fullRelease);
        CHECK(ReadAllTiles(*baseRelease.value) == ReadAllTiles(*fullRelease.value));
    }
    SECTION("format minor differs")
    {
        FixtureTree tree("incremental_minor", "mixed-2x2.terrain.json");
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        const TerrainReport baked = RunTerrainBake(base);
        REQUIRE(baked.Ok());
        REQUIRE(baked.release);
        const fs::path dir = PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);
        PatchIndexAndReseal(dir, 8, kTerrainFormatMinor + 1);

        TerrainBakeRequest incremental =
            WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
        const TerrainReport report = RunTerrainBake(incremental);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(report.Ok());
        const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
        REQUIRE(full.Ok());
        CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
        CHECK(CheckDetail(report, check::kEncode).find("format minor") != std::string::npos);
    }
    SECTION("content version differs")
    {
        FixtureTree tree("incremental_content", "mixed-2x2.terrain.json");
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        const TerrainReport baked = RunTerrainBake(base);
        REQUIRE(baked.Ok());
        REQUIRE(baked.release);
        const fs::path dir = PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);
        PatchIndexAndReseal(dir, 16, kTerrainHeightsContentVersion - 1);

        TerrainBakeRequest incremental =
            WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
        const TerrainReport report = RunTerrainBake(incremental);
        REQUIRE(report.Ok());
        const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
        REQUIRE(full.Ok());
        CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
        const std::string detail = CheckDetail(report, check::kEncode);
        CHECK(detail.find("full bake because") != std::string::npos);
        CHECK(detail.find("does not open") != std::string::npos);
    }
    SECTION("the base release does not open")
    {
        FixtureTree tree("incremental_damaged", "mixed-2x2.terrain.json");
        TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
        const TerrainReport baked = RunTerrainBake(base);
        REQUIRE(baked.Ok());
        REQUIRE(baked.release);
        const fs::path dir = PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        index[0] ^= std::byte{0xFF};
        WriteFile(dir / kTerrainIndexFileName, index);

        TerrainBakeRequest incremental =
            WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
        const TerrainReport report = RunTerrainBake(incremental);
        REQUIRE(report.Ok());
        const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce));
        REQUIRE(full.Ok());
        CHECK(Snapshot(incremental.candidateDir) == Snapshot(tree.scratch.path / "full"));
        CHECK(CheckDetail(report, check::kEncode).find("does not open") != std::string::npos);
    }
}

TEST_CASE("Validating an incremental candidate reuses base payloads and catches a changed one",
          "[terraintool][incremental]")
{
    FixtureTree tree("incremental_validate", "relief-2x2.terrain.json");
    TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce);
    const TerrainReport baked = RunTerrainBake(base);
    REQUIRE(baked.Ok());
    REQUIRE(baked.release);
    PublishRelease(base.candidateDir, tree.releases, tree.asset, baked.release->release);

    const TerrainResult<TerrainReleaseHandle> opened = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(opened);

    TerrainBakeRequest incremental =
        WithBase(tree.Request(tree.scratch.path / "incremental", kNonce), tree.releases, tree.asset);
    const TerrainReport report = RunTerrainBake(incremental);
    REQUIRE(report.Ok());

    TerrainValidateRequest withBase{incremental.candidateDir, tree.recipe, *opened.value};
    const TerrainReport reused = RunTerrainValidate(withBase);
    for (const TerrainDiagnostic &d : reused.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(reused.Ok());
    CHECK(CheckDetail(reused, check::kPayloads).find("reused from the base release") != std::string::npos);

    TerrainValidateRequest withoutBase{incremental.candidateDir, tree.recipe, std::nullopt};
    const TerrainReport rederived = RunTerrainValidate(withoutBase);
    REQUIRE(rederived.Ok());
    const std::string fresh = CheckDetail(rederived, check::kPayloads);
    CHECK(fresh.find("from final source") != std::string::npos);
    CHECK(fresh.find("reused") == std::string::npos);

    const fs::path corrupt = tree.scratch.path / "corrupt";
    fs::copy(incremental.candidateDir, corrupt, fs::copy_options::recursive);
    PatchPayloadSealed(corrupt, {-1, -1}, 0, TerrainPayloadKind::LodHeight);

    TerrainValidateRequest changed{corrupt, tree.recipe, *opened.value};
    const TerrainReport failed = RunTerrainValidate(changed);
    for (const TerrainDiagnostic &d : failed.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    CHECK(failed.Status(check::kPayloads) == CheckStatus::Failed);
    CHECK(std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(),
                      [](const TerrainDiagnostic &d)
                      {
                          return d.check == check::kPayloads && d.tiles.size() == 1 && d.tiles[0] == TileCoord{-1, -1};
                      }));
}

TEST_CASE("Validation ignores a base whose index does not match the candidate", "[terraintool][incremental]")
{
    FixtureTree tree("incremental_ignored", "mixed-2x2.terrain.json");
    TerrainBakeRequest candidate = tree.Request(tree.scratch.path / "candidate", kNonce);
    const TerrainReport baked = RunTerrainBake(candidate);
    REQUIRE(baked.Ok());

    TerrainBakeRequest base = tree.Request(tree.scratch.path / "base", kNonce + 1);
    base.options.regions = {8, kTerrainRegionCells};
    const TerrainReport baseBaked = RunTerrainBake(base);
    REQUIRE(baseBaked.Ok());
    REQUIRE(baseBaked.release);
    PublishRelease(base.candidateDir, tree.releases, tree.asset, baseBaked.release->release);
    const TerrainResult<TerrainReleaseHandle> opened = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    const TerrainResult<TerrainReleaseHandle> candidateRelease =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(candidate.candidateDir));
    REQUIRE(opened);
    REQUIRE(candidateRelease);
    CHECK(ReadAllTiles(*opened.value) == ReadAllTiles(*candidateRelease.value));

    TerrainValidateRequest request{candidate.candidateDir, tree.recipe, *opened.value};
    const TerrainReport report = RunTerrainValidate(request);
    for (const TerrainDiagnostic &d : report.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(report.Ok());
    const std::string detail = CheckDetail(report, check::kPayloads);
    CHECK(detail.find("ignored") != std::string::npos);
    CHECK(detail.find("Region grid") != std::string::npos);
}

TEST_CASE("Sync bakes incrementally from the current release and matches a full bake", "[terraintool][incremental]")
{
    FixtureTree tree("incremental_sync", "relief-2x2.terrain.json");
    const TerrainReport first = RunTerrainSync(tree.Sync(tree.scratch.path / "sync", kNonce));
    for (const TerrainDiagnostic &d : first.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(first.Ok());
    REQUIRE(first.publish);
    REQUIRE(first.publish->published);
    REQUIRE(first.publish->current);
    const TerrainReleaseId baseRelease = *first.publish->current;

    const TerrainRecipe recipe = RecipeOf(tree.recipe);
    TerrainEditLayer layer = AdditiveEditLayer("edit");
    SetTerrainEditSample(layer, recipe.layout,
                         {recipe.layout.TileFirstSample(-1) + recipe.layout.IntervalsPerTile() / 2,
                          recipe.layout.TileFirstSample(-1) + recipe.layout.IntervalsPerTile() / 2},
                         4.0f);
    WriteEditLayers(tree.recipe, std::span(&layer, 1));

    const TerrainReport changed = RunTerrainSync(tree.Sync(tree.scratch.path / "sync", kNonce + 1));
    for (const TerrainDiagnostic &d : changed.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(changed.Ok());
    REQUIRE(changed.publish);
    CHECK(changed.publish->published);
    CHECK(changed.publish->previous == baseRelease);
    REQUIRE(changed.publish->current);
    REQUIRE(changed.release);
    CHECK(CheckDetail(changed, check::kEncode).find("incremental") != std::string::npos);

    const TerrainReport full = RunTerrainBake(tree.Request(tree.scratch.path / "full", kNonce + 1));
    REQUIRE(full.Ok());
    REQUIRE(full.release);
    CHECK(*changed.publish->current == full.release->release);
    const fs::path published = TerrainReleaseDir(tree.releases, tree.asset, *changed.publish->current);
    CHECK(ReleaseFiles(published) == ReleaseFiles(tree.scratch.path / "full"));
}
