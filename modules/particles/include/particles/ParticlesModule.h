/**
 * @file ParticlesModule.h
 * @author sumin.park
 * @brief Registers particle components, lifecycle stages and CPU playback.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/ecs/ModuleContext.h>

#include <array>
#include <cstdint>
#include <string>

namespace mir
{
    class AssetResolver;
    class AssetsModule;
    class ParticleWorld;

    class ParticlesModule
    {
    public:
        ParticlesModule(ModuleContext context, AssetsModule &assets, const AssetResolver &resolver,
            uint32_t capacity = 64);
        ParticlesModule(const ParticlesModule &) = delete;
        ParticlesModule &operator=(const ParticlesModule &) = delete;
        ParticleWorld &Particles() { return mParticles; }

    private:
        // Owned by the World, which outlives the module and its registrations.
        ParticleWorld &mParticles;
        Registration mHooks;
        std::array<Registration, 7> mStages;
    };
}
