/**
 * @file MaterialResolution.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/MaterialResolution.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialResolution.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    SurfaceMaterialCatalog MakeCatalog()
    {
        SurfaceMaterialCatalog catalog;
        catalog.materials.push_back(SurfaceMaterialRegistration{
            .stem = "default_DefaultMaterial_1",
            .type = "DefaultMaterial",
            .frontend = "mesh",
            .asset = "assets/materials/default.material.json",
            .shader = "assets/materials/default.slang",
            .parametersJson = "{}",
            .reflectionFile = "default.layout.reflection.json" });
        catalog.materials.push_back(SurfaceMaterialRegistration{
            .stem = "game_Custom_2",
            .type = "Custom",
            .frontend = "mesh",
            .asset = "games/g/assets/materials/custom.material.json",
            .shader = "games/g/assets/materials/custom.slang",
            .parametersJson = "{}",
            .reflectionFile = "custom.layout.reflection.json" });
        return catalog;
    }
}

TEST_CASE("Explicit default requests normalize to the reflected default", "[renderer][material]")
{
    const SurfaceMaterialCatalog catalog = MakeCatalog();
    MaterialDesc requested;
    requested.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    requested.cullMode = VK_CULL_MODE_NONE;
    requested.alphaMode = MaterialAlphaMode::Mask;
    requested.alphaCutoff = 0.25f;
    requested.sampler.maxAnisotropy = 4.0f;

    std::string error;
    const auto resolved = ResolveMaterialDesc(requested, catalog, true, &error);
    INFO(error);
    REQUIRE(resolved.has_value());
    CHECK(resolved->shaderName == "default_DefaultMaterial_1");
    CHECK(resolved->surfaceAsset == "assets/materials/default.material.json");
    CHECK(resolved->surfaceMaterial);
    CHECK(resolved->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    CHECK(resolved->cullMode == VK_CULL_MODE_NONE);
    CHECK(resolved->alphaMode == MaterialAlphaMode::Mask);
    CHECK(resolved->alphaCutoff == 0.25f);
    CHECK(resolved->sampler.maxAnisotropy == 4.0f);
}

TEST_CASE("A device without shared tables keeps the legacy default", "[renderer][material]")
{
    const SurfaceMaterialCatalog catalog = MakeCatalog();
    const auto resolved = ResolveMaterialDesc(MaterialDesc{}, catalog, false, nullptr);
    REQUIRE(resolved.has_value());
    CHECK(resolved->shaderName == "default");
    CHECK_FALSE(resolved->surfaceMaterial);
    CHECK(resolved->surfaceAsset.empty());
}

TEST_CASE("Custom surface stems resolve to their own registration", "[renderer][material]")
{
    const SurfaceMaterialCatalog catalog = MakeCatalog();
    MaterialDesc requested;
    requested.shaderName = "game_Custom_2";
    requested.surfaceAsset = "games/g/assets/materials/custom.material.json";
    const auto resolved = ResolveMaterialDesc(requested, catalog, true, nullptr);
    REQUIRE(resolved.has_value());
    CHECK(resolved->surfaceMaterial);
    CHECK(resolved->shaderName == "game_Custom_2");
}

TEST_CASE("Unknown surface requests reject instead of becoming default", "[renderer][material]")
{
    const SurfaceMaterialCatalog catalog = MakeCatalog();
    MaterialDesc requested;
    requested.surfaceMaterial = true;
    requested.shaderName = "missing_Type";
    std::string error;
    CHECK_FALSE(ResolveMaterialDesc(requested, catalog, true, &error).has_value());
    CHECK_FALSE(error.empty());

    CHECK_FALSE(ResolveMaterialDesc(requested, catalog, false, nullptr).has_value());
}
