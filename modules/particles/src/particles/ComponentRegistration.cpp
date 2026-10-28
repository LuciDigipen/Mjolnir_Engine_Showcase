/**
 * @file ComponentRegistration.cpp
 * @author sumin.park
 * @brief Registers particle emitter authored fields without runtime handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ComponentRegistration.h"
#include "particles/components/ParticleEmitter.h"

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterParticleComponents()
    {
        ComponentRegistry::Instance().Register<ParticleEmitter>(kParticleEmitterFields);
    }
}
