/**
 * @file ParticleRibbonGeometry.h
 * @author sumin.park
 * @brief Per-view ribbon geometry built after world precision camera subtraction.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles_gpu/ParticleSubmission.h"

#include <vector>

namespace mir
{
    inline constexpr uint32_t kRibbonVerticesPerFrame = 65532;
    struct RibbonVertex
    {
        glm::vec4 position;
        glm::vec4 color;
        glm::vec4 uv;
    };
    static_assert(sizeof(RibbonVertex) == 48);
    std::vector<RibbonVertex> BuildRibbonGeometry(std::span<const RibbonSubmission> ribbons,
        WorldPosition camera, glm::vec3 cameraRight, uint32_t maxVertices = kRibbonVerticesPerFrame);
}
