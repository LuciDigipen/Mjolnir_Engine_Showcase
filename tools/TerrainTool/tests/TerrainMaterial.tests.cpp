/**
 * @file TerrainMaterial.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainMaterial parsing and weight identity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <TerrainMaterial.h>
#include <TerrainRecipe.h>

#include <terrain/TerrainRecipeMaterials.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;

namespace
{
    using Json = nlohmann::json;

    const std::string kExamplePath = std::string(TERRAIN_FIXTURE_DIR) + "/relief-2x2.terrain.json";

    Json Example()
    {
        std::ifstream in(kExamplePath);
        return Json::parse(in);
    }

    std::string RecipeText(std::string_view relative)
    {
        std::ifstream in(std::string(TERRAIN_PROJECT_DIR) + "/" + std::string(relative), std::ios::binary);
        REQUIRE(in);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    RecipeParseResult Parse(const Json &json)
    {
        return ParseTerrainRecipe(json.dump());
    }

    TerrainRecipe Valid(const Json &json)
    {
        RecipeParseResult result = Parse(json);
        for (const RecipeIssue &issue : result.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(result.recipe);
        return *result.recipe;
    }

    bool HasIssue(const RecipeParseResult &result, std::string_view field, std::string_view text = "")
    {
        for (const RecipeIssue &issue : result.issues)
            if (issue.field == field && issue.message.find(text) != std::string::npos)
                return true;
        return false;
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

    Json Layer(std::string name, const Json &rules)
    {
        Json layer = Json::object();
        layer["name"] = std::move(name);
        layer["rules"] = rules;
        return layer;
    }

    Json Materials(const Json &first, const Json &second)
    {
        Json materials = Json::object();
        materials["layers"] = Json::array({first, second});
        return materials;
    }

    Json RuleMaterials(const Json &rules)
    {
        return Materials(Json{{"name", "base"}}, Layer("sand", rules));
    }

    Json ExampleWith(const Json &materials)
    {
        Json json = Example();
        json["materials"] = materials;
        return json;
    }

    uint64_t WeightHash(const TerrainRecipe &recipe)
    {
        return FingerprintWeights(recipe, FingerprintRecipe(recipe)).value;
    }
}

TEST_CASE("A recipe accepts material layers with every rule", "[terraintool][recipe][material]")
{
    Json json = Example();
    json["materials"] = Materials(
        Json{{"name", "grass"}},
        Layer("sand", Json{{"height", HeightRule(1.0, 5.0, 2.0)},
                           {"slope", SlopeRule(0.0, 30.0, 5.0)},
                           {"noise", NoiseRule(16.0, 3, 0.5)}}));
    json["materials"]["layers"].push_back(Json{{"name", "rock_2"}});

    const TerrainRecipe recipe = Valid(json);
    REQUIRE(recipe.materials.size() == 3);
    CHECK(recipe.materials[0].name == "grass");
    CHECK_FALSE(recipe.materials[0].rules.height);
    CHECK_FALSE(recipe.materials[0].rules.slope);
    CHECK_FALSE(recipe.materials[0].rules.noise);

    const TerrainMaterialRules &rules = recipe.materials[1].rules;
    REQUIRE(rules.height);
    CHECK(rules.height->min == 1.0);
    CHECK(rules.height->max == 5.0);
    CHECK(rules.height->blend == 2.0);
    REQUIRE(rules.slope);
    CHECK(rules.slope->min == 0.0);
    CHECK(rules.slope->max == 30.0);
    CHECK(rules.slope->blend == 5.0);
    REQUIRE(rules.noise);
    CHECK(rules.noise->wavelength == 16.0);
    CHECK(rules.noise->octaves == 3);
    CHECK(rules.noise->strength == 0.5);
    CHECK(recipe.materials[2].name == "rock_2");

    const TerrainRecipe plain = Valid(Example());
    CHECK(CanonicalRecipeText(recipe) == CanonicalRecipeText(plain));
    CHECK(FingerprintRecipe(recipe) == FingerprintRecipe(plain));
}

TEST_CASE("A recipe without materials has one fallback layer", "[terraintool][recipe][material]")
{
    const TerrainRecipe recipe = Valid(Example());
    REQUIRE(recipe.materials.size() == 1);
    CHECK(recipe.materials[0].name == "base");
    CHECK_FALSE(recipe.materials[0].rules.height);
    CHECK_FALSE(recipe.materials[0].rules.slope);
    CHECK_FALSE(recipe.materials[0].rules.noise);
}

TEST_CASE("Material issues are reported by field path", "[terraintool][recipe][material]")
{
    const auto parse = [](const Json &materials)
    {
        return Parse(ExampleWith(materials));
    };

    SECTION("materials is not an object")
    {
        const RecipeParseResult result = parse(Json(3));
        CHECK(HasIssue(result, "materials", "expected an object"));
    }
    SECTION("layers is missing")
    {
        const RecipeParseResult result = parse(Json::object());
        CHECK(HasIssue(result, "materials.layers", "missing"));
    }
    SECTION("layers is empty")
    {
        const RecipeParseResult result = parse(Json{{"layers", Json::array()}});
        CHECK(HasIssue(result, "materials.layers", "at least one"));
    }
    SECTION("layers is not an array")
    {
        const RecipeParseResult result = parse(Json{{"layers", 4}});
        CHECK(HasIssue(result, "materials.layers", "expected an array"));
    }
    SECTION("unknown field in materials")
    {
        Json materials = RuleMaterials(Json{{"height", HeightRule(1.0, 5.0, 2.0)}});
        materials["shading"] = true;
        const RecipeParseResult result = parse(materials);
        CHECK(HasIssue(result, "materials.shading", "unknown field"));
    }
    SECTION("unknown field in a layer")
    {
        Json sand = Layer("sand", Json{{"height", HeightRule(1.0, 5.0, 2.0)}});
        sand["colour"] = "red";
        const RecipeParseResult result = parse(Materials(Json{{"name", "base"}}, sand));
        CHECK(HasIssue(result, "materials.layers[1].colour", "unknown field"));
    }
    SECTION("unknown field in a rule")
    {
        const RecipeParseResult result =
            parse(RuleMaterials(Json{{"height", Json{{"min", 1.0}, {"max", 5.0}, {"blend", 0.0}, {"tint", 2.0}}}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.tint", "unknown field"));
    }
    SECTION("a layer has no name")
    {
        const RecipeParseResult result = parse(Materials(Json{{"name", "base"}}, Json{{"rules", Json::object()}}));
        CHECK(HasIssue(result, "materials.layers[1].name", "missing"));
    }
    SECTION("a layer name is not lowercase")
    {
        const RecipeParseResult result =
            parse(Materials(Json{{"name", "base"}}, Json{{"name", "Sand"}}));
        CHECK(HasIssue(result, "materials.layers[1].name", "[a-z0-9_]"));
    }
    SECTION("a layer name is too long")
    {
        const RecipeParseResult result =
            parse(Materials(Json{{"name", "base"}}, Json{{"name", std::string(33, 'a')}}));
        CHECK(HasIssue(result, "materials.layers[1].name", "[a-z0-9_]"));
    }
    SECTION("layer names repeat")
    {
        const RecipeParseResult result = parse(Materials(Json{{"name", "base"}}, Json{{"name", "base"}}));
        CHECK(HasIssue(result, "materials.layers[1].name", "duplicate"));
    }
    SECTION("the fallback layer takes no rules")
    {
        Json materials = Materials(Json{{"name", "base"}}, Json{{"name", "sand"}});
        materials["layers"][0]["rules"] = Json{{"height", HeightRule(1.0, 5.0, 2.0)}};
        const RecipeParseResult result = parse(materials);
        CHECK(HasIssue(result, "materials.layers[0].rules", "fallback"));
    }
    SECTION("a rule object has all of its fields")
    {
        const RecipeParseResult result =
            parse(RuleMaterials(Json{{"height", Json{{"min", 1.0}, {"max", 5.0}}}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.blend", "missing"));
    }
    SECTION("a height range is ordered")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"height", HeightRule(5.0, 1.0, 0.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height", "less than or equal to max"));
    }
    SECTION("a height value is bounded")
    {
        const RecipeParseResult result =
            parse(RuleMaterials(Json{{"height", HeightRule(-20000.0, 5.0, 0.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.min", "out of range"));
    }
    SECTION("a height blend is not negative")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"height", HeightRule(1.0, 5.0, -1.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.blend", "greater than or equal to 0"));
    }
    SECTION("a slope range is ordered")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"slope", SlopeRule(40.0, 20.0, 0.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.slope", "less than or equal to max"));
    }
    SECTION("a slope is at most 90 degrees")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"slope", SlopeRule(0.0, 91.0, 0.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.slope.max", "out of range"));
    }
    SECTION("a slope is not negative")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"slope", SlopeRule(-1.0, 20.0, 0.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.slope.min", "out of range"));
    }
    SECTION("a slope blend is not negative")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"slope", SlopeRule(0.0, 20.0, -2.0)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.slope.blend", "greater than or equal to 0"));
    }
    SECTION("a noise wavelength has a lower bound")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"noise", NoiseRule(0.005, 2, 0.5)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.noise.wavelength", "out of range"));
    }
    SECTION("a noise wavelength has an upper bound")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"noise", NoiseRule(1.0e7, 2, 0.5)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.noise.wavelength", "out of range"));
    }
    SECTION("noise octaves are bounded")
    {
        const RecipeParseResult result = parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 9, 0.5)}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.noise.octaves", "out of range"));
    }
    SECTION("a noise strength is in [0, 1]")
    {
        CHECK(HasIssue(parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 2, 1.5)}})),
                       "materials.layers[1].rules.noise.strength", "out of range"));
        CHECK(HasIssue(parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 2, -0.5)}})),
                       "materials.layers[1].rules.noise.strength", "out of range"));
    }
    SECTION("at most 32 layers")
    {
        Json layers = Json::array({Json{{"name", "base"}}});
        for (int i = 0; i < 32; ++i)
            layers.push_back(Json{{"name", "layer_" + std::to_string(i)}});
        const RecipeParseResult result = parse(Json{{"layers", layers}});
        CHECK(HasIssue(result, "materials.layers", "exceed the limit"));
    }
}

TEST_CASE("Materials leave the game recipes' height identity alone", "[terraintool][recipe][material]")
{
    const std::pair<const char *, const char *> recipes[] = {
        {"TerrainCheck relief-2x2", "games/TerrainCheck/assets/terrain/relief-2x2.terrain.json"},
        {"TerrainCheck streamed-16x16", "games/TerrainCheck/assets/terrain/streamed-16x16.terrain.json"},
        {"CalderaDemo caldera", "games/CalderaDemo/assets/terrain/caldera.terrain.json"},
    };

    for (const auto &[name, relative] : recipes)
    {
        INFO(name);
        const std::string text = RecipeText(relative);
        const RecipeParseResult plain = ParseTerrainRecipe(text);
        for (const RecipeIssue &issue : plain.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(plain.recipe);

        Json json = Json::parse(text);
        json["materials"] = RuleMaterials(Json{{"height", HeightRule(1.0, 100.0, 10.0)},
                                               {"slope", SlopeRule(10.0, 60.0, 5.0)},
                                               {"noise", NoiseRule(32.0, 4, 0.5)}});
        const RecipeParseResult dressed = ParseTerrainRecipe(json.dump());
        for (const RecipeIssue &issue : dressed.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(dressed.recipe);

        CHECK(CanonicalRecipeText(*dressed.recipe) == CanonicalRecipeText(*plain.recipe));
        CHECK(FingerprintRecipe(*dressed.recipe) == FingerprintRecipe(*plain.recipe));
        CHECK(WeightHash(*dressed.recipe) != WeightHash(*plain.recipe));
    }
}

TEST_CASE("The weight fingerprint follows material fields, order and heights", "[terraintool][recipe][material]")
{
    const TerrainRecipe plain = Valid(Example());
    const TerrainSourceFingerprint heights = FingerprintRecipe(plain);
    CHECK_FALSE(FingerprintWeights(plain, heights).IsNull());

    const Json sandRules = Json{{"height", HeightRule(1.0, 5.0, 2.0)},
                                {"slope", SlopeRule(0.0, 30.0, 5.0)},
                                {"noise", NoiseRule(16.0, 3, 0.5)}};
    Json dressedJson = Example();
    dressedJson["materials"] = Materials(Json{{"name", "base"}}, Layer("sand", sandRules));
    const TerrainRecipe dressed = Valid(dressedJson);
    const uint64_t dressedHash = WeightHash(dressed);
    CHECK(dressedHash != WeightHash(plain));

    SECTION("any material field")
    {
        Json edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["height"]["min"] = 2.0;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["height"]["blend"] = 4.0;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["slope"]["max"] = 40.0;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["slope"]["blend"] = 10.0;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["noise"]["wavelength"] = 8.0;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["noise"]["octaves"] = 2;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["rules"]["noise"]["strength"] = 0.25;
        CHECK(WeightHash(Valid(edited)) != dressedHash);

        edited = dressedJson;
        edited["materials"]["layers"][1]["name"] = "rock";
        CHECK(WeightHash(Valid(edited)) != dressedHash);
    }
    SECTION("layer order")
    {
        Json layered = dressedJson;
        layered["materials"]["layers"].push_back(Json{{"name", "rock"}});
        const uint64_t before = WeightHash(Valid(layered));
        std::swap(layered["materials"]["layers"][1], layered["materials"]["layers"][2]);
        CHECK(WeightHash(Valid(layered)) != before);
    }
    SECTION("the height fingerprint")
    {
        Json reseeded = dressedJson;
        reseeded["seed"] = 20261002u;
        const TerrainRecipe other = Valid(reseeded);
        CHECK(FingerprintRecipe(other) != FingerprintRecipe(dressed));
        CHECK(WeightHash(other) != dressedHash);
    }
    SECTION("not JSON spelling or whitespace")
    {
        Json respelled = dressedJson;
        respelled["materials"]["layers"][1]["rules"]["height"]["min"] = 1;
        respelled["materials"]["layers"][1]["rules"]["noise"]["strength"] = 5e-1;
        const RecipeParseResult compact = ParseTerrainRecipe(respelled.dump());
        const RecipeParseResult spaced = ParseTerrainRecipe(respelled.dump(4));
        REQUIRE(compact.recipe);
        REQUIRE(spaced.recipe);
        CHECK(WeightHash(*compact.recipe) == dressedHash);
        CHECK(WeightHash(*spaced.recipe) == dressedHash);
    }
    SECTION("one explicit base layer matches no materials section")
    {
        Json explicitBase = Example();
        explicitBase["materials"] = Json{{"layers", Json::array({Json{{"name", "base"}}})}};
        const TerrainRecipe withBase = Valid(explicitBase);
        CHECK(FingerprintRecipe(withBase) == heights);
        CHECK(FingerprintWeights(withBase, FingerprintRecipe(withBase)) == FingerprintWeights(plain, heights));
    }
    SECTION("the edit weight stack hash")
    {
        TerrainRecipe withWeights = dressed;
        CHECK(FingerprintWeights(withWeights, heights).value == dressedHash);

        withWeights.editWeightStackHash = 0x5eed;
        CHECK(FingerprintWeights(withWeights, heights).value != dressedHash);

        TerrainRecipe withHeightEdit = dressed;
        withHeightEdit.editStackHash = 0x5eed;
        CHECK(FingerprintWeights(withHeightEdit, heights).value == dressedHash);
        CHECK(FingerprintRecipe(withHeightEdit) != heights);
    }
}

TEST_CASE("A rewritten recipe keeps its recipe and weight identity", "[terraintool][recipe][material]")
{
    const std::string text = RecipeText("games/TerrainCheck/assets/terrain/relief-2x2.terrain.json");
    const RecipeParseResult original = ParseTerrainRecipe(text);
    for (const RecipeIssue &issue : original.issues)
        UNSCOPED_INFO(issue.field << ": " << issue.message);
    REQUIRE(original.recipe);

    const TerrainRecipeMaterialsWriteResult rewritten =
        ReplaceTerrainRecipeMaterials(text, original.recipe->materials);
    REQUIRE(rewritten.text);
    const RecipeParseResult reparsed = ParseTerrainRecipe(*rewritten.text);
    for (const RecipeIssue &issue : reparsed.issues)
        UNSCOPED_INFO(issue.field << ": " << issue.message);
    REQUIRE(reparsed.recipe);

    CHECK(reparsed.recipe->materials == original.recipe->materials);
    CHECK(CanonicalRecipeText(*reparsed.recipe) == CanonicalRecipeText(*original.recipe));
    CHECK(FingerprintRecipe(*reparsed.recipe) == FingerprintRecipe(*original.recipe));
    CHECK(FingerprintWeights(*reparsed.recipe, FingerprintRecipe(*reparsed.recipe)) ==
          FingerprintWeights(*original.recipe, FingerprintRecipe(*original.recipe)));
}
