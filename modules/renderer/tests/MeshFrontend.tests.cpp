/**
 * @file MeshFrontend.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/MeshFrontend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshFrontend.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

using namespace mir;

TEST_CASE("Mesh tangents use the model linear transform under non-uniform scale", "[renderer][material]")
{
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));
    const glm::vec4 tangent = TransformMeshTangent(model, glm::vec3(0, 1, 0), glm::vec4(1, 0, 0, 1));
    CHECK(tangent.x == 1.0f);
    CHECK(tangent.y == 0.0f);
    CHECK(tangent.z == 0.0f);
    CHECK(tangent.w == 1.0f);
}

TEST_CASE("Mesh tangents flip handedness for a mirrored model", "[renderer][material]")
{
    const glm::mat4 mirrored = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
    const glm::vec4 tangent = TransformMeshTangent(mirrored, glm::vec3(0, 1, 0), glm::vec4(0, 0, 1, 1));
    CHECK(tangent.w == -1.0f);
}

TEST_CASE("Mesh tangents are orthogonalized against the normal", "[renderer][material]")
{
    const glm::vec4 tangent = TransformMeshTangent(glm::mat4(1.0f), glm::vec3(1, 1, 0), glm::vec4(1, 0, 0, 1));
    CHECK(tangent.x > 0.0f);
    CHECK(tangent.y < 0.0f);
    CHECK(tangent.z == 0.0f);
    CHECK(tangent.w == 1.0f);
}
