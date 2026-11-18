/**
 * @file AircraftRuntime.h
 * @author Jackey Cen
 * @brief The per-step aircraft state and the body-frame forces the pipeline accumulates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ComponentFields.h>
#include <core/math/WorldReal.h>
#include <physics/PhysicsTypes.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    struct AircraftConfig;

    struct AircraftState
    {
        WorldVec3 position{0};
        // Entity to world.
        glm::quat rotation = kIdentityRotation;
        glm::vec3 linearVelocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        glm::vec3 linearVelocityBody{0.0f};
        glm::vec3 angularVelocityBody{0.0f};
    };

    // Body-frame force and moment about the centre of mass, cleared before every step.
    struct ForceAccumulator
    {
        glm::vec3 force{0.0f};
        glm::vec3 moment{0.0f};
    };

    struct AircraftRuntime
    {
        AircraftState state;
        ForceAccumulator accumulator;
        // The path the RigidBody was last written for, and its decoded configuration; null until one loads.
        char appliedConfig[kFieldStringCapacity] = {};
        const AircraftConfig *config = nullptr;
    };
}
