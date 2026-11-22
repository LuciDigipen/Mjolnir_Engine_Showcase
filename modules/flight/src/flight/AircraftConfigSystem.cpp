/**
 * @file AircraftConfigSystem.cpp
 * @author Jackey Cen
 * @brief Applies an aircraft's configuration to its RigidBody before the physics sync.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfigSystem.h>

#include <flight/FlightFrames.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <physics/components/RigidBody.h>

#include <glm/mat3x3.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace mir
{
    void WriteMassProperties(const AircraftConfig &config, RigidBody &rigidBody)
    {
        const glm::mat3 bodyTensor{
            glm::vec3{config.inertiaDiagonal.x, -config.inertiaProducts.x, -config.inertiaProducts.y},
            glm::vec3{-config.inertiaProducts.x, config.inertiaDiagonal.y, -config.inertiaProducts.z},
            glm::vec3{-config.inertiaProducts.y, -config.inertiaProducts.z, config.inertiaDiagonal.z},
        };
        const glm::mat3 local = InertiaBodyToLocal(bodyTensor);

        rigidBody.mass = config.mass;
        rigidBody.customInertia = true;
        rigidBody.inertiaDiagonal = glm::vec3{local[0][0], local[1][1], local[2][2]};
        rigidBody.inertiaProducts = glm::vec3{-local[0][1], -local[0][2], -local[1][2]};
        rigidBody.customCenterOfMass = true;
        rigidBody.centerOfMassOffset = BodyToLocal(config.centerOfMass);
        rigidBody.linearDamping = 0.0f;
        rigidBody.angularDamping = 0.0f;
        rigidBody.gyroscopicForce = true;
        rigidBody.motionQuality = PhysicsMotionQuality::LinearCast;
        rigidBody.interpolate = true;
    }

    AircraftConfigSystem::AircraftConfigSystem(AircraftConfigStore &store) : mStore(store) {}

    void AircraftConfigSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Aircraft, RigidBody>();
    }

    void AircraftConfigSystem::OnUpdate(SystemContext &context)
    {
        // Edit mode would write the derived values into the authored RigidBody.
        if (!IsSimulating(context.world))
            return;

        mQuery->ForEach(
            [this](Entity, Aircraft &aircraft, RigidBody &rigidBody)
            {
                const std::string_view path = aircraft.config;
                const uint32_t generation = mStore.Generation();
                const bool pathChanged = path != std::string_view(aircraft.runtime.appliedConfig);
                const bool reloaded = aircraft.runtime.configGeneration != generation;
                const uint32_t editGeneration = mStore.EditGeneration();
                const bool edited = aircraft.runtime.configEditGeneration != editGeneration;
                if (!pathChanged && !reloaded && !edited)
                    return;

                aircraft.runtime.configEditGeneration = editGeneration;
                if (!pathChanged && !reloaded)
                {
                    if (aircraft.runtime.config != nullptr)
                        WriteMassProperties(*aircraft.runtime.config, rigidBody);
                    return;
                }

                if (pathChanged)
                    std::memcpy(aircraft.runtime.appliedConfig, aircraft.config,
                        sizeof(aircraft.runtime.appliedConfig));
                aircraft.runtime.config = path.empty() ? nullptr : mStore.Get(path);
                aircraft.runtime.configGeneration = generation;
                if (aircraft.runtime.config == nullptr)
                    return;

                if (reloaded)
                {
                    // Keep the stages that compare their config pointer re-running on the swapped config.
                    aircraft.runtime.aeroMissingConfig = nullptr;
                    aircraft.runtime.propulsionConfig = nullptr;
                    aircraft.runtime.cameraConfig = nullptr;
                }

                WriteMassProperties(*aircraft.runtime.config, rigidBody);
            });
    }
}
