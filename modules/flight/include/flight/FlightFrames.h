/**
 * @file FlightFrames.h
 * @author Jackey Cen
 * @brief Converts between the aircraft body frame and the engine's local axes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    // Body axes: X nose, Y right wing, Z belly. Local axes: X right, Y up, Z back.
    glm::mat3 BodyToLocalMatrix();

    glm::vec3 BodyToLocal(const glm::vec3 &vector);
    glm::vec3 LocalToBody(const glm::vec3 &vector);

    // R I R^T, with R the body-to-local matrix.
    glm::mat3 InertiaBodyToLocal(const glm::mat3 &inertia);

    float AngleOfAttack(const glm::vec3 &bodyVelocity);
    float Sideslip(const glm::vec3 &bodyVelocity);
}
