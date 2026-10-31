/**
 * @file SurfaceMaterialCatalog.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/SurfaceMaterialCatalog.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialCatalog.h>

#include <core/fs/Paths.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace mir;

namespace
{
    constexpr const char *kCatalog = R"({
        "version": 1,
        "materials": [
            { "stem": "game_Type_1", "type": "Type", "frontend": "mesh",
              "asset": "games/one/assets/materials/a.material.json",
              "shader": "games/one/assets/materials/a.slang",
              "parameters": { "value": 1.0 },
              "reflection": "unit_surface.reflection.json" },
            { "stem": "game_Type_1", "type": "Type", "frontend": "mesh",
              "asset": "games/one/assets/materials/b.material.json",
              "shader": "games/one/assets/materials/a.slang",
              "parameters": { "value": 2.0 },
              "reflection": "unit_surface.reflection.json" },
            { "stem": "game_Type_2", "type": "Type", "frontend": "terrain",
              "asset": "games/one/assets/materials/terrain.material.json",
              "shader": "games/one/assets/materials/a.slang",
              "parameters": { "value": 3.0 },
              "reflection": "unit_surface.reflection.json" }
        ]
    })";

    constexpr const char *kReflection = R"({
        "entryPoints": [{ "parameters": [{ "type": {
            "baseShape": "structuredBuffer",
            "resultType": { "fields": [{ "name": "value",
                "type": { "kind": "scalar", "scalarType": "float32" },
                "binding": { "offset": 0, "size": 4 } }] } } }] }]
    })";
}

TEST_CASE("Surface catalog rejects malformed versions and entries", "[renderer][material]")
{
    const std::filesystem::path shaders = ShaderPath("");
    std::filesystem::create_directories(shaders);
    {
        std::ofstream catalog(ShaderPath("surface_materials.json"));
        catalog << R"({"version":"1","materials":[]})";
    }
    std::string error;
    CHECK_FALSE(LoadSurfaceMaterialCatalog(&error).has_value());
    CHECK_FALSE(error.empty());

    {
        std::ofstream catalog(ShaderPath("surface_materials.json"));
        catalog << R"({"version":1,"materials":[{"stem":1}]})";
    }
    CHECK_FALSE(LoadSurfaceMaterialCatalog(&error).has_value());

    std::filesystem::remove(ShaderPath("surface_materials.json"));
}

TEST_CASE("Surface catalog keeps per-asset records while sharing a stem", "[renderer][material]")
{
    const std::filesystem::path shaders = ShaderPath("");
    std::filesystem::create_directories(shaders);
    {
        std::ofstream catalog(ShaderPath("surface_materials.json"));
        catalog << kCatalog;
        std::ofstream reflection(ShaderPath("unit_surface.reflection.json"));
        reflection << kReflection;
    }

    std::string error;
    const auto catalog = LoadSurfaceMaterialCatalog(&error);
    INFO(error);
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->materials.size() == 3);
    CHECK(catalog->materials[0].stem == catalog->materials[1].stem);
    CHECK(catalog->materials[0].asset != catalog->materials[1].asset);
    CHECK(catalog->materials[2].frontend == "terrain");
    CHECK(catalog->FindByAsset("games/one/assets/materials/terrain.material.json", "terrain") ==
          &catalog->materials[2]);
    CHECK(catalog->FindByAsset("games/one/assets/materials/terrain.material.json", "mesh") == nullptr);

    const auto *first = catalog->Find("game_Type_1", "games/one/assets/materials/a.material.json");
    const auto *second = catalog->Find("game_Type_1", "games/one/assets/materials/b.material.json");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(catalog->IndexOf(first) == 0);
    CHECK(catalog->IndexOf(second) == 1);
    CHECK(SurfaceMaterialCatalog::HitGroupRecordOffset(0) == 2);
    CHECK(SurfaceMaterialCatalog::HitGroupRecordOffset(1) == 4);

    const auto record = PackSurfaceMaterialRecord(*second, &error);
    INFO(error);
    REQUIRE(record.has_value());
    float value = 0.0f;
    std::memcpy(&value, record->data(), sizeof(value));
    CHECK(value == 2.0f);

    std::filesystem::remove(ShaderPath("surface_materials.json"));
    std::filesystem::remove(ShaderPath("unit_surface.reflection.json"));
}
