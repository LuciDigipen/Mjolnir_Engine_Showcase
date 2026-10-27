/**
 * @file MeshFrontend.h
 * @author Jackey Cen
 * @brief CPU mirror of the mesh tangent frame used by the surface shaders.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace mir
{
    // Transforms an object-space tangent by the model linear transform,
    // orthogonalizes it against the transformed normal, and flips tangent.w
    // when the transform mirrors. SurfaceForward.slang and SurfaceRt.slang use
    // the same steps.
    glm::vec4 TransformMeshTangent(const glm::mat4 &model, const glm::vec3 &normalWS, const glm::vec4 &tangentOS);
}
