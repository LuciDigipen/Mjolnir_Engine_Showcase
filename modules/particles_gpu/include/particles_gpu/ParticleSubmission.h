/**
 * @file ParticleSubmission.h
 * @author sumin.park
 * @brief Immutable renderer particle inputs and shared GPU record layout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kGpuParticleInstances = 64;
    inline constexpr uint32_t kGpuParticleCapacity = 65536;
    inline constexpr uint32_t kGpuParticleSpawnsPerFrame = 4096;

    // Immutable input, no dependency on the particle module or its handles.
    struct ParticleSubmission
    {
        uint64_t scriptKey = 0;
        std::vector<std::byte> scriptCode;
        uint32_t index = 0;
        uint32_t generation = 0;
        uint32_t capacity = 0;
        uint32_t spawns = 0;
        WorldPosition anchor;
        float seconds = 0;
        float lifetime = 1;
        glm::vec3 velocity{0, 1, 0};
        float size = 0.15f;
        glm::vec4 color{1, 0.4f, 0.1f, 0.5f};
    };

    // Shared with particlePool.slang, offsets are part of the GPU ABI.
    struct GpuParticle
    {
        glm::vec4 positionAge;
        glm::vec4 velocityLifetime;
        glm::vec4 sizeRotation;
        glm::vec4 color;
        glm::uvec4 identity;
    };
    static_assert(sizeof(GpuParticle) == 80);
    static_assert(offsetof(GpuParticle, velocityLifetime) == 16);
    static_assert(offsetof(GpuParticle, sizeRotation) == 32);
    static_assert(offsetof(GpuParticle, color) == 48);
    static_assert(offsetof(GpuParticle, identity) == 64);

    struct ParticleGpuStats
    {
        uint64_t completedFrame = 0;
        uint32_t alive = 0;
        uint32_t spawned = 0;
        uint32_t dropped = 0;
        uint64_t rejectedSubmissions = 0;
        uint64_t droppedSubmitSpawns = 0;
    };
}

