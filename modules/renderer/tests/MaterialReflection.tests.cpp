/**
 * @file MaterialReflection.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/MaterialReflection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialReflection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace mir;

namespace
{
    constexpr const char *kReflection = R"({
        "entryPoints": [{
            "name": "layoutMain",
            "parameters": [{
                "name": "layout",
                "type": {
                    "kind": "resource",
                    "baseShape": "structuredBuffer",
                    "resultType": {
                        "kind": "struct",
                        "name": "ProbeMaterial",
                        "fields": [
                            { "name": "amount", "type": { "kind": "scalar", "scalarType": "float32" },
                              "binding": { "kind": "uniform", "offset": 0, "size": 4 } },
                            { "name": "color", "type": { "kind": "vector", "elementCount": 3,
                                "elementType": { "kind": "scalar", "scalarType": "float32" } },
                              "binding": { "kind": "uniform", "offset": 16, "size": 12 } },
                            { "name": "texture", "type": { "kind": "struct", "name": "TextureRef", "fields": [
                                { "name": "image", "type": { "kind": "scalar", "scalarType": "uint32" },
                                  "binding": { "kind": "uniform", "offset": 0, "size": 4 } } ] },
                              "binding": { "kind": "uniform", "offset": 28, "size": 4 } }
                        ]
                    }
                }
            }]
        }]
    })";
}

TEST_CASE("Material reflection exposes the typed witness fields", "[renderer][material][reflection]")
{
    std::string error;
    const auto layout = ParseMaterialLayout(kReflection, &error);
    INFO(error);
    REQUIRE(layout.has_value());
    REQUIRE(layout->fields.size() == 3);
    CHECK(layout->fields[0].name == "amount");
    CHECK(layout->fields[0].offset == 0);
    CHECK(layout->fields[0].size == 4);
    CHECK(layout->fields[0].kind == MaterialFieldKind::Float);
    CHECK(layout->fields[1].offset == 16);
    CHECK(layout->fields[1].kind == MaterialFieldKind::Float3);
    CHECK(layout->fields[2].offset == 28);
    CHECK(layout->fields[2].kind == MaterialFieldKind::TextureRef);
    CHECK(layout->size == 32);
}

TEST_CASE("Material parameters pack by reflected field name", "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());
    const auto bytes = PackMaterialRecord(*layout,
        R"({"amount": 1.5, "color": [2.0, 3.0, 4.0], "texture": {"image": 7}})", nullptr);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 32);

    float amount = 0.0f;
    std::memcpy(&amount, bytes->data(), sizeof(amount));
    CHECK(amount == 1.5f);

    float color[3]{};
    std::memcpy(color, bytes->data() + 16, sizeof(color));
    CHECK(color[0] == 2.0f);
    CHECK(color[1] == 3.0f);
    CHECK(color[2] == 4.0f);

    uint32_t image = 0;
    std::memcpy(&image, bytes->data() + 28, sizeof(image));
    CHECK(image == 7);
}

TEST_CASE("Material packing rejects fields it cannot lay out", "[renderer][material][reflection]")
{
    std::string error;
    const auto layout = ParseMaterialLayout(kReflection, &error);
    REQUIRE(layout.has_value());

    CHECK_FALSE(PackMaterialRecord(*layout, R"({"amount": 1.0})", nullptr).has_value());
    CHECK_FALSE(PackMaterialRecord(*layout,
        R"({"amount": 1.0, "color": [1.0, 2.0], "texture": {"image": 1}})", nullptr)
                    .has_value());

    const char *unsupported = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"data","type":{"kind":"array"},"binding":{"offset":0,"size":16}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(unsupported, &error).has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Malformed material layouts reject before packing", "[renderer][material][reflection]")
{
    const char *overflow = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"data","type":{"kind":"scalar","scalarType":"float32"},
        "binding":{"offset":4294967295,"size":8}}]}}}]}]})";
    std::string error;
    CHECK_FALSE(ParseMaterialLayout(overflow, &error).has_value());
    CHECK_FALSE(error.empty());

    const char *overlap = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[
        {"name":"a","type":{"kind":"scalar","scalarType":"float32"},"binding":{"offset":0,"size":4}},
        {"name":"b","type":{"kind":"scalar","scalarType":"float32"},"binding":{"offset":0,"size":4}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(overlap, &error).has_value());

    const char *duplicate = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[
        {"name":"a","type":{"kind":"scalar","scalarType":"float32"},"binding":{"offset":0,"size":4}},
        {"name":"a","type":{"kind":"scalar","scalarType":"float32"},"binding":{"offset":4,"size":4}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(duplicate, &error).has_value());

    const char *wrongSize = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"a","type":{"kind":"vector","elementCount":3,
        "elementType":{"kind":"scalar","scalarType":"float32"}},"binding":{"offset":0,"size":8}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(wrongSize, &error).has_value());
}

TEST_CASE("Material packing rejects wrong JSON types and out-of-range numbers", "[renderer][material][reflection]")
{
    const char *reflection = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[
        {"name":"flag","type":{"kind":"scalar","scalarType":"bool"},"binding":{"offset":0,"size":4}},
        {"name":"index","type":{"kind":"scalar","scalarType":"uint32"},"binding":{"offset":4,"size":4}},
        {"name":"count","type":{"kind":"scalar","scalarType":"int32"},"binding":{"offset":8,"size":4}},
        {"name":"value","type":{"kind":"scalar","scalarType":"float32"},"binding":{"offset":12,"size":4}}]}}}]}]})";
    const auto layout = ParseMaterialLayout(reflection);
    REQUIRE(layout.has_value());

    CHECK_FALSE(PackMaterialRecord(*layout, R"({"flag":"true","index":1,"count":1,"value":1})").has_value());
    CHECK_FALSE(PackMaterialRecord(*layout, R"({"flag":true,"index":-1,"count":1,"value":1})").has_value());
    CHECK_FALSE(PackMaterialRecord(*layout, R"({"flag":true,"index":1,"count":1.5,"value":1})").has_value());
    CHECK_FALSE(PackMaterialRecord(*layout, R"({"flag":true,"index":1,"count":1,"value":1e300})").has_value());
    CHECK_FALSE(PackMaterialRecord(*layout, R"({"flag":true,"index":1,"count":1,"value":1,"extra":1})")
                    .has_value());
}

TEST_CASE("Material shader names carry their namespace", "[renderer][material][reflection]")
{
    const std::string base = MaterialShaderStem("", "DefaultMaterial", "assets/materials/default.slang", "mesh");
    CHECK(base == "DefaultMaterial_3610072657");
    CHECK(MaterialShaderStem("default", "DefaultMaterial", "assets/materials/default.slang", "mesh") ==
          "default_DefaultMaterial_3610072657");
    CHECK(MaterialShaderStem("", "DefaultMaterial", "assets/materials/default.slang", "mesh") == base);
    CHECK(MaterialShaderStem("", "DefaultMaterial", "assets/materials/DEFAULT.slang", "mesh") == base);
    CHECK(MaterialShaderStem("", "A-B", "assets/materials/default.slang", "mesh") !=
          MaterialShaderStem("", "A_B", "assets/materials/default.slang", "mesh"));
    CHECK(MaterialShaderStem("", "DefaultMaterial", "assets/materials/other.slang", "mesh") != base);
    CHECK(MaterialShaderOutput(base, "fragmentMain") == base + ".fragmentMain.spv");
}
