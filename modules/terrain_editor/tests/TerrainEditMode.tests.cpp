/**
 * @file TerrainEditMode.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainEditMode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_editor/TerrainEditMode.h>

#include <core/ecs/GroundQuery.h>
#include <core/math/WorldCoordinates.h>
#include <renderer/CameraMath.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace
{
    using mir::DocumentId;
    using mir::FindTerrainCursor;
    using mir::GroundQueryResult;
    using mir::GroundQueryStatus;
    using mir::kSceneDocument;
    using mir::TerrainBrush;
    using mir::TerrainEditMode;
    using mir::WorldPosition;
    using mir::WorldRay;

    constexpr double kTolerance = 1e-3;

    WorldPosition At(double x, double y, double z)
    {
        return *WorldPosition::FromDoubleMeters({ x, y, z });
    }

    GroundQueryResult SurfaceAt(double height)
    {
        GroundQueryResult result;
        result.status = GroundQueryStatus::Ready;
        result.surface = WorldPosition::FromDoubleMeters({ 0.0, height, 0.0 });
        return result;
    }
}

TEST_CASE("Terrain edit mode turns on and off in Edit on the scene document", "[editor][terrain]")
{
    TerrainEditMode mode;
    CHECK_FALSE(mode.Active());

    CHECK(mode.Toggle(true, kSceneDocument));
    CHECK(mode.Active());

    CHECK_FALSE(mode.Toggle(true, kSceneDocument));
    CHECK_FALSE(mode.Active());
}

TEST_CASE("Terrain edit mode is refused outside Edit and on a Prefab document", "[editor][terrain]")
{
    TerrainEditMode mode;

    CHECK_FALSE(mode.Toggle(false, kSceneDocument));
    CHECK_FALSE(mode.Active());

    CHECK_FALSE(mode.Toggle(true, DocumentId{ 1, 0 }));
    CHECK_FALSE(mode.Active());
}

TEST_CASE("Terrain edit mode turns off on Play", "[editor][terrain]")
{
    TerrainEditMode mode;
    REQUIRE(mode.Toggle(true, kSceneDocument));

    mode.Update(true, kSceneDocument);
    CHECK(mode.Active());

    mode.Update(false, kSceneDocument);
    CHECK_FALSE(mode.Active());
}

TEST_CASE("Terrain edit mode turns off when a Prefab document becomes active", "[editor][terrain]")
{
    TerrainEditMode mode;
    REQUIRE(mode.Toggle(true, kSceneDocument));

    mode.Update(true, DocumentId{ 2, 3 });
    CHECK_FALSE(mode.Active());
}

TEST_CASE("Terrain brush settings stay inside their ranges", "[editor][terrain]")
{
    TerrainEditMode mode;
    CHECK(mode.Brush() == TerrainBrush::RaiseLower);

    mode.SetRadius(1000.0f);
    CHECK(mode.Radius() == TerrainEditMode::kMaxRadius);
    mode.SetRadius(0.0f);
    CHECK(mode.Radius() == TerrainEditMode::kMinRadius);

    mode.SetStrength(2.0f);
    CHECK(mode.Strength() == 2.0f);
    mode.SetStrength(TerrainEditMode::kMaxStrength * 2.0f);
    CHECK(mode.Strength() == TerrainEditMode::kMaxStrength);
    mode.SetStrength(-1.0f);
    CHECK(mode.Strength() == 0.0f);

    mode.SetFalloff(2.0f);
    CHECK(mode.Falloff() == 1.0f);
    mode.SetFalloff(-1.0f);
    CHECK(mode.Falloff() == 0.0f);

    mode.SetBrush(TerrainBrush::Paint);
    CHECK(mode.Brush() == TerrainBrush::Paint);
}

TEST_CASE("The cursor lands where the ray meets a flat ground", "[editor][terrain]")
{
    const auto flat = [](WorldPosition point)
    {
        return *WorldPosition::FromDoubleMeters({ point.Meters().x, 10.0, point.Meters().z });
    };
    const auto ground = [&flat](WorldPosition point)
    {
        GroundQueryResult result;
        result.status = GroundQueryStatus::Ready;
        result.surface = flat(point);
        return result;
    };

    WorldRay ray{ At(0.0, 50.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f) };
    const std::optional<WorldPosition> hit = FindTerrainCursor(ray, 100.0, ground);

    REQUIRE(hit.has_value());
    CHECK(hit->Meters().x == Catch::Approx(0.0).margin(kTolerance));
    CHECK(hit->Meters().z == Catch::Approx(0.0).margin(kTolerance));
    CHECK(hit->Meters().y == Catch::Approx(10.0).margin(kTolerance));
}

TEST_CASE("The cursor lands on a sloped ground", "[editor][terrain]")
{
    const auto ground = [](WorldPosition point)
    {
        const glm::dvec3 meters(point.Meters());
        GroundQueryResult result;
        result.status = GroundQueryStatus::Ready;
        result.surface = WorldPosition::FromDoubleMeters({ meters.x, 5.0 + 0.25 * meters.x, meters.z });
        return result;
    };

    const WorldRay ray{ At(0.0, 50.0, 0.0), glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f)) };
    const std::optional<WorldPosition> hit = FindTerrainCursor(ray, 200.0, ground);

    REQUIRE(hit.has_value());
    const glm::dvec3 meters(hit->Meters());
    CHECK(meters.x == Catch::Approx(36.0).margin(kTolerance));
    CHECK(meters.y == Catch::Approx(5.0 + 0.25 * meters.x).margin(kTolerance));
}

TEST_CASE("The cursor is absent when the ray points away from the ground", "[editor][terrain]")
{
    const auto ground = [](WorldPosition)
    {
        return SurfaceAt(10.0);
    };

    const WorldRay ray{ At(0.0, 50.0, 0.0), glm::vec3(0.0f, 1.0f, 0.0f) };
    CHECK_FALSE(FindTerrainCursor(ray, 100.0, ground).has_value());
}

TEST_CASE("The cursor is absent while the ground only reports Loading", "[editor][terrain]")
{
    const auto loading = [](WorldPosition)
    {
        GroundQueryResult result;
        result.status = GroundQueryStatus::Loading;
        result.diagnostic = "Waiting for the source.";
        return result;
    };

    const WorldRay ray{ At(0.0, 50.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f) };
    CHECK_FALSE(FindTerrainCursor(ray, 100.0, loading).has_value());
}

TEST_CASE("The cursor is absent when the ground lies beyond the maximum distance", "[editor][terrain]")
{
    const auto ground = [](WorldPosition)
    {
        return SurfaceAt(0.0);
    };

    const WorldRay ray{ At(0.0, 50.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f) };
    CHECK_FALSE(FindTerrainCursor(ray, 20.0, ground).has_value());
}
