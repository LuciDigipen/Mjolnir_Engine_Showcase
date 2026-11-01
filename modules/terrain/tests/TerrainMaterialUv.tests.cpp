/**
 * @file TerrainMaterialUv.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainMaterialUv.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainMaterialUv.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace mir;

namespace
{
    float Fraction(float meters, float tileSize)
    {
        return meters / tileSize;
    }
}

TEST_CASE("Terrain material UVs reduce the origin once then stay affine", "[terrain][uv]")
{
    const glm::dvec3 origin{kTerrainMaterialUvPeriodMeters - 0.5, 0.0, -0.5};
    const glm::vec2 first = TerrainMaterialUv(origin, glm::vec3{0.0f, 0.0f, 0.0f});
    const glm::vec2 second = TerrainMaterialUv(origin, glm::vec3{1.0f, 0.0f, 1.0f});
    CHECK(first.x == Catch::Approx(float(kTerrainMaterialUvPeriodMeters - 0.5)));
    CHECK(second.x == Catch::Approx(float(kTerrainMaterialUvPeriodMeters + 0.5)));
    CHECK(first.y == Catch::Approx(float(kTerrainMaterialUvPeriodMeters - 0.5)));
    CHECK(second.y == Catch::Approx(float(kTerrainMaterialUvPeriodMeters + 0.5)));
    CHECK(second.x - first.x == Catch::Approx(1.0f));
    CHECK(second.y - first.y == Catch::Approx(1.0f));
}

TEST_CASE("Terrain material UVs interpolate across the period without a per-vertex wrap", "[terrain][uv]")
{
    const glm::dvec3 origin{kTerrainMaterialUvPeriodMeters - 0.5, 0.0, 0.0};
    const glm::vec3 vertices[]{{0.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const glm::vec2 a = TerrainMaterialUv(origin, vertices[0]);
    const glm::vec2 b = TerrainMaterialUv(origin, vertices[1]);
    const glm::vec2 c = TerrainMaterialUv(origin, vertices[2]);

    const float tile = 2.0f;
    const float expectedStep = 0.5f / tile;
    CHECK(Fraction(b.x, tile) - Fraction(a.x, tile) == Catch::Approx(expectedStep));
    CHECK(Fraction(c.x, tile) - Fraction(b.x, tile) == Catch::Approx(expectedStep));
    CHECK(Fraction(c.x, tile) - Fraction(a.x, tile) == Catch::Approx(2.0f * expectedStep));
}

TEST_CASE("Terrain material UVs stay affine for negative origins, Regions and meshes wider than the period", "[terrain][uv]")
{
    const glm::dvec3 negative{-1.0, 0.0, -kTerrainMaterialUvPeriodMeters - 1.0};
    const glm::vec2 n0 = TerrainMaterialUv(negative, glm::vec3{0.0f, 0.0f, 0.0f});
    const glm::vec2 n1 = TerrainMaterialUv(negative, glm::vec3{1.0f, 0.0f, 1.0f});
    CHECK(n1.x - n0.x == Catch::Approx(1.0f));
    CHECK(n1.y - n0.y == Catch::Approx(1.0f));

    const glm::dvec3 wide{0.0, 0.0, 0.0};
    const glm::vec2 w0 = TerrainMaterialUv(wide, glm::vec3{0.0f, 0.0f, 0.0f});
    const glm::vec2 w1 = TerrainMaterialUv(wide, glm::vec3{static_cast<float>(kTerrainMaterialUvPeriodMeters) + 16.0f,
        0.0f, 0.0f});
    CHECK(w1.x == Catch::Approx(float(kTerrainMaterialUvPeriodMeters + 16.0)));
    CHECK(w1.x - w0.x == Catch::Approx(float(kTerrainMaterialUvPeriodMeters + 16.0)));

    const glm::dvec3 tileOrigin{64.0, 0.0, 0.0};
    const glm::dvec3 regionOrigin{0.0, 0.0, 0.0};
    CHECK(TerrainMaterialUv(tileOrigin, glm::vec3{0.0f, 0.0f, 0.0f}).x ==
          Catch::Approx(TerrainMaterialUv(regionOrigin, glm::vec3{64.0f, 0.0f, 0.0f}).x));
}

TEST_CASE("Terrain material UVs keep the vertex slope at a far origin", "[terrain][uv]")
{
    const glm::dvec3 origin{150000.5, 0.0, -75000.25};
    const glm::vec2 a = TerrainMaterialUv(origin, glm::vec3{0.0f, 0.0f, 0.0f});
    const glm::vec2 b = TerrainMaterialUv(origin, glm::vec3{1.0f, 0.0f, 0.0f});
    CHECK(b.x - a.x == Catch::Approx(1.0f));
    CHECK(std::isfinite(a.y));
}

TEST_CASE("Terrain material tile periods divide the shared period", "[terrain][uv]")
{
    for (const double period : {0.5, 1.0, 2.0, 2.5, 8.0, 16.0, 1280.0, 3840.0, 0.001953125})
    {
        INFO(period);
        double remainder = -1.0;
        CHECK(TerrainMaterialTilePeriodSupported(period, &remainder));
        CHECK(remainder >= 0.0);
        CHECK(remainder < 1e-6);
    }

    for (const double period : {0.0, -8.0, 7.0, 7680.0, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()})
    {
        INFO(period);
        CHECK_FALSE(TerrainMaterialTilePeriodSupported(period));
    }
}
