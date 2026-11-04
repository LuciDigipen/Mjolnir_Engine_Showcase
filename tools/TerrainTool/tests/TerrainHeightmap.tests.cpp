/**
 * @file TerrainHeightmap.tests.cpp
 * @author Rahul Nair
 * @brief Tests for the "mir.heightmap" generator and its MHGT source file.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainHeightSource.h>
#include <TerrainProgress.h>
#include <TerrainPublish.h>
#include <TerrainSync.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    constexpr std::string_view kRecipeSource = "terrain/hm.terrain.json";
    constexpr int kSide = 17; // two 8 m tiles of 1 m samples, -8 to 8

    // Tilted differently per axis with a cross term, so a swap or flip changes it.
    double Surface(double x, double z)
    {
        return 3.0 + 0.25 * x - 0.5 * z + 0.01 * x * z;
    }

    struct HeightFile
    {
        char magic[4] = { 'M', 'H', 'G', 'T' };
        uint32_t version = kHeightSourceVersion;
        uint32_t width = kSide;
        uint32_t height = kSide;
        double originX = -8.0;
        double originZ = -8.0;
        double spacing = 1.0;
        double offset = 0.0;

        std::vector<std::byte> Bytes() const
        {
            std::vector<std::byte> bytes(kHeightSourceHeaderSize + size_t(width) * height * 4);
            std::memcpy(bytes.data(), magic, 4);
            std::memcpy(bytes.data() + 4, &version, 4);
            std::memcpy(bytes.data() + 8, &width, 4);
            std::memcpy(bytes.data() + 12, &height, 4);
            std::memcpy(bytes.data() + 16, &originX, 8);
            std::memcpy(bytes.data() + 24, &originZ, 8);
            std::memcpy(bytes.data() + 32, &spacing, 8);
            for (uint32_t k = 0; k < height; ++k)
                for (uint32_t i = 0; i < width; ++i)
                {
                    const float h = float(Surface(originX + i * spacing, originZ + k * spacing) + offset);
                    std::memcpy(bytes.data() + kHeightSourceHeaderSize + (size_t(k) * width + i) * 4, &h, 4);
                }
            return bytes;
        }
    };

    void SetSample(std::vector<std::byte> &bytes, size_t index, float value)
    {
        std::memcpy(bytes.data() + kHeightSourceHeaderSize + index * 4, &value, 4);
    }

    std::string HashText(std::span<const std::byte> bytes)
    {
        return TerrainIdHex(Fnv1a64(bytes));
    }

    nlohmann::json RecipeJson(std::string_view hash)
    {
        return { { "recipeVersion", 2 },
                { "generator",{ { "id", "mir.heightmap" },{ "version", 1 } } },
                { "seed", 0 },
                { "layout",
                 { { "sampleSpacing", 1.0 },
                  { "intervalsPerTile", 8 },
                  { "bounds",{ { "minX", -8.0 },{ "minZ", -8.0 },{ "maxX", 8.0 },{ "maxZ", 8.0 } } } } },
                { "parameters",{ { "source", "../../source/hm.heights" },{ "contentHash", std::string(hash) } } },
                { "precision",{ { "defaultSpacing", 1.0 },{ "tiles", nlohmann::json::array() } } } };
    }

    // games/G with a recipe and its height file.
    struct Tree
    {
        ScratchDir scratch;
        fs::path gameDir = scratch.path / "games" / "G";
        fs::path recipe = gameDir / "assets" / "terrain" / "hm.terrain.json";
        fs::path heights = gameDir / "source" / "hm.heights";
        fs::path releases = TerrainReleasesRoot(gameDir);

        explicit Tree(std::string_view tag) : scratch(tag)
        {
            fs::create_directories(recipe.parent_path());
            fs::create_directories(heights.parent_path());
        }

        // A recipe naming these exact bytes.
        void Write(std::span<const std::byte> bytes, nlohmann::json json) const
        {
            WriteFile(heights, bytes);
            REQUIRE(WriteText(recipe, json.dump(2)));
        }
        void Write(std::span<const std::byte> bytes) const
        {
            Write(bytes, RecipeJson(HashText(bytes)));
        }

        TerrainSyncRequest Request() const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kRecipeSource);
            request.gameDir = gameDir;
            request.candidateDir = scratch.path / "build" / "hm";
            return request;
        }

        TerrainReport Sync() const
        {
            TerrainReport report = RunTerrainSync(Request());
            for (const TerrainDiagnostic &d : report.diagnostics)
                UNSCOPED_INFO(FormatDiagnostic(d));
            return report;
        }
    };

    bool GenerateFails(const TerrainReport &report, std::string_view text)
    {
        if (report.Status(check::kGenerate) != CheckStatus::Failed)
            return false;
        for (const TerrainDiagnostic &d : report.diagnostics)
            if (d.check == check::kGenerate && d.message.find(text) != std::string::npos)
                return true;
        return false;
    }

    bool HasIssue(const RecipeParseResult &result, std::string_view field, std::string_view text)
    {
        for (const RecipeIssue &issue : result.issues)
            if (issue.field == field && issue.message.find(text) != std::string::npos)
                return true;
        return false;
    }

    TerrainLayout TestLayout()
    {
        return *TerrainLayout::Create(1.0, 8, { -8.0, -8.0, 8.0, 8.0 });
    }

    // The authored tiles, without the release's derivative layers.
    std::vector<TileHeights> ReadSourceTiles(const TerrainReleaseHandle &handle)
    {
        std::vector<TileHeights> tiles;
        for (const TerrainTileRecord &record : handle.Index().tiles)
        {
            if (record.kind != TerrainPayloadKind::Height || !record.source)
                continue;
            TerrainResult<TileHeights> tile = handle.ReadTileRecord(record);
            REQUIRE(tile);
            tiles.push_back(std::move(*tile.value));
        }
        return tiles;
    }
}

TEST_CASE("A heightmap recipe parses, and its hash alone identifies the source", "[terraintool][heightmap]")
{
    const RecipeParseResult parsed = ParseTerrainRecipe(RecipeJson("0123456789abcdef").dump());
    REQUIRE(parsed.recipe);
    CHECK(parsed.recipe->IsHeightmap());
    CHECK(parsed.recipe->heightmapPath == "../../source/hm.heights");
    CHECK(parsed.recipe->heightmapHash == 0x0123456789abcdefull);
    CHECK_FALSE(parsed.recipe->heightSource);

    const auto fingerprint = FingerprintRecipe(*parsed.recipe);
    CHECK(FingerprintRecipe(*ParseTerrainRecipe(RecipeJson("0123456789abcde0").dump()).recipe) != fingerprint);

    nlohmann::json moved = RecipeJson("0123456789abcdef");
    moved["parameters"]["source"] = "elsewhere/hm.heights";
    CHECK(FingerprintRecipe(*ParseTerrainRecipe(moved.dump()).recipe) == fingerprint);

    CHECK(CanonicalRecipeText(*parsed.recipe).find("parameters.broad") == std::string::npos);
}

TEST_CASE("Bad heightmap recipes report every field", "[terraintool][heightmap]")
{
    const auto edited = [](auto edit)
        {
            nlohmann::json json = RecipeJson("0123456789abcdef");
            edit(json);
            return ParseTerrainRecipe(json.dump());
        };
    CHECK(HasIssue(edited([](auto &j) { j["parameters"].erase("source"); }), "parameters.source", "missing"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["source"] = ""; }), "parameters.source", "relative"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["source"] = "/abs/hm.heights"; }), "parameters.source", "relative"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["source"] = "C:/abs/hm.heights"; }), "parameters.source", "relative"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["source"] = 3; }), "parameters.source", "expected a string"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"].erase("contentHash"); }), "parameters.contentHash", "missing"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["contentHash"] = "abc"; }), "parameters.contentHash", "16 lowercase"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["contentHash"] = "0123456789ABCDEF"; }), "parameters.contentHash", "16 lowercase"));
    CHECK(HasIssue(edited([](auto &j) { j["parameters"]["baseHeight"] = 1.0; }), "parameters.baseHeight", "unknown field"));
    CHECK(HasIssue(edited([](auto &j) { j["generator"]["version"] = 2; }), "generator.version", "unsupported version 2"));
    CHECK(HasIssue(edited([](auto &j) { j["recipeVersion"] = 1; j.erase("precision"); }), "generator.id", "requires recipeVersion 2"));
}

TEST_CASE("A height source samples exactly, interpolates, and clamps off the map", "[terraintool][heightmap]")
{
    Tree tree("hm_sample");
    const std::vector<std::byte> bytes = HeightFile{}.Bytes();
    WriteFile(tree.heights, bytes);
    std::string error;
    const auto source = TerrainHeightSource::Load(tree.heights, Fnv1a64(std::span<const std::byte>(bytes)), TestLayout(), error);
    REQUIRE(source);

    CHECK(source->Sample(-8.0, -8.0) == float(Surface(-8.0, -8.0)));
    CHECK(source->Sample(3.0, -5.0) == float(Surface(3.0, -5.0)));
    CHECK(source->Sample(8.0, 8.0) == float(Surface(8.0, 8.0)));
    CHECK(source->Sample(3.5, -5.0) == Catch::Approx((float(Surface(3.0, -5.0)) + double(float(Surface(4.0, -5.0)))) / 2.0));
    CHECK(source->Sample(-20.0, 1.0) == float(Surface(-8.0, 1.0)));
    CHECK(source->Sample(1.0, 20.0) == float(Surface(1.0, 8.0)));
}

TEST_CASE("A bad height source fails the bake with its reason", "[terraintool][heightmap]")
{
    Tree tree("hm_bad");
    const auto expectFailure = [&](const std::vector<std::byte> &bytes, std::string_view text)
        {
            INFO(text);
            tree.Write(bytes);
            const TerrainReport report = tree.Sync();
            CHECK_FALSE(report.Ok());
            CHECK(GenerateFails(report, text));
            CHECK_FALSE(report.publish->current);
        };

    SECTION("missing")
    {
        REQUIRE(WriteText(tree.recipe, RecipeJson("0123456789abcdef").dump()));
        CHECK(GenerateFails(tree.Sync(), "does not exist"));
    }
    SECTION("wrong hash")
    {
        const std::vector<std::byte> bytes = HeightFile{}.Bytes();
        tree.Write(bytes, RecipeJson("0123456789abcdef"));
        CHECK(GenerateFails(tree.Sync(), "contentHash is 0123456789abcdef"));
    }
    SECTION("not MHGT")
    {
        std::vector<std::byte> bytes = HeightFile{}.Bytes();
        bytes[0] = std::byte{ 'X' };
        expectFailure(bytes, "not an MHGT");
    }
    SECTION("shorter than a header")
    {
        expectFailure(std::vector<std::byte>(8), "not an MHGT");
    }
    SECTION("newer version")
    {
        HeightFile file;
        file.version = kHeightSourceVersion + 1;
        expectFailure(file.Bytes(), "is version");
    }
    SECTION("truncated")
    {
        std::vector<std::byte> bytes = HeightFile{}.Bytes();
        bytes.pop_back();
        expectFailure(bytes, "do not match");
    }
    SECTION("NaN")
    {
        std::vector<std::byte> bytes = HeightFile{}.Bytes();
        SetSample(bytes, 40, std::numeric_limits<float>::quiet_NaN());
        expectFailure(bytes, "outside finite");
    }
    SECTION("beyond the height limit")
    {
        std::vector<std::byte> bytes = HeightFile{}.Bytes();
        SetSample(bytes, 40, float(kMaxHeightMagnitude * 2.0));
        expectFailure(bytes, "outside finite");
    }
    SECTION("wrong spacing")
    {
        HeightFile file;
        file.spacing = 2.0;
        expectFailure(file.Bytes(), "differs from the recipe");
    }
    SECTION("off the sample grid")
    {
        HeightFile file;
        file.originX = -8.5;
        file.width = kSide + 1;
        file.height = kSide;
        expectFailure(file.Bytes(), "off the");
    }
    SECTION("short of the map's tiles")
    {
        HeightFile file;
        file.width = kSide - 1;
        expectFailure(file.Bytes(), "the map's tiles need");
    }
}

TEST_CASE("Sync publishes the file's exact samples, then needs no source", "[terraintool][heightmap][sync]")
{
    Tree tree("hm_sync");
    tree.Write(HeightFile{}.Bytes());
    const TerrainReport first = tree.Sync();
    REQUIRE(first.Ok());
    REQUIRE(first.publish->current);
    CHECK(first.publish->published);

    const AssetId asset = ResolveTerrainAssetPath(kIdRoot, kRecipeSource).asset;
    TerrainResult<TerrainReleaseHandle> release = OpenTerrainRelease(tree.releases, asset, *first.publish->current);
    REQUIRE(release);
    const std::vector<TileHeights> tiles = ReadSourceTiles(*release.value);
    REQUIRE(tiles.size() == 4);
    for (const TileHeights &tile : tiles)
    {
        const int32_t n = tile.IntervalsPerTile();
        const std::span<const float> samples = tile.Samples();
        for (int32_t k = 0; k <= n; ++k)
            for (int32_t i = 0; i <= n; ++i)
            {
                const double x = (tile.Tile().x * n) + i;
                const double z = (tile.Tile().z * n) + k;
                INFO("tile " << tile.Tile().x << ", " << tile.Tile().z << " sample " << i << ", " << k);
                CHECK(samples[size_t(k) * (n + 1) + i] == float(Surface(x, z)));
            }
    }

    SECTION("a current release does not read the source")
    {
        fs::remove(tree.heights);
        const TerrainReport again = tree.Sync();
        REQUIRE(again.Ok());
        CHECK_FALSE(again.publish->published);
        CHECK(again.publish->current == first.publish->current);
        CHECK(again.Status(check::kGenerate) == CheckStatus::Skipped);
    }
    SECTION("new heights under a new hash publish a new release")
    {
        HeightFile changed;
        changed.offset = 1.0;
        tree.Write(changed.Bytes());
        const TerrainReport again = tree.Sync();
        REQUIRE(again.Ok());
        CHECK(again.publish->published);
        CHECK(again.publish->current != first.publish->current);
    }
}

TEST_CASE("A decimal sample spacing keeps its origin on the grid and its samples exact", "[terraintool][heightmap]")
{
    // -24 * 0.1 is not the double nearest -2.4, and neither has a zero remainder by 0.1.
    constexpr int kFirst = -24;
    constexpr int kDecimalSide = 49;
    const double spacing = 0.1;
    const double origin = kFirst * spacing;
    const double end = -origin;
    REQUIRE(std::fmod(origin, spacing) != 0.0);

    HeightFile file;
    file.width = kDecimalSide;
    file.height = kDecimalSide;
    file.originX = origin;
    file.originZ = origin;
    file.spacing = spacing;
    const std::vector<std::byte> bytes = file.Bytes();

    nlohmann::json json = RecipeJson(HashText(bytes));
    json["layout"]["sampleSpacing"] = spacing;
    json["layout"]["bounds"] = { { "minX", origin },{ "minZ", origin },{ "maxX", end },{ "maxZ", end } };
    json["precision"]["defaultSpacing"] = spacing;

    Tree tree("hm_decimal");
    tree.Write(bytes, json);
    const TerrainReport report = tree.Sync();
    REQUIRE(report.Ok());
    REQUIRE(report.publish->current);

    const AssetId asset = ResolveTerrainAssetPath(kIdRoot, kRecipeSource).asset;
    TerrainResult<TerrainReleaseHandle> release = OpenTerrainRelease(tree.releases, asset, *report.publish->current);
    REQUIRE(release);
    const std::vector<TileHeights> tiles = ReadSourceTiles(*release.value);
    REQUIRE(tiles.size() == 36);
    for (const TileHeights &tile : tiles)
    {
        const int32_t n = tile.IntervalsPerTile();
        const std::span<const float> samples = tile.Samples();
        for (int32_t k = 0; k <= n; k += n)
            for (int32_t i = 0; i <= n; i += n)
            {
                const size_t row = size_t(tile.Tile().z * n + k - kFirst);
                const size_t col = size_t(tile.Tile().x * n + i - kFirst);
                float expected = 0.0f;
                std::memcpy(&expected, bytes.data() + kHeightSourceHeaderSize + (row * kDecimalSide + col) * 4, 4);
                CHECK(samples[size_t(k) * (n + 1) + i] == expected);
            }
    }
}

TEST_CASE("A bake reports its stages in order, and a current release reports none", "[terraintool][heightmap]")
{
    static std::vector<std::string> lines;
    lines.clear();
    struct SinkScope
    {
        SinkScope() { SetTerrainProgressSink([](std::string_view line) { lines.emplace_back(line); }); }
        ~SinkScope() { SetTerrainProgressSink(nullptr); }
    } scope;

    Tree tree("hm_progress");
    tree.Write(HeightFile{}.Bytes());
    REQUIRE(tree.Sync().Ok());

    size_t at = 0;
    for (std::string_view stage : {"load heights", "generate: tiles", "seams", "derivatives: tiles", "regions", "encode",
        "write", "validate", "validate: tiles", "validate: regions"})
    {
        INFO(stage);
        const auto found = std::find_if(lines.begin() + at, lines.end(),
            [&](const std::string &line) { return line.find(stage) != std::string::npos; });
        REQUIRE(found != lines.end());
        at = size_t(found - lines.begin());
    }

    lines.clear();
    REQUIRE(tree.Sync().Ok());
    CHECK(lines.empty());
}

TEST_CASE("Progress prints only when the whole percent changes", "[terraintool][heightmap]")
{
    static int printed = 0;
    printed = 0;
    SetTerrainProgressSink([](std::string_view) { ++printed; });
    for (uint64_t done = 0; done < 1000; ++done)
        TerrainProgressStep("many", done, 1000);
    TerrainProgressStep("many", 0, 0);
    SetTerrainProgressSink(nullptr);
    CHECK(printed == 100);
}

TEST_CASE("A coarse heightmap tile filters past the map edge", "[terraintool][heightmap]")
{
    Tree tree("hm_mixed");
    const std::vector<std::byte> bytes = HeightFile{}.Bytes();
    nlohmann::json json = RecipeJson(HashText(bytes));
    json["precision"]["tiles"] = { { { "x", -1 },{ "z", -1 },{ "spacing", 2.0 } } };
    tree.Write(bytes, json);
    const TerrainReport report = tree.Sync();
    CHECK(report.Ok());
}
