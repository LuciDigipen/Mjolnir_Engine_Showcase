/**
 * @file TerrainRecipeMaterials.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainRecipeMaterials.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainRecipeMaterials.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    using Json = nlohmann::json;
    using OrderedJson = nlohmann::ordered_json;

    OrderedJson Example()
    {
        std::ifstream in(TERRAIN_MATERIAL_RECIPE, std::ios::binary);
        REQUIRE(in);
        return OrderedJson::parse(in);
    }

    std::string ExampleText()
    {
        std::ifstream in(TERRAIN_MATERIAL_RECIPE, std::ios::binary);
        REQUIRE(in);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    TerrainRecipeMaterialsParseResult Parse(const Json &materials)
    {
        return ParseTerrainRecipeMaterials(&materials);
    }

    bool HasIssue(const TerrainRecipeMaterialsParseResult &result, std::string_view path, std::string_view message)
    {
        for (const TerrainRecipeMaterialIssue &issue : result.issues)
            if (issue.path == path && issue.message == message)
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

    Json NoiseRule(double wavelength, Json octaves, double strength)
    {
        return Json{{"wavelength", wavelength}, {"octaves", std::move(octaves)}, {"strength", strength}};
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
        return Json{{"layers", Json::array({first, second})}};
    }

    Json RuleMaterials(const Json &rules)
    {
        return Materials(Json{{"name", "base"}}, Layer("sand", rules));
    }

    std::vector<std::string> Keys(const OrderedJson &object)
    {
        std::vector<std::string> keys;
        for (auto it = object.begin(); it != object.end(); ++it)
            keys.push_back(it.key());
        return keys;
    }
}

TEST_CASE("ParseTerrainRecipeMaterials reads the relief-2x2 layers", "[terrain][recipe][materials]")
{
    const OrderedJson recipe = Example();
    const Json materials = recipe.at("materials");

    const TerrainRecipeMaterialsParseResult result = Parse(materials);
    CHECK(result.issues.empty());
    REQUIRE(result.layers.size() == 2);
    CHECK(result.layers[0].name == "sand");
    CHECK_FALSE(result.layers[0].rules.height);
    CHECK_FALSE(result.layers[0].rules.slope);
    CHECK_FALSE(result.layers[0].rules.noise);

    const TerrainMaterialRules &rules = result.layers[1].rules;
    CHECK(result.layers[1].name == "dark_sand");
    CHECK_FALSE(rules.height);
    REQUIRE(rules.slope);
    CHECK(rules.slope->min == 20.0);
    CHECK(rules.slope->max == 90.0);
    CHECK(rules.slope->blend == 4.0);
    REQUIRE(rules.noise);
    CHECK(rules.noise->wavelength == 96.0);
    CHECK(rules.noise->octaves == 2);
    CHECK(rules.noise->strength == 0.35);

    CHECK(IsValidTerrainMaterialName("sand"));
    CHECK_FALSE(IsValidTerrainMaterialName("Sand"));
}

TEST_CASE("A recipe without materials parses to one fallback layer", "[terrain][recipe][materials]")
{
    const TerrainRecipeMaterialsParseResult result = ParseTerrainRecipeMaterials(nullptr);
    CHECK(result.issues.empty());
    REQUIRE(result.layers.size() == 1);
    CHECK(result.layers[0].name == "base");
    CHECK_FALSE(result.layers[0].rules.height);
    CHECK_FALSE(result.layers[0].rules.slope);
    CHECK_FALSE(result.layers[0].rules.noise);
}

TEST_CASE("Material issues keep their paths and messages", "[terrain][recipe][materials]")
{
    SECTION("materials is not an object")
    {
        CHECK(HasIssue(Parse(Json(3)), "materials", "expected an object, got 3"));
    }
    SECTION("layers is missing")
    {
        CHECK(HasIssue(Parse(Json::object()), "materials.layers", "missing required field"));
    }
    SECTION("layers is not an array")
    {
        CHECK(HasIssue(Parse(Json{{"layers", 4}}), "materials.layers", "expected an array, got 4"));
    }
    SECTION("layers is empty")
    {
        CHECK(HasIssue(Parse(Json{{"layers", Json::array()}}), "materials.layers",
                       "at least one layer is required"));
    }
    SECTION("layers exceed the limit")
    {
        Json layers = Json::array({Json{{"name", "base"}}});
        for (int i = 0; i < 32; ++i)
            layers.push_back(Json{{"name", "layer_" + std::to_string(i)}});
        CHECK(HasIssue(Parse(Json{{"layers", layers}}), "materials.layers",
                       "33 layers exceed the limit 32"));
    }
    SECTION("an unknown field in materials")
    {
        Json materials = RuleMaterials(Json{{"height", HeightRule(1.0, 5.0, 2.0)}});
        materials["shading"] = true;
        CHECK(HasIssue(Parse(materials), "materials.shading", "unknown field (value true)"));
    }
    SECTION("an unknown field in a layer")
    {
        Json sand = Layer("sand", Json{{"height", HeightRule(1.0, 5.0, 2.0)}});
        sand["colour"] = "red";
        CHECK(HasIssue(Parse(Materials(Json{{"name", "base"}}, sand)), "materials.layers[1].colour",
                       "unknown field (value \"red\")"));
    }
    SECTION("an unknown field in a rule")
    {
        const TerrainRecipeMaterialsParseResult result =
            Parse(RuleMaterials(Json{{"height", Json{{"min", 1.0}, {"max", 5.0}, {"blend", 0.0}, {"tint", 2.0}}}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.tint", "unknown field (value 2.0)"));
    }
    SECTION("a layer has no name")
    {
        CHECK(HasIssue(Parse(Materials(Json{{"name", "base"}}, Json{{"rules", Json::object()}})),
                       "materials.layers[1].name", "missing required field"));
    }
    SECTION("a layer name is not lowercase")
    {
        CHECK(HasIssue(Parse(Materials(Json{{"name", "base"}}, Json{{"name", "Sand"}})),
                       "materials.layers[1].name", "\"Sand\" must be 1 to 32 characters of [a-z0-9_]"));
    }
    SECTION("a layer name is too long")
    {
        CHECK(HasIssue(Parse(Materials(Json{{"name", "base"}}, Json{{"name", std::string(33, 'a')}})),
                       "materials.layers[1].name",
                       "\"" + std::string(33, 'a') + "\" must be 1 to 32 characters of [a-z0-9_]"));
    }
    SECTION("layer names repeat")
    {
        CHECK(HasIssue(Parse(Materials(Json{{"name", "base"}}, Json{{"name", "base"}})),
                       "materials.layers[1].name", "duplicate layer name \"base\""));
    }
    SECTION("the fallback layer takes no rules")
    {
        Json materials = Materials(Json{{"name", "base"}}, Json{{"name", "sand"}});
        materials["layers"][0]["rules"] = Json{{"height", HeightRule(1.0, 5.0, 2.0)}};
        CHECK(HasIssue(Parse(materials), "materials.layers[0].rules",
                       "the first layer is the fallback and takes no rules"));
    }
    SECTION("a rule object has all of its fields")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", Json{{"min", 1.0}, {"max", 5.0}}}})),
                       "materials.layers[1].rules.height.blend", "missing required field"));
    }
    SECTION("a height value is a number")
    {
        const TerrainRecipeMaterialsParseResult result =
            Parse(RuleMaterials(Json{{"height", Json{{"min", "low"}, {"max", 5.0}, {"blend", 0.0}}}}));
        CHECK(HasIssue(result, "materials.layers[1].rules.height.min", "expected a number, got \"low\""));
    }
    SECTION("a height value is finite")
    {
        Json rule = HeightRule(1.0, 5.0, 0.0);
        rule["min"] = std::numeric_limits<double>::infinity();
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", rule}})), "materials.layers[1].rules.height.min",
                       "inf is not a finite number"));
    }
    SECTION("a height value is exact")
    {
        Json rule = HeightRule(1.0, 5.0, 0.0);
        rule["min"] = std::numeric_limits<uint64_t>::max();
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", rule}})), "materials.layers[1].rules.height.min",
                       "18446744073709551615 is not exactly representable as a double"));
    }
    SECTION("a height range is ordered")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", HeightRule(5.0, 1.0, 0.0)}})),
                       "materials.layers[1].rules.height", "min 5 must be less than or equal to max 1"));
    }
    SECTION("a height value is bounded")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", HeightRule(-20000.0, 5.0, 0.0)}})),
                       "materials.layers[1].rules.height.min", "-20000 is out of range [-10000, 10000] m"));
    }
    SECTION("a height blend is not negative")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"height", HeightRule(1.0, 5.0, -1.0)}})),
                       "materials.layers[1].rules.height.blend", "-1 must be greater than or equal to 0"));
    }
    SECTION("a slope range is ordered")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"slope", SlopeRule(40.0, 20.0, 0.0)}})),
                       "materials.layers[1].rules.slope", "min 40 must be less than or equal to max 20"));
    }
    SECTION("a slope is at most 90 degrees")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"slope", SlopeRule(0.0, 91.0, 0.0)}})),
                       "materials.layers[1].rules.slope.max", "91 is out of range [0, 90] deg"));
    }
    SECTION("a slope is not negative")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"slope", SlopeRule(-1.0, 20.0, 0.0)}})),
                       "materials.layers[1].rules.slope.min", "-1 is out of range [0, 90] deg"));
    }
    SECTION("a slope blend is not negative")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"slope", SlopeRule(0.0, 20.0, -2.0)}})),
                       "materials.layers[1].rules.slope.blend", "-2 must be greater than or equal to 0"));
    }
    SECTION("a noise wavelength has bounds")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"noise", NoiseRule(0.005, 2, 0.5)}})),
                       "materials.layers[1].rules.noise.wavelength", "0.005 is out of range [0.01, 1e+06] m"));
    }
    SECTION("noise octaves are integral and bounded")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 2.5, 0.5)}})),
                       "materials.layers[1].rules.noise.octaves", "expected an integer, got 2.5"));
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 9, 0.5)}})),
                       "materials.layers[1].rules.noise.octaves", "9 is out of range [1, 8]"));
    }
    SECTION("a noise strength is in [0, 1]")
    {
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 2, 1.5)}})),
                       "materials.layers[1].rules.noise.strength", "1.5 is out of range [0, 1]"));
        CHECK(HasIssue(Parse(RuleMaterials(Json{{"noise", NoiseRule(16.0, 2, -0.5)}})),
                       "materials.layers[1].rules.noise.strength", "-0.5 is out of range [0, 1]"));
    }
}

TEST_CASE("TerrainRecipeMaterialsToJson writes the on-disk materials section", "[terrain][recipe][materials]")
{
    std::vector<TerrainRecipeMaterialLayer> layers;
    layers.push_back({"sand", {}});
    TerrainMaterialRules dark;
    dark.slope = TerrainMaterialSlopeRule{20.0, 90.0, 4.0};
    dark.noise = TerrainMaterialNoiseRule{96.0, 2, 0.35};
    layers.push_back({"dark_sand", dark});

    const OrderedJson written = TerrainRecipeMaterialsToJson(layers);
    CHECK(written == Example().at("materials"));
    CHECK(written.dump(2) == Example().at("materials").dump(2));

    std::vector<TerrainRecipeMaterialLayer> ruled;
    ruled.push_back({"base", {}});
    TerrainMaterialRules height;
    height.height = TerrainMaterialHeightRule{1.0, 5.0, 2.0};
    ruled.push_back({"ridge", height});
    CHECK(TerrainRecipeMaterialsToJson(ruled).dump() ==
          R"({"layers":[{"name":"base"},{"name":"ridge","rules":{"height":{"min":1.0,"max":5.0,"blend":2.0}}}]})");
}

TEST_CASE("Material layers survive a writer round trip", "[terrain][recipe][materials]")
{
    std::vector<TerrainRecipeMaterialLayer> layers;
    layers.push_back({"base", {}});

    TerrainMaterialRules every;
    every.height = TerrainMaterialHeightRule{1.0, 5.0, 2.0};
    every.slope = TerrainMaterialSlopeRule{0.0, 30.0, 5.0};
    every.noise = TerrainMaterialNoiseRule{16.0, 3, 0.5};
    layers.push_back({"sand", every});

    TerrainMaterialRules height;
    height.height = TerrainMaterialHeightRule{-10.0, 10.0, 0.0};
    layers.push_back({"ridge", height});

    TerrainMaterialRules slope;
    slope.slope = TerrainMaterialSlopeRule{10.0, 60.0, 5.0};
    layers.push_back({"cliff", slope});

    TerrainMaterialRules noise;
    noise.noise = TerrainMaterialNoiseRule{32.0, 4, 0.25};
    layers.push_back({"dunes", noise});

    const Json written = TerrainRecipeMaterialsToJson(layers);
    const TerrainRecipeMaterialsParseResult reparsed = Parse(written);
    CHECK(reparsed.issues.empty());
    CHECK(reparsed.layers == layers);
}

TEST_CASE("ReplaceTerrainRecipeMaterials replaces only the materials section", "[terrain][recipe][materials]")
{
    const std::string text = ExampleText();
    const OrderedJson whole = OrderedJson::parse(text);
    const Json originalMaterials = whole.at("materials");
    const TerrainRecipeMaterialsParseResult parsed = Parse(originalMaterials);
    REQUIRE(parsed.issues.empty());

    SECTION("a recipe without materials gains the section after parameters")
    {
        OrderedJson without = whole;
        without.erase("materials");
        const TerrainRecipeMaterialsWriteResult result =
            ReplaceTerrainRecipeMaterials(without.dump(2) + "\n", parsed.layers);
        CHECK(result.error.empty());
        REQUIRE(result.text);
        CHECK(result.text->back() == '\n');

        const OrderedJson rewritten = OrderedJson::parse(*result.text);
        CHECK(rewritten.at("materials") == TerrainRecipeMaterialsToJson(parsed.layers));

        std::vector<std::string> expectedKeys;
        for (auto it = without.begin(); it != without.end(); ++it)
        {
            expectedKeys.push_back(it.key());
            if (it.key() == "parameters")
                expectedKeys.push_back("materials");
        }
        CHECK(Keys(rewritten) == expectedKeys);
        for (const auto &[key, value] : without.items())
            CHECK(rewritten.at(key) == value);
    }
    SECTION("a recipe with materials keeps its position and every other value")
    {
        const std::vector<TerrainRecipeMaterialLayer> replacement = {{"base", {}}};
        const TerrainRecipeMaterialsWriteResult result = ReplaceTerrainRecipeMaterials(text, replacement);
        CHECK(result.error.empty());
        REQUIRE(result.text);

        const OrderedJson rewritten = OrderedJson::parse(*result.text);
        CHECK(Keys(rewritten) == Keys(whole));
        for (const auto &[key, value] : whole.items())
            if (key != "materials")
                CHECK(rewritten.at(key) == value);

        const Json writtenMaterials = rewritten.at("materials");
        const TerrainRecipeMaterialsParseResult reread = Parse(writtenMaterials);
        CHECK(reread.issues.empty());
        CHECK(reread.layers == replacement);
    }
    SECTION("invalid layers are refused with the first issue")
    {
        TerrainMaterialRules rules;
        rules.height = TerrainMaterialHeightRule{5.0, 1.0, 0.0};
        const std::vector<TerrainRecipeMaterialLayer> layers = {{"base", rules}};
        const TerrainRecipeMaterialsWriteResult result = ReplaceTerrainRecipeMaterials(text, layers);
        CHECK_FALSE(result.text);
        CHECK(result.error == "materials.layers[0].rules: the first layer is the fallback and takes no rules");

        const TerrainRecipeMaterialsWriteResult empty = ReplaceTerrainRecipeMaterials(text, {});
        CHECK_FALSE(empty.text);
        CHECK(empty.error == "materials.layers: at least one layer is required");
    }
    SECTION("a recipe that is not valid JSON is refused")
    {
        const TerrainRecipeMaterialsWriteResult result = ReplaceTerrainRecipeMaterials("{", parsed.layers);
        CHECK_FALSE(result.text);
        CHECK(result.error.rfind("not valid JSON: ", 0) == 0);
    }
}
