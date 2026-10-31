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

    struct ParticleGpuBudgets
    {
        uint64_t snapshotBytes = 64ull * 1024 * 1024;
        uint64_t textureBytes = 64ull * 1024 * 1024;
        uint64_t textureUploadBytes = 16ull * 1024 * 1024;
        uint32_t birthUploads = kGpuParticleSpawnsPerFrame;
        uint32_t ribbonVertices = 65532;
    };

    struct ParticleGpuBirth
    {
        float fraction = 1;
        float age = 0;
        uint32_t sequence = 0;
        uint32_t reserved = 0;
    };
    static_assert(sizeof(ParticleGpuBirth) == 16);

    struct ParticleRenderProperties
    {
        uint32_t alignment = 0, facing = 0, blend = 0, sort = 0;
        float softDistance = 0.25f, maxDistance = 65536.0f;
        bool lit = false, fog = true, textured = false;
        float stretch = 0.0f;
    };
    bool ValidParticleRenderProperties(const ParticleRenderProperties &properties);

    // Immutable input, no dependency on the particle module or its handles.
    struct ParticleSubmission
    {
        ParticleRenderProperties renderer;
        uint64_t scriptKey = 0;
        std::vector<std::byte> scriptCode;
        uint32_t index = 0;
        uint32_t generation = 0;
        uint32_t capacity = 0;
        uint32_t spawns = 0;
        WorldPosition anchor;
        WorldPosition emissionStart;
        WorldPosition tileOrigin;
        bool explicitTile = false;
        bool swept = false;
        glm::vec3 attachmentVelocity{0};
        // Extra shift for live particles, so an emitter that follows its attachment carries them along.
        glm::vec3 followDelta{0};
        glm::vec3 wind{0};
        float inheritVelocity = 0;
        uint32_t seed = 1;
        std::vector<ParticleGpuBirth> births;
        uint64_t textureKey = 0;
        uint32_t textureWidth = 0, textureHeight = 0;
        std::vector<uint8_t> texturePixels;
        float seconds = 0;
        float lifetime = 1;
        glm::vec3 velocity{0, 1, 0};
        float size = 0.15f;
        glm::vec4 color{1, 0.4f, 0.1f, 0.5f};
    };

    struct RibbonPointSubmission
    {
        WorldPosition position;
        double textureDistance = 0;
        uint64_t ribbon = 0;
        uint64_t link = 0;
        float width = 0;
        glm::vec4 color{0};
    };
    struct RibbonSubmission
    {
        std::vector<RibbonPointSubmission> points;
        ParticleRenderProperties renderer;
        uint64_t textureKey = 0;
        uint32_t textureWidth = 0, textureHeight = 0;
        std::vector<uint8_t> texturePixels;
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
        WorldPosition firstPosition;
        glm::vec3 firstVelocity{0};
        float firstAge = 0;
        float firstSize = 0;
        float firstAlpha = 0;
        uint32_t firstFrame = 0;
        uint32_t firstSequence = 0;
        uint64_t gpuBytes = 0, gpuHighWaterBytes = 0, snapshotBytes = 0, uploadBytes = 0;
        uint64_t deferredTextures = 0, droppedRibbonSegments = 0;
        double cpuRecordMs = 0, simulationGpuMs = 0, uploadGpuMs = 0, prepareGpuMs = 0, drawGpuMs = 0;
        double cpuSimulationRecordMs = 0, cpuTextureRecordMs = 0, cpuPrepareRecordMs = 0, cpuDrawRecordMs = 0;
        bool gpuTimingAvailable = false;
        uint64_t tileShifts = 0;
        uint64_t completedFrame = 0;
        uint64_t renderFrame = 0;
        uint32_t visible[16]{};
        uint32_t firstSortedParticle[16]{};
        uint32_t alive = 0;
        uint32_t spawned = 0;
        uint32_t dropped = 0;
        uint64_t rejectedSubmissions = 0;
        uint64_t droppedSubmitSpawns = 0;
    };
}

