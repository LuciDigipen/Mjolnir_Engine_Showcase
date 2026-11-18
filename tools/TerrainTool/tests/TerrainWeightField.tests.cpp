/**
 * @file TerrainWeightField.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool weight evaluation of material rules.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <TerrainBake.h>
#include <TerrainFinalSurface.h>
#include <TerrainMaterial.h>
#include <TerrainNoise.h>
#include <TerrainRecipe.h>

#include <assets/AssetId.h>
#include <terrain/TerrainWeights.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;

namespace
{
    using Json = nlohmann::json;

    std::string RecipeText(std::string_view relative)
    {
        std::ifstream in(std::string(TERRAIN_PROJECT_DIR) + "/" + std::string(relative), std::ios::binary);
        REQUIRE(in);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    RecipeParseResult Parse(const Json &json)
    {
        RecipeParseResult result = ParseTerrainRecipe(json.dump());
        for (const RecipeIssue &issue : result.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        return result;
    }

    TerrainRecipe Valid(const Json &json)
    {
        RecipeParseResult result = Parse(json);
        REQUIRE(result.recipe);
        return *result.recipe;
    }

    Json HeightRule(double min, double max, double blend)
    {
        return Json{{"min", min}, {"max", max}, {"blend", blend}};
    }

    Json SlopeRule(double min, double max, double blend)
    {
        return Json{{"min", min}, {"max", max}, {"blend", blend}};
    }

    Json NoiseRule(double wavelength, int octaves, double strength)
    {
        return Json{{"wavelength", wavelength}, {"octaves", octaves}, {"strength", strength}};
    }

    Json Layers(std::initializer_list<Json> layers)
    {
        Json array = Json::array();
        for (const Json &layer : layers)
            array.push_back(layer);
        Json materials = Json::object();
        materials["layers"] = std::move(array);
        return materials;
    }

    Json BaseRecipe()
    {
        Json json = Json::object();
        json["recipeVersion"] = 1;
        json["generator"] = Json{{"id", "mir.relief"}, {"version", 1}};
        json["seed"] = 424242u;
        json["layout"] = Json{{"sampleSpacing", 1.0},
                              {"intervalsPerTile", 8},
                              {"bounds", Json{{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 8.0}, {"maxZ", 8.0}}}};
        json["parameters"] = Json{{"baseHeight", 0.0},
                                  {"tilt", Json{{"x", 0.0}, {"z", 0.0}}},
                                  {"broad", Json{{"amplitude", 0.0}, {"wavelength", 64.0}, {"octaves", 1}}},
                                  {"ridges", Json{{"amplitude", 0.0}, {"wavelength", 64.0}, {"octaves", 1}}},
                                  {"detail", Json{{"amplitude", 0.0}, {"wavelength", 64.0}, {"octaves", 1}}},
                                  {"plains", Json{{"height", 0.0}, {"blend", 0.0}}}};
        return json;
    }

    TileHeights RampTile(const TerrainLayout &layout)
    {
        std::optional<TileHeights> tile = TileHeights::Create({0, 0}, layout.IntervalsPerTile());
        REQUIRE(tile);
        for (int32_t k = 0; k <= layout.IntervalsPerTile(); ++k)
            for (int32_t i = 0; i <= layout.IntervalsPerTile(); ++i)
            {
                const double x = layout.SamplePosition(layout.TileFirstSample(0) + i);
                tile->Set(i, k, static_cast<float>(10.0 + x));
            }
        return std::move(*tile);
    }

    std::map<std::pair<int32_t, int32_t>, const TileHeights *> ByTile(const std::vector<TileHeights> &tiles)
    {
        std::map<std::pair<int32_t, int32_t>, const TileHeights *> byTile;
        for (const TileHeights &tile : tiles)
            byTile[{tile.Tile().z, tile.Tile().x}] = &tile;
        return byTile;
    }

    std::optional<TerrainWeightTile> Evaluate(const TerrainRecipe &recipe,
                                              const std::map<std::pair<int32_t, int32_t>, const TileHeights *> &byTile,
                                              TileCoord tile)
    {
        TerrainFinalSurface surface{recipe, byTile};
        return EvaluateTerrainWeights(recipe, surface, tile);
    }

    void CheckWeights(const TerrainWeightTile &weights)
    {
        const size_t count = weights.SampleCount();
        for (size_t sample = 0; sample < count; ++sample)
        {
            double sum = 0.0;
            for (size_t layer = 0; layer < weights.layers.size(); ++layer)
            {
                const float weight = weights.weights[layer * count + sample];
                CHECK(weight >= 0.0f);
                CHECK(weight <= 1.0f);
                sum += weight;
            }
            CHECK(std::abs(sum - 1.0) <= kTerrainWeightSumTolerance);
        }
    }

    bool SameBits(const TerrainWeightTile &a, const TerrainWeightTile &b)
    {
        if (a.weights.size() != b.weights.size())
            return false;
        for (size_t i = 0; i < a.weights.size(); ++i)
            if (std::bit_cast<uint32_t>(a.weights[i]) != std::bit_cast<uint32_t>(b.weights[i]))
                return false;
        return true;
    }
}

TEST_CASE("Height rules ramp over blend and give hard edges at blend 0", "[terraintool][material][weights]")
{
    const TerrainRecipe recipe = Valid(BaseRecipe());
    const std::vector<TileHeights> tiles{RampTile(recipe.layout)};
    const auto byTile = ByTile(tiles);

    SECTION("a blended range")
    {
        Json json = BaseRecipe();
        json["materials"] = Layers({Json{{"name", "base"}},
                                    Json{{"name", "sand"}, {"rules", Json{{"height", HeightRule(12.0, 14.0, 2.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);

        const double expected[] = {0.0, 0.5, 1.0, 1.0, 1.0, 0.5, 0.0, 0.0, 0.0};
        for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); ++i)
        {
            INFO("x sample " << i);
            CHECK(weights->At(1, i, 0) == Catch::Approx(expected[i]).margin(1e-6));
            CHECK(weights->At(0, i, 0) == Catch::Approx(1.0 - expected[i]).margin(1e-6));
        }
    }
    SECTION("a hard edge is inclusive")
    {
        Json json = BaseRecipe();
        json["materials"] = Layers({Json{{"name", "base"}},
                                    Json{{"name", "sand"}, {"rules", Json{{"height", HeightRule(12.0, 14.0, 0.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);

        const double expected[] = {0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0};
        for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); ++i)
        {
            INFO("x sample " << i);
            CHECK(weights->At(1, i, 0) == Catch::Approx(expected[i]).margin(1e-6));
        }
    }
}

TEST_CASE("A layer without rules covers nothing until it is painted", "[terraintool][material][weights]")
{
    Json json = BaseRecipe();
    json["materials"] = Layers({Json{{"name", "grass"}}, Json{{"name", "sand"}}});
    const TerrainRecipe dressed = Valid(json);
    const std::vector<TileHeights> tiles{RampTile(dressed.layout)};
    const auto byTile = ByTile(tiles);
    const auto weights = Evaluate(dressed, byTile, {0, 0});
    REQUIRE(weights);
    CheckWeights(*weights);

    for (int32_t k = 0; k <= dressed.layout.IntervalsPerTile(); ++k)
        for (int32_t i = 0; i <= dressed.layout.IntervalsPerTile(); ++i)
        {
            CHECK(weights->At(0, i, k) == Catch::Approx(1.0).margin(1e-6));
            CHECK(weights->At(1, i, k) == Catch::Approx(0.0).margin(1e-6));
        }
}

TEST_CASE("Slope rules ramp on the final surface normal", "[terraintool][material][weights]")
{
    const TerrainRecipe recipe = Valid(BaseRecipe());
    const std::vector<TileHeights> tiles{RampTile(recipe.layout)};
    const auto byTile = ByTile(tiles);
    const TerrainFinalSurface surface{recipe, byTile};
    const glm::vec3 normal = surface.Normal(0.0, 0.0, tiles.front());
    const double slope = std::acos(static_cast<double>(normal.y)) * 57.295779513082320876798154814105;

    SECTION("inside the inclusive range")
    {
        Json json = BaseRecipe();
        json["materials"] = Layers({Json{{"name", "base"}},
                                    Json{{"name", "sand"}, {"rules", Json{{"slope", SlopeRule(0.0, slope, 0.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);
        for (int32_t k = 0; k <= recipe.layout.IntervalsPerTile(); ++k)
            for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); ++i)
            {
                INFO("sample " << i << ", " << k);
                CHECK(weights->At(1, i, k) == Catch::Approx(1.0).margin(1e-6));
            }
    }
    SECTION("outside the range with a blend")
    {
        Json json = BaseRecipe();
        json["materials"] =
            Layers({Json{{"name", "base"}},
                    Json{{"name", "sand"}, {"rules", Json{{"slope", SlopeRule(50.0, 60.0, 20.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);

        const double expected = 1.0 - (50.0 - slope) / 20.0;
        CHECK(expected > 0.0);
        CHECK(expected < 1.0);
        for (int32_t k = 0; k <= recipe.layout.IntervalsPerTile(); k += 2)
            for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); i += 2)
            {
                INFO("sample " << i << ", " << k);
                CHECK(weights->At(1, i, k) == Catch::Approx(expected).margin(1e-6));
            }
    }
}

TEST_CASE("Noise rules follow the recipe seed and layer name", "[terraintool][material][weights]")
{
    Json json = BaseRecipe();
    json["materials"] = Layers({Json{{"name", "base"}},
                                Json{{"name", "sand"}, {"rules", Json{{"noise", NoiseRule(4.0, 3, 0.8)}}}}});
    const TerrainRecipe recipe = Valid(json);
    const std::vector<TileHeights> tiles{RampTile(recipe.layout)};
    const auto byTile = ByTile(tiles);

    const auto weights = Evaluate(recipe, byTile, {0, 0});
    REQUIRE(weights);
    CheckWeights(*weights);

    const OctaveNoise noise(MixBits(recipe.seed ^ Fnv1a64("sand")), 4.0, 3);
    for (int32_t k = 0; k <= recipe.layout.IntervalsPerTile(); k += 3)
        for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); i += 3)
        {
            const double x = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(0) + i);
            const double z = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(0) + k);
            const double expected = std::clamp(1.0 + 0.8 * noise.Fbm(x, z), 0.0, 1.0);
            INFO("sample " << i << ", " << k);
            CHECK(weights->At(1, i, k) == Catch::Approx(expected).margin(1e-6));
        }
}

TEST_CASE("Later material layers composite over earlier ones", "[terraintool][material][weights]")
{
    const TerrainRecipe recipe = Valid(BaseRecipe());
    const std::vector<TileHeights> tiles{RampTile(recipe.layout)};
    const auto byTile = ByTile(tiles);

    SECTION("a later layer with full coverage takes the weight")
    {
        Json json = BaseRecipe();
        json["materials"] =
            Layers({Json{{"name", "base"}},
                    Json{{"name", "middle"}, {"rules", Json{{"height", HeightRule(-1000.0, 1000.0, 0.0)}}}},
                    Json{{"name", "top"}, {"rules", Json{{"height", HeightRule(-1000.0, 1000.0, 0.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);
        for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); ++i)
        {
            CHECK(weights->At(0, i, 0) == 0.0f);
            CHECK(weights->At(1, i, 0) == 0.0f);
            CHECK(weights->At(2, i, 0) == 1.0f);
        }
    }
    SECTION("a later layer with no coverage keeps the earlier weight")
    {
        Json json = BaseRecipe();
        json["materials"] =
            Layers({Json{{"name", "base"}},
                    Json{{"name", "middle"}, {"rules", Json{{"height", HeightRule(-1000.0, 1000.0, 0.0)}}}},
                    Json{{"name", "top"}, {"rules", Json{{"height", HeightRule(100.0, 200.0, 0.0)}}}}});
        const TerrainRecipe dressed = Valid(json);
        const auto weights = Evaluate(dressed, byTile, {0, 0});
        REQUIRE(weights);
        CheckWeights(*weights);
        for (int32_t i = 0; i <= recipe.layout.IntervalsPerTile(); ++i)
        {
            CHECK(weights->At(0, i, 0) == 0.0f);
            CHECK(weights->At(1, i, 0) == 1.0f);
            CHECK(weights->At(2, i, 0) == 0.0f);
        }
    }
}

TEST_CASE("Reference-grid weights agree across mixed-precision tiles", "[terraintool][material][weights]")
{
    const std::string text = RecipeText("games/TerrainCheck/assets/terrain/relief-2x2.terrain.json");
    Json json = Json::parse(text);
    json["materials"] =
        Layers({Json{{"name", "base"}},
                Json{{"name", "sand"},
                     {"rules", Json{{"height", HeightRule(20.0, 60.0, 20.0)},
                                    {"slope", SlopeRule(5.0, 40.0, 10.0)},
                                    {"noise", NoiseRule(96.0, 3, 0.5)}}}}});
    const TerrainRecipe recipe = Valid(json);

    TerrainReport report("weights", {});
    std::optional<std::vector<TileHeights>> baked = BakeFinalHeights(recipe, report);
    REQUIRE(baked);
    const auto byTile = ByTile(*baked);

    const TerrainLayout &layout = recipe.layout;
    const int32_t intervals = layout.IntervalsPerTile();
    std::vector<TerrainWeightTile> grids;
    for (int32_t z = layout.Tiles().min.z; z <= layout.Tiles().max.z; ++z)
        for (int32_t x = layout.Tiles().min.x; x <= layout.Tiles().max.x; ++x)
        {
            std::optional<TerrainWeightTile> grid = Evaluate(recipe, byTile, {x, z});
            REQUIRE(grid);
            CHECK(grid->intervals == intervals);
            CHECK(grid->layers.size() == 2);
            CHECK(ValidateTerrainWeightTile(*grid) == TerrainWeightStatus::Ok);
            CheckWeights(*grid);
            grids.push_back(std::move(*grid));
        }

    std::map<std::pair<int64_t, int64_t>, std::vector<float>> shared;
    size_t sharedSamples = 0;
    for (const TerrainWeightTile &grid : grids)
    {
        const int32_t side = grid.SamplesPerSide();
        for (int32_t k = 0; k < side; ++k)
            for (int32_t i = 0; i < side; ++i)
            {
                const int64_t gx = layout.TileFirstSample(grid.tile.x) + i;
                const int64_t gz = layout.TileFirstSample(grid.tile.z) + k;
                std::vector<float> values;
                for (size_t layer = 0; layer < grid.layers.size(); ++layer)
                    values.push_back(grid.At(layer, i, k));
                const auto inserted = shared.emplace(std::make_pair(gx, gz), std::move(values));
                if (!inserted.second)
                {
                    ++sharedSamples;
                    REQUIRE(inserted.first->second.size() == grid.layers.size());
                    for (size_t layer = 0; layer < grid.layers.size(); ++layer)
                        CHECK(std::bit_cast<uint32_t>(inserted.first->second[layer]) ==
                              std::bit_cast<uint32_t>(grid.At(layer, i, k)));
                }
            }
    }
    CHECK(sharedSamples > 0);

    const auto first = Evaluate(recipe, byTile, {-1, -1});
    const auto second = Evaluate(recipe, byTile, {-1, -1});
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->layers == second->layers);
    CHECK(SameBits(*first, *second));
}

TEST_CASE("An edit layer moves height-rule weights only in its influence", "[terraintool][material][weights]")
{
    const std::string text = RecipeText("games/TerrainCheck/assets/terrain/relief-2x2.terrain.json");
    const TerrainRecipe plain = Valid(Json::parse(text));

    TerrainReport plainReport("weights", {});
    std::optional<std::vector<TileHeights>> baselineTiles = BakeFinalHeights(plain, plainReport);
    REQUIRE(baselineTiles);

    const TerrainLayout &layout = plain.layout;
    const int32_t intervals = layout.IntervalsPerTile();
    const int64_t firstX = layout.TileFirstSample(-1);
    const int64_t firstZ = layout.TileFirstSample(-1);
    const TileHeights &baselineTile = *ByTile(*baselineTiles).at({-1, -1});
    float maxHeight = baselineTile.Samples().front();
    for (float height : baselineTile.Samples())
        maxHeight = std::max(maxHeight, height);

    Json dressedJson = Json::parse(text);
    dressedJson["materials"] =
        Layers({Json{{"name", "base"}},
                Json{{"name", "sand"},
                     {"rules", Json{{"height", HeightRule(double(maxHeight) + 25.0, 10000.0, 0.0)}}}}});
    const TerrainRecipe dressed = Valid(dressedJson);

    TerrainEditLayer edit;
    edit.info.name = "raise";
    edit.info.heightBlend = TerrainEditBlend::Additive;
    for (int64_t dz = 0; dz < intervals; ++dz)
        for (int64_t dx = 0; dx < intervals; ++dx)
            SetTerrainEditSample(edit, layout, {firstX + dx, firstZ + dz}, 200.0f);
    REQUIRE(edit.tiles.size() == 1);

    TerrainRecipe edited = dressed;
    edited.editStack = std::make_shared<const std::vector<TerrainEditLayer>>(std::vector<TerrainEditLayer>{edit});
    TerrainReport editedReport("weights", {});
    std::optional<std::vector<TileHeights>> editedTiles = BakeFinalHeights(edited, editedReport);
    REQUIRE(editedTiles);

    const auto baseline = ByTile(*baselineTiles);
    const auto lifted = ByTile(*editedTiles);
    const int32_t side = intervals + 1;
    size_t changed = 0;
    size_t changedOutside = 0;
    const double originX = layout.SamplePosition(firstX);
    const double originZ = layout.SamplePosition(firstZ);
    const double reach = 32.0;
    for (int32_t z = layout.Tiles().min.z; z <= layout.Tiles().max.z; ++z)
        for (int32_t x = layout.Tiles().min.x; x <= layout.Tiles().max.x; ++x)
        {
            const auto before = Evaluate(dressed, baseline, {x, z});
            const auto after = Evaluate(edited, lifted, {x, z});
            REQUIRE(before);
            REQUIRE(after);
            for (int32_t k = 0; k < side; ++k)
                for (int32_t i = 0; i < side; ++i)
                {
                    bool differs = false;
                    for (size_t layer = 0; layer < before->layers.size(); ++layer)
                        if (std::bit_cast<uint32_t>(before->At(layer, i, k)) !=
                            std::bit_cast<uint32_t>(after->At(layer, i, k)))
                            differs = true;
                    if (!differs)
                        continue;
                    ++changed;
                    const double px = layout.SamplePosition(layout.TileFirstSample(x) + i);
                    const double pz = layout.SamplePosition(layout.TileFirstSample(z) + k);
                    const double size = layout.TileSize();
                    if (px < originX - reach || px > originX + size + reach || pz < originZ - reach ||
                        pz > originZ + size + reach)
                        ++changedOutside;
                }
        }
    CHECK(changed > 0);
    CHECK(changedOutside == 0);

    const auto beforeCenter = Evaluate(dressed, baseline, {-1, -1});
    const auto afterCenter = Evaluate(edited, lifted, {-1, -1});
    REQUIRE(beforeCenter);
    REQUIRE(afterCenter);
    CHECK(beforeCenter->At(1, intervals / 2, intervals / 2) == 0.0f);
    CHECK(afterCenter->At(1, intervals / 2, intervals / 2) == 1.0f);
}
