/**
 * @file FlightFrames.cpp
 * @author Jackey Cen
 * @brief Converts between the aircraft body frame and the engine's local axes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightFrames.h>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <cmath>

namespace mir
{
    glm::mat3 BodyToLocalMatrix()
    {
        return glm::mat3{
            glm::vec3{ 0.0f, 0.0f, -1.0f }, // body X, nose -> local -Z
            glm::vec3{ 1.0f, 0.0f, 0.0f },  // body Y, right wing -> local +X
            glm::vec3{ 0.0f, -1.0f, 0.0f }, // body Z, belly -> local -Y
        };
    }

    glm::vec3 BodyToLocal(const glm::vec3 &vector)
    {
        return BodyToLocalMatrix() * vector;
    }

    glm::vec3 LocalToBody(const glm::vec3 &vector)
    {
        return glm::transpose(BodyToLocalMatrix()) * vector;
    }

    glm::mat3 InertiaBodyToLocal(const glm::mat3 &inertia)
    {
        const glm::mat3 basis = BodyToLocalMatrix();
        return basis * inertia * glm::transpose(basis);
    }

    float AngleOfAttack(const glm::vec3 &bodyVelocity)
    {
        if (glm::dot(bodyVelocity, bodyVelocity) == 0.0f)
            return 0.0f;
        return std::atan2(bodyVelocity.z, bodyVelocity.x);
    }

    float Sideslip(const glm::vec3 &bodyVelocity)
    {
        if (glm::dot(bodyVelocity, bodyVelocity) == 0.0f)
            return 0.0f;
        return std::atan2(bodyVelocity.y, std::sqrt(bodyVelocity.x * bodyVelocity.x + bodyVelocity.z * bodyVelocity.z));
    }
}
