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
    };
}
