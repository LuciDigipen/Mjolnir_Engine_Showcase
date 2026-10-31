/**
 * @file SurfaceMaterialLibrary.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/SurfaceMaterialLibrary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialTexture.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

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