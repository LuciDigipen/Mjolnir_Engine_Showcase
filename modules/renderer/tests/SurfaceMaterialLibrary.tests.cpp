/**
 * @file SurfaceMaterialLibrary.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/SurfaceMaterialLibrary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialTexture.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <vector>

using namespace mir;

TEST_CASE("Surface texture fields use the documented names and aliases", "[renderer][material]")
{
    for (const char *name : {"albedo", "albedoTexture", "baseColor", "baseColorTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Albedo);
    for (const char *name : {"normal", "normalTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Normal);
    for (const char *name : {"surface", "surfaceTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Surface);
}

TEST_CASE("Surface texture field mapping rejects ambiguous names", "[renderer][material]")
{
    for (const char *name : {"baseColorNormal", "normalRoughness", "roughness", "mask", "albedoSurface", ""})
        CHECK_FALSE(SurfaceTextureRoleForField(name).has_value());
    for (const char *name : {"macro", "macroTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Macro);
}

TEST_CASE("Macro and parallax parameters come from the packed record", "[renderer][material]")
{
    MaterialLayout layout;
    const auto add = [&layout](const char *name, MaterialFieldKind kind, uint32_t offset, uint32_t size)
    {
        layout.fields.push_back(MaterialFieldLayout{.name = name, .kind = kind, .offset = offset, .size = size});
    };
    add("macroPeriodMeters", MaterialFieldKind::Float, 0, 4);
    add("macroStrength", MaterialFieldKind::Float, 4, 4);
    add("macroHueShiftDegrees", MaterialFieldKind::Float, 8, 4);
    add("macroRoughnessContrast", MaterialFieldKind::Float, 12, 4);
    add("parallaxEnabled", MaterialFieldKind::Bool, 16, 4);
    add("parallaxRtEnabled", MaterialFieldKind::Bool, 20, 4);
    add("parallaxHeightMeters", MaterialFieldKind::Float, 24, 4);
    add("parallaxReference", MaterialFieldKind::Float, 28, 4);
    add("parallaxDistanceMeters", MaterialFieldKind::Float, 32, 4);
    add("parallaxSteps", MaterialFieldKind::Uint, 36, 4);
    add("parallaxFadeStartMeters", MaterialFieldKind::Float, 40, 4);
    add("parallaxFadeEndMeters", MaterialFieldKind::Float, 44, 4);
    layout.size = 48;

    std::array<std::byte, 48> record{};
    const auto store = [&record](uint32_t offset, auto value)
    {
        std::memcpy(record.data() + offset, &value, sizeof(value));
    };
    store(0, 64.0f);
    store(4, 0.35f);
    store(8, 8.0f);
    store(12, 0.35f);
    store(16, 1u);
    store(20, 0u);
    store(24, 0.057f);
    store(28, 0.455f);
    store(32, 0.08f);
    store(36, 24u);
    store(40, 40.0f);
    store(44, 120.0f);

    const std::optional<SurfaceMaterialMacroParameters> macro =
        SurfaceMaterialMacroParametersFromRecord(layout, record);
    REQUIRE(macro);
    REQUIRE(macro->periodMeters);
    CHECK(*macro->periodMeters == 64.0);
    CHECK(macro->strength == Catch::Approx(0.35));
    CHECK(macro->hueShiftDegrees == Catch::Approx(8.0));
    CHECK(ValidateSurfaceMaterialMacroParameters(*macro, nullptr));

    const std::optional<SurfaceMaterialParallaxParameters> parallax =
        SurfaceMaterialParallaxParametersFromRecord(layout, record);
    REQUIRE(parallax);
    CHECK(parallax->enabled);
    CHECK_FALSE(parallax->rayTracedEnabled);
    CHECK(parallax->heightMeters == Catch::Approx(0.057));
    CHECK(parallax->reference == Catch::Approx(0.455));
    CHECK(parallax->steps == 24);
    CHECK(ValidateSurfaceMaterialParallaxParameters(*parallax, nullptr));
}

TEST_CASE("Material numeric bounds reject unsafe macro and parallax values", "[renderer][material]")
{
    SurfaceMaterialMacroParameters macro;
    macro.periodMeters = 0.0;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));
    macro.periodMeters = 64.0;
    macro.strength = 1.5;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));
    macro.strength = 0.35;
    macro.hueShiftDegrees = 181.0;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));

    SurfaceMaterialParallaxParameters parallax;
    parallax.heightMeters = 0.057;
    parallax.reference = 0.455;
    parallax.maxDistanceMeters = 0.08;
    parallax.steps = 24;
    parallax.fadeEndMeters = 120.0;
    CHECK(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.heightMeters = 0.6;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.heightMeters = 0.057;
    parallax.steps = 129;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.steps = 24;
    parallax.fadeStartMeters = 130.0;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
}

TEST_CASE("The macro texture is deterministic and non-uniform", "[renderer][material]")
{
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    BuildSurfaceMacroTexture(256, first);
    BuildSurfaceMacroTexture(256, second);
    REQUIRE(first.size() == 256u * 256u * 4u);
    CHECK(first == second);

    const std::byte third = std::byte{255};
    CHECK(first[3] == third);
    bool distinct = false;
    for (size_t index = 0; index < first.size() && !distinct; index += 4)
        distinct = first[index] != first[0];
    CHECK(distinct);
}

TEST_CASE("Terrain tile periods come from the packed float record", "[renderer][material]")
{
    MaterialLayout layout;
    layout.fields.push_back(MaterialFieldLayout{.name = "tileSizeMeters",
                                                .kind = MaterialFieldKind::Float,
                                                .offset = 0,
                                                .size = 4});
    layout.size = 16;
    std::array<std::byte, 16> record{};

    const float two = 2.0f;
    std::memcpy(record.data(), &two, sizeof(two));
    const std::optional<double> period = SurfaceMaterialTilePeriodFromRecord(layout, record);
    REQUIRE(period);
    CHECK(*period == 2.0);

    MaterialLayout absent;
    absent.size = 16;
    CHECK_FALSE(SurfaceMaterialTilePeriodFromRecord(absent, record).has_value());
}