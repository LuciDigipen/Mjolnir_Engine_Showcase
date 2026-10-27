/**
 * @file MeshFrontend.cpp
 * @author Jackey Cen
 * @brief CPU mirror of the mesh tangent frame used by the surface shaders.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshFrontend.h>

#include <glm/geometric.hpp>

#include <cmath>

namespace mir
{
    glm::vec4 TransformMeshTangent(const glm::mat4 &model, const glm::vec3 &normalWS, const glm::vec4 &tangentOS)
    {
        const glm::mat3 linear(model);
        glm::vec3 tangent = linear * glm::vec3(tangentOS);
        const glm::vec3 normal = glm::normalize(normalWS);
        tangent -= normal * glm::dot(normal, tangent);
        if (glm::dot(tangent, tangent) < 1e-12f)
            tangent = glm::cross(normal, std::abs(normal.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
        tangent = glm::normalize(tangent);

        float handedness = tangentOS.w < 0.0f ? -1.0f : 1.0f;
        const glm::vec3 c0(linear[0]);
        const glm::vec3 c1(linear[1]);
        const glm::vec3 c2(linear[2]);
        if (glm::dot(c0, glm::cross(c1, c2)) < 0.0f)
            handedness = -handedness;
        return glm::vec4(tangent, handedness);
    }
}
