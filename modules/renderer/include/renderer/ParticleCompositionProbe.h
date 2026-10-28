/**
 * @file ParticleCompositionProbe.h
 * @author sumin.park
 * @brief Checked world-space inputs for the static composition experiment.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <optional>

namespace mir
{
    // CPU/Slang layout, not an effect instance or particle buffer.
    struct ParticleProbeView
    {
        glm::mat4 invViewProj{1.0f};
        glm::vec4 anchor{0.0f}; // camera-relative, w = 0 copies source color only
        glm::vec4 rect{0.0f}; // pixels
        glm::vec4 sampling{0.0f}; // xy: inverse image extent, z: ray-distance depth
        glm::vec4 depthJitter{0.0f}; // xy: RT sampling jitter in pixels
    };
    static_assert(sizeof(ParticleProbeView) == 128);

    // Rejects singular/non-finite projections, empty rects and unrepresentable offsets.
    std::optional<ParticleProbeView> BuildParticleProbeView(WorldPosition anchor,
        WorldPosition camera, const glm::mat4 &viewProj, glm::vec4 rect,
        glm::vec2 invExtent, bool rayDistance);
}
