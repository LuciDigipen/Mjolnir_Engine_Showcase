/**
 * @file ParticleSystem.h
 * @author sumin.park
 * @brief Component effect reference and playback state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleRuntime.h"

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>

namespace mir
{
    struct ParticleSystem
    {
        char systemName[kFieldStringCapacity] = {};
        bool playOnStart = true;
        bool overrideParameters = false;
        ParticleUserParameters userParameters;
        glm::vec3 emitterOffsets[kMaxSystemEmitters] = {};// Per-instance origin of each emitter placement
        ParticleHandle instance;
        bool initialized = false;
        bool wantsPlay = false;
        bool paused = false;
        AssetId failedEffect;
    };
    MIR_ASSERT_COMPONENT(ParticleSystem);

    inline constexpr FieldDesc kParticleSystemFields[] = {
        { "systemName", FieldKind::String, offsetof(ParticleSystem, systemName) },
        { "playOnStart", FieldKind::Bool, offsetof(ParticleSystem, playOnStart) },
        { "overrideParameters", FieldKind::Bool, offsetof(ParticleSystem, overrideParameters) },
        { "wind", FieldKind::Vec3, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, wind) },
        { "spawnRateScale", FieldKind::Float, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, spawnRateScale) },
        { "sizeScale", FieldKind::Float, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, sizeScale) },
        // Edited in the particle panel's emitter list, so the inspector hides them.
        { .mName = "emitterOffset0", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets), .mHidden = true },
        { .mName = "emitterOffset1", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 1 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset2", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 2 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset3", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 3 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset4", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 4 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset5", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 5 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset6", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 6 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset7", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 7 * sizeof(glm::vec3), .mHidden = true },
    };
    static_assert(kMaxSystemEmitters == 8, "kParticleSystemFields lists one emitterOffset field per placement");
}
