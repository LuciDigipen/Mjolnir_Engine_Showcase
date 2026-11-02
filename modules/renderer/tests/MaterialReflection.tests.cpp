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

#include <nlohmann/json.hpp>

#include <cstring>
#include <span>
#include <utility>
#include <vector>

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

TEST_CASE("A same-size field reorder changes the material schema revision", "[renderer][material][reflection]")
{
    mir::MaterialLayout first;
    first.fields = {
        {.name = "x", .kind = mir::MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "y", .kind = mir::MaterialFieldKind::Float, .offset = 4, .size = 4},
    };
    first.size = 16;

    mir::MaterialLayout reordered = first;
    std::swap(reordered.fields[0].offset, reordered.fields[1].offset);

    CHECK(mir::MaterialLayoutRevision(first) == mir::MaterialLayoutRevision(first));
    CHECK(mir::MaterialLayoutRevision(first) != mir::MaterialLayoutRevision(reordered));
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

    const char *wideOffset = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"a","type":{"kind":"scalar","scalarType":"float32"},
        "binding":{"offset":4294967296,"size":4}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(wideOffset, &error).has_value());

    const char *oversized = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"a","type":{"kind":"scalar","scalarType":"float32"},
        "binding":{"offset":0,"size":4294967295}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(oversized, &error).has_value());

    const char *misalignedVector = R"({"entryPoints":[{"parameters":[{"type":{"baseShape":"structuredBuffer",
        "resultType":{"fields":[{"name":"a","type":{"kind":"vector","elementCount":2,
        "elementType":{"kind":"scalar","scalarType":"float32"}},"binding":{"offset":4,"size":8}}]}}}]}]})";
    CHECK_FALSE(ParseMaterialLayout(misalignedVector, &error).has_value());
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
    CHECK_FALSE(PackMaterialRecord(*layout,
        R"({"flag":true,"index":1,"count":18446744073709551615,"value":1})")
                    .has_value());
    CHECK(PackMaterialRecord(*layout,
        R"({"flag":true,"index":4294967295,"count":-2147483648,"value":-3.5})")
              .has_value());
    CHECK(PackMaterialRecord(*layout,
        R"({"flag":false,"index":0,"count":2147483647,"value":3.5})")
              .has_value());
}

TEST_CASE("A shader-only schema change completes missing values and drops deleted keys",
    "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());

    std::string error;
    const auto completed = CompleteMaterialParameters(*layout,
        R"({"amount": 1.5, "retired": [1.0, 2.0]})", R"({"color": [2.0, 3.0, 4.0], "texture": {"image": 9}})",
        &error);
    INFO(error);
    REQUIRE(completed.has_value());

    const nlohmann::json parameters = nlohmann::json::parse(*completed);
    CHECK(parameters.size() == 3);
    CHECK(parameters.at("amount") == 1.5f);
    CHECK(parameters.at("color") == nlohmann::json::array({ 2.0f, 3.0f, 4.0f }));
    CHECK(parameters.at("texture").at("image") == 9);
    CHECK_FALSE(parameters.contains("retired"));

    const auto bytes = PackMaterialRecord(*layout, *completed, &error);
    INFO(error);
    REQUIRE(bytes.has_value());
    uint32_t image = 0;
    std::memcpy(&image, bytes->data() + 28, sizeof(image));
    CHECK(image == 9);
}

TEST_CASE("Completion falls back to the typed default when nothing authored a field",
    "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());

    const auto completed = CompleteMaterialParameters(*layout, "{}", "{}");
    REQUIRE(completed.has_value());
    const nlohmann::json parameters = nlohmann::json::parse(*completed);
    CHECK(parameters.at("amount") == 0.0f);
    CHECK(parameters.at("color") == nlohmann::json::array({ 0.0f, 0.0f, 0.0f }));
    CHECK(parameters.at("texture").at("image") == 0);
    CHECK(PackMaterialRecord(*layout, *completed).has_value());

    CHECK_FALSE(CompleteMaterialParameters(*layout, "[]", "{}").has_value());
    CHECK_FALSE(CompleteMaterialParameters(*layout, "{}", "[]").has_value());
}

TEST_CASE("A packed record decodes back to parameters by field name", "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());
    const auto bytes = PackMaterialRecord(*layout,
        R"({"amount": 1.5, "color": [2.0, 3.0, 4.0], "texture": {"image": 7}})", nullptr);
    REQUIRE(bytes.has_value());

    std::string error;
    const auto parameters = MaterialRecordParameters(*layout, *bytes, &error);
    INFO(error);
    REQUIRE(parameters.has_value());
    CHECK(nlohmann::json::parse(*parameters) ==
          nlohmann::json::parse(R"({"amount": 1.5, "color": [2.0, 3.0, 4.0], "texture": {"image": 7}})"));

    CHECK_FALSE(MaterialRecordParameters(*layout, std::span<const std::byte>(*bytes).first(4)).has_value());
}

TEST_CASE("Record decoding rejects storage that disagrees with the field kind", "[renderer][material][reflection]")
{
    std::vector<std::byte> record(16);
    std::string error;

    MaterialLayout narrow;
    narrow.fields.push_back({.name = "tail", .kind = MaterialFieldKind::Float4, .offset = 12, .size = 4});
    narrow.size = 16;
    CHECK_FALSE(MaterialRecordParameters(narrow, record, &error).has_value());
    CHECK(error.find("does not fit the record") != std::string::npos);

    MaterialLayout wideTexture;
    wideTexture.fields.push_back({.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 0, .size = 12});
    wideTexture.size = 16;
    CHECK_FALSE(MaterialRecordParameters(wideTexture, record, &error).has_value());

    MaterialLayout emptyLayout;
    emptyLayout.size = 16;
    CHECK_FALSE(MaterialRecordParameters(emptyLayout, std::span<const std::byte>{}, &error).has_value());

    MaterialLayout cover;
    cover.fields.push_back({.name = "amount", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4});
    cover.size = 16;
    CHECK_FALSE(MaterialRecordParameters(cover, std::span<const std::byte>(record).first(8), &error).has_value());
    CHECK(MaterialRecordParameters(cover, record, &error).has_value());
}

TEST_CASE("A peer record donates numeric values but not its texture rows", "[renderer][material][reflection]")
{
    MaterialLayout mesh;
    mesh.fields = {
        {.name = "amount", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 8, .size = 8},
        {.name = "color", .kind = MaterialFieldKind::Float3, .offset = 16, .size = 12},
    };
    mesh.size = 32;
    const char *ownJson = R"({"amount": 0, "albedo": {"image": 2, "sampler": 1}, "color": [0, 0, 0]})";
    const auto own = PackMaterialRecord(mesh, ownJson);
    REQUIRE(own);

    MaterialLayout terrain;
    terrain.fields = {
        {.name = "color", .kind = MaterialFieldKind::Float3, .offset = 0, .size = 12},
        {.name = "amount", .kind = MaterialFieldKind::Float, .offset = 16, .size = 4},
        {.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 20, .size = 4},
        {.name = "tileSizeMeters", .kind = MaterialFieldKind::Float, .offset = 24, .size = 4},
    };
    terrain.size = 32;
    const auto peer = PackMaterialRecord(terrain,
        R"({"color": [1, 0.5, 0.25], "amount": 3, "albedo": {"image": 9}, "tileSizeMeters": 2})");
    REQUIRE(peer);

    std::string error;
    const auto merged = MergePeerNumericParameters(mesh, ownJson, terrain, *peer, &error);
    INFO(error);
    REQUIRE(merged);
    const nlohmann::json values = nlohmann::json::parse(*merged);
    CHECK(values.at("amount").get<double>() == 3.0);
    CHECK(values.at("color") == nlohmann::json::array({ 1.0f, 0.5f, 0.25f }));
    CHECK(values.at("albedo").at("image") == 2);
    CHECK(values.at("albedo").at("sampler") == 1);
    CHECK_FALSE(values.contains("tileSizeMeters"));

    MaterialLayout retyped = terrain;
    retyped.fields[1].kind = MaterialFieldKind::Uint;
    const auto retypedPeer = PackMaterialRecord(retyped,
        R"({"color": [1, 0.5, 0.25], "amount": 3, "albedo": {"image": 9}, "tileSizeMeters": 2})");
    REQUIRE(retypedPeer);
    const auto kept = MergePeerNumericParameters(mesh, ownJson, retyped, *retypedPeer, &error);
    REQUIRE(kept);
    const nlohmann::json keptValues = nlohmann::json::parse(*kept);
    CHECK(keptValues.at("amount").get<double>() == 0.0);
    CHECK(keptValues.at("color") == nlohmann::json::array({ 1.0f, 0.5f, 0.25f }));
    CHECK_FALSE(MergePeerNumericParameters(mesh, ownJson, terrain,
                    std::span<const std::byte>(*peer).first(8), &error)
                    .has_value());
}
TEST_CASE("A shared overlay keeps only the fields one frontend reflects", "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());

    std::optional<std::string> filtered = FilterMaterialParameterOverlay(*layout,
        R"({"amount": 2.5, "tileSizeMeters": 4, "color": [1, 2, 3]})");
    REQUIRE(filtered);
    nlohmann::json values = nlohmann::json::parse(*filtered);
    CHECK(values.size() == 2);
    CHECK(values.at("amount") == 2.5f);
    CHECK_FALSE(values.contains("tileSizeMeters"));

    const auto merged = OverlayMaterialParameters(*layout, R"({"amount": 1, "color": [0, 0, 0], "texture": {"image": 4}})",
        *filtered);
    REQUIRE(merged);
    CHECK(nlohmann::json::parse(*merged).at("amount") == 2.5f);
    CHECK(FilterMaterialParameterOverlay(*layout, "{}").value() == "{}");
    CHECK_FALSE(FilterMaterialParameterOverlay(*layout, "[]").has_value());
}

TEST_CASE("Parameter overlays keep resolved texture rows", "[renderer][material][reflection]")
{
    const auto layout = ParseMaterialLayout(kReflection);
    REQUIRE(layout.has_value());
    std::string error;
    const auto merged = OverlayMaterialParameters(*layout,
        R"({"amount": 1.5, "color": [2.0, 3.0, 4.0], "texture": {"image": 7}})",
        R"({"amount": 9.0, "color": [5.0, 6.0, 7.0]})", &error);
    INFO(error);
    REQUIRE(merged.has_value());

    const auto bytes = PackMaterialRecord(*layout, *merged, &error);
    INFO(error);
    REQUIRE(bytes.has_value());
    float amount = 0.0f;
    std::memcpy(&amount, bytes->data(), sizeof(amount));
    CHECK(amount == 9.0f);
    float color[3]{};
    std::memcpy(color, bytes->data() + 16, sizeof(color));
    CHECK(color[2] == 7.0f);
    uint32_t image = 0;
    std::memcpy(&image, bytes->data() + 28, sizeof(image));
    CHECK(image == 7);

    CHECK_FALSE(OverlayMaterialParameters(*layout,
        R"({"amount": 1.0, "color": [1.0, 1.0, 1.0], "texture": {"image": 1}})",
        R"({"unknown": 2.0})", &error)
                    .has_value());
    CHECK_FALSE(OverlayMaterialParameters(*layout,
        R"({"amount": 1.0, "color": [1.0, 1.0, 1.0], "texture": {"image": 1}})",
        R"({"texture": {"image": 3}})", &error)
                    .has_value());
    CHECK_FALSE(OverlayMaterialParameters(*layout,
        R"({"amount": 1.0, "color": [1.0, 1.0, 1.0], "texture": {"image": 1}})",
        R"({"amount": 1e300})", &error)
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
