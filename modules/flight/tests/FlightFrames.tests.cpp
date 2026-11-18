/**
 * @file FlightFrames.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightFrames.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightFrames.h>

#include <physics/PhysicsTypes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace
{
    constexpr float kTolerance = 1e-5f;

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(kTolerance));
        CHECK(actual.y == Catch::Approx(expected.y).margin(kTolerance));
        CHECK(actual.z == Catch::Approx(expected.z).margin(kTolerance));
    }

    glm::vec3 WorldToBody(const glm::vec3 &worldVector, const glm::quat &rotation)
    {
        return mir::LocalToBody(glm::inverse(rotation) * worldVector);
    }
}

TEST_CASE("The body axes map to the engine local axes", "[flight][frames]")
{
    const glm::mat3 basis = mir::BodyToLocalMatrix();

    CheckVector(basis * glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, -1.0f});
    CheckVector(basis * glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{1.0f, 0.0f, 0.0f});
    CheckVector(basis * glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, -1.0f, 0.0f});
    CHECK(glm::determinant(basis) == Catch::Approx(1.0f).margin(kTolerance));
}

TEST_CASE("Body and local conversions round trip", "[flight][frames]")
{
    const glm::vec3 vectors[] = {
        glm::vec3{1.0f, 2.0f, 3.0f},
        glm::vec3{-0.5f, 4.0f, -2.0f},
        glm::vec3{0.0f, 0.0f, 0.0f},
    };

    for (const glm::vec3 &vector : vectors)
        CheckVector(mir::LocalToBody(mir::BodyToLocal(vector)), vector);
}

TEST_CASE("Gravity reads as body-frame acceleration", "[flight][frames]")
{
    const glm::vec3 gravity{0.0f, -9.81f, 0.0f};

    CheckVector(WorldToBody(gravity, mir::kIdentityRotation), glm::vec3{0.0f, 0.0f, 9.81f});

    const glm::quat noseUp = glm::angleAxis(glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
    CheckVector(WorldToBody(gravity, noseUp), glm::vec3{-9.81f, 0.0f, 0.0f});
}

TEST_CASE("Air angles come from body velocity", "[flight][frames]")
{
    REQUIRE(mir::AngleOfAttack(glm::vec3{50.0f, 0.0f, 5.0f}) > 0.0f);
    REQUIRE(mir::Sideslip(glm::vec3{50.0f, 5.0f, 0.0f}) > 0.0f);

    REQUIRE(mir::AngleOfAttack(glm::vec3{0.0f}) == 0.0f);
    REQUIRE(mir::Sideslip(glm::vec3{0.0f}) == 0.0f);

    REQUIRE(std::isfinite(mir::AngleOfAttack(glm::vec3{-50.0f, 0.0f, 0.0f})));
    REQUIRE(std::isfinite(mir::Sideslip(glm::vec3{-50.0f, 0.0f, 0.0f})));
}
