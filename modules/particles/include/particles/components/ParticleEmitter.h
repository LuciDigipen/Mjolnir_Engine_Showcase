/**
 * @file ParticleEmitter.h
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
    struct ParticleEmitter
    {
        char effectName[kFieldStringCapacity] = {};
        bool playOnStart = true;
        ParticleHandle instance;
        bool initialized = false;
        bool wantsPlay = false;
        bool paused = false;
        AssetId failedEffect;
    };
    MIR_ASSERT_COMPONENT(ParticleEmitter);

    inline constexpr FieldDesc kParticleEmitterFields[] = {
        { "effectName", FieldKind::String, offsetof(ParticleEmitter, effectName) },
        { "playOnStart", FieldKind::Bool, offsetof(ParticleEmitter, playOnStart) },
    };
}
