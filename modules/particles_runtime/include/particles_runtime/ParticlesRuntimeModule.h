/**
 * @file ParticlesRuntimeModule.h
 * @author sumin.park
 * @brief Adapts CPU playback into immutable renderer-owned particle submissions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <array>

namespace mir
{
    class ParticlesModule;
    class RendererModule;
    class ParticlesRuntimeModule
    {
    public:
        ParticlesRuntimeModule(ModuleContext context, ParticlesModule &particles, RendererModule &renderer);
        ParticlesRuntimeModule(const ParticlesRuntimeModule &) = delete;
        ParticlesRuntimeModule &operator=(const ParticlesRuntimeModule &) = delete;
    private:
        std::array<Registration, 5> mStages;
    };
}
