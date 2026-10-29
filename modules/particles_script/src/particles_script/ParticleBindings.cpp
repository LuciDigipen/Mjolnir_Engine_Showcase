/**
 * @file ParticleBindings.cpp
 * @author sumin.park
 * @brief Entity-based particle playback controls, resolved on each call.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_script/ParticleBindings.h"

#include <core/ecs/World.h>
#include <particles/ParticleWorld.h>

#include <sol/sol.hpp>

namespace mir
{
    void RegisterParticleBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("particle_system_start", [](World &world, Entity owner)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                return particles != nullptr && particles->Start(owner);
            });
        worldType.set_function("particle_system_stop", [](World &world, Entity owner)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                return particles != nullptr && particles->Stop(owner);
            });
        worldType.set_function("particle_system_pause", [](World &world, Entity owner, bool paused)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                return particles != nullptr && particles->Pause(owner, paused);
            });
        worldType.set_function("particle_system_reset", [](World &world, Entity owner)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                return particles != nullptr && particles->Reset(owner);
            });
        worldType.set_function("particle_system_destroy", [](World &world, Entity owner)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                return particles != nullptr && particles->Destroy(owner);
            });
        worldType.set_function("particle_system_parameters", [](World &world, Entity owner, float spawnRateScale, float sizeScale)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                if (!particles) return false;
                const auto *component = world.GetComponent<ParticleSystem>(owner);
                if (!component) return false;
                const auto *instance = particles->Find(owner);
                auto parameters = instance ? instance->parameters : component->userParameters;
                parameters.spawnRateScale = spawnRateScale;
                parameters.sizeScale = sizeScale;
                return particles->SetParameters(owner, parameters);
            });
        worldType.set_function("particle_system_wind", [](World &world, Entity owner, float x, float y, float z)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                const auto *component = world.GetComponent<ParticleSystem>(owner);
                if (!particles || !component) return false;
                const auto *instance = particles->Find(owner);
                auto parameters = instance ? instance->parameters : component->userParameters;
                parameters.wind = {x, y, z};
                return particles->SetParameters(owner, parameters);
            });
        worldType.set_function("particle_system_state", [](World &world, Entity owner)
            {
                auto *particles = world.TryResource<ParticleWorld>();
                const auto *instance = particles ? particles->Find(owner) : nullptr;
                if (instance == nullptr)
                    return std::string("complete");
                if (instance->paused)
                    return std::string("paused");
                return std::string(instance->state == ParticleExecutionState::Active ? "active" : "inactive");
            });
    }
}
