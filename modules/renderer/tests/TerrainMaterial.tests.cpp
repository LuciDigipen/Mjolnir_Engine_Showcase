/**
 * @file TerrainMaterial.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/TerrainMaterialAsset and renderer/TerrainLayerBlend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/TerrainLayerBlend.h>
#include <renderer/TerrainMaterialAsset.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace mir;

namespace
{
    constexpr const char *kOwner = "games/G/assets/materials/layered.terrainmaterial.json";
    constexpr const char *kValidLayers =
        R"({"version":1,"blend":"height","heightContrast":0.2,"layers":[)"
        R"({"name":"sand","material":"materials/sand.material.json"},)"
        R"({"name":"dirt","material":"materials/dirt.material.json"}]})";

    SurfaceMaterialRegistration Child(std::string asset, std::string frontend = "terrain")
    {
        SurfaceMaterialRegistration registration;
        registration.stem = asset;
        registration.type = "ChildMaterial";
        registration.frontend = std::move(frontend);
        registration.asset = std::move(asset);
        registration.shader = "games/G/assets/materials/child.slang";
        registration.reflectionFile = "child.layout.reflection.json";
        return registration;
    }

    SurfaceMaterialCatalog ValidCatalog()
    {
        SurfaceMaterialCatalog catalog;
        catalog.materials.push_back(Child("games/G/assets/materials/sand.material.json"));
        catalog.materials.push_back(Child("games/G/assets/materials/dirt.material.json"));
        return catalog;
    }
}

TEST_CASE("Terrain layer blend factors follow weights, heights and alpha coverage",
    "[renderer][terrain][material]")
{
    const std::array<float, kMaxTerrainLayers> heights{ 0.2f, 0.8f, 0.0f, 0.0f };
    const std::array<float, kMaxTerrainLayers> flatterHeights{ 0.2f, 0.21f, 0.0f, 0.0f };
    const std::array<float, kMaxTerrainLayers> single{ 1.0f, 0.0f, 0.0f, 0.0f };
    for (const TerrainLayerBlendMode mode :
        { TerrainLayerBlendMode::Weight, TerrainLayerBlendMode::Height, TerrainLayerBlendMode::Alpha })
    {
        const auto factors = TerrainLayerBlendFactors(single, heights, 2, mode, 0.1f);
        CHECK(factors[0] == Catch::Approx(1.0f));
        CHECK(factors[1] == 0.0f);
    }

    const std::array<float, kMaxTerrainLayers> half{ 0.5f, 0.0f, 0.5f, 0.0f };
    for (const TerrainLayerBlendMode mode :
        { TerrainLayerBlendMode::Weight, TerrainLayerBlendMode::Height, TerrainLayerBlendMode::Alpha })
    {
        const auto factors = TerrainLayerBlendFactors(half, heights, 3, mode, 0.1f);
        CHECK(factors[1] == 0.0f);
        CHECK(factors[0] + factors[1] + factors[2] == Catch::Approx(1.0f));
    }

    const std::array<float, kMaxTerrainLayers> equal{ 0.5f, 0.5f, 0.0f, 0.0f };
    const auto weight = TerrainLayerBlendFactors(equal, heights, 2, TerrainLayerBlendMode::Weight, 0.1f);
    CHECK(weight[0] == Catch::Approx(0.5f));
    CHECK(weight[1] == Catch::Approx(0.5f));

    const auto height = TerrainLayerBlendFactors(equal, heights, 2, TerrainLayerBlendMode::Height, 0.1f);
    CHECK(height[1] > height[0]);
    CHECK(height[0] + height[1] == Catch::Approx(1.0f));

    const auto thin = TerrainLayerBlendFactors(equal, flatterHeights, 2, TerrainLayerBlendMode::Height, 1e-4f);
    CHECK(thin[1] > 0.99f);

    const auto broad = TerrainLayerBlendFactors(equal, heights, 2, TerrainLayerBlendMode::Height, 100.0f);
    CHECK(broad[0] == Catch::Approx(0.5f).margin(0.01f));
    CHECK(broad[1] == Catch::Approx(0.5f).margin(0.01f));

    const std::array<float, kMaxTerrainLayers> top{ 0.0f, 1.0f, 0.0f, 0.0f };
    const auto opaque = TerrainLayerBlendFactors(top, heights, 2, TerrainLayerBlendMode::Alpha, 0.1f);
    CHECK(opaque[1] == Catch::Approx(1.0f));
    CHECK(opaque[0] == 0.0f);

    const std::array<float, kMaxTerrainLayers> below{ 0.4f, 0.6f, 0.0f, 0.0f };
    const auto covered = TerrainLayerBlendFactors(below, heights, 3, TerrainLayerBlendMode::Alpha, 0.1f);
    CHECK(covered[2] == 0.0f);
    CHECK(covered[1] == Catch::Approx(0.6f));
    CHECK(covered[0] == Catch::Approx(0.4f));
}

TEST_CASE("A layered terrain material resolves its layer names and child materials",
    "[renderer][terrain][material]")
{
    const SurfaceMaterialCatalog catalog = ValidCatalog();
    std::string error;
    const auto asset = ReadTerrainMaterial(kValidLayers, kOwner, &catalog, &error);
    INFO(error);
    REQUIRE(asset);
    CHECK(asset->blend == TerrainLayerBlendMode::Height);
    CHECK(asset->heightContrast == Catch::Approx(0.2f));
    REQUIRE(asset->layers.size() == 2);
    CHECK(asset->layers[0].name == "sand");
    CHECK(asset->layers[1].name == "dirt");
    CHECK(asset->layers[0].material == "games/G/assets/materials/sand.material.json");
    CHECK(asset->layers[0].asset == MakeAssetId(asset->layers[0].material));
}

TEST_CASE("A layered terrain material rejects malformed layer lists and children",
    "[renderer][terrain][material]")
{
    const SurfaceMaterialCatalog catalog = ValidCatalog();
    std::string error;

    const auto rejects = [&](std::string_view json, std::string_view expected, const SurfaceMaterialCatalog *override)
    {
        error.clear();
        CHECK_FALSE(ReadTerrainMaterial(json, kOwner, override, &error));
        CHECK(error.find(expected) != std::string::npos);
    };

    rejects(R"({"version":1,"layers":[{"name":"sand","material":"materials/sand.material.json"}]})",
        "two and four", &catalog);
    rejects(R"({"version":1,"layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/dirt.material.json"},)"
            R"({"name":"rock","material":"materials/sand.material.json"},)"
            R"({"name":"moss","material":"materials/sand.material.json"},)"
            R"({"name":"snow","material":"materials/sand.material.json"}]})",
        "two and four", &catalog);
    rejects(R"({"version":1,"layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"sand","material":"materials/dirt.material.json"}]})",
        "duplicate", &catalog);
    rejects(R"({"version":1,"layers":[)"
            R"({"name":"sand layer","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/dirt.material.json"}]})",
        "lowercase", &catalog);
    rejects(R"({"version":1,"layers":[)"
            R"({"name":"darkSand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/dirt.material.json"}]})",
        "lowercase", &catalog);
    rejects(R"({"version":1,"blend":"noise","layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/dirt.material.json"}]})",
        "unknown", &catalog);
    rejects(R"({"version":1,"heightContrast":0,"layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/dirt.material.json"}]})",
        "positive", &catalog);
    rejects(R"({"version":1,"layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/missing.material.json"}]})",
        "not a cooked", &catalog);

    SurfaceMaterialCatalog meshOnly = ValidCatalog();
    meshOnly.materials[1] = Child("games/G/assets/materials/dirt.material.json", "mesh");
    rejects(kValidLayers, "terrain frontend", &meshOnly);

    rejects(R"({"version":1,"layers":[)"
            R"({"name":"sand","material":"materials/sand.material.json"},)"
            R"({"name":"dirt","material":"materials/nested.terrainmaterial.json"}]})",
        "layered", &catalog);
}
