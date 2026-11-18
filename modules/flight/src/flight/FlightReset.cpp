/**
 * @file FlightReset.cpp
 * @author Jackey Cen
 * @brief Puts an aircraft back at a pose with zeroed controls, gear state and engines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightReset.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/World.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>
#include <renderer/CameraCut.h>
#include <renderer/components/Camera.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstddef>

namespace mir
{
    namespace
    {
        void ResetActuators(float throttle, ActuatorState &actuators)
        {
            actuators = {};
            actuators.throttle = std::clamp(throttle, 0.0f, 1.0f);
        }

        void ResetEngines(const AircraftConfig &config, float throttle, PropulsionState &propulsion)
        {
            for (std::size_t i = 0; i < kMaxEngines; ++i)
            {
                if (i >= config.engines.size())
                {
                    propulsion.engines[i] = {};
                    continue;
                }

                const AircraftConfig::Engine &engine = config.engines[i];
                propulsion.engines[i].rpm = engine.idleRpm + throttle * (engine.maxRpm - engine.idleRpm);
                propulsion.engines[i].thrust = 0.0f;
            }
        }

        void CutActiveCameras(World &world)
        {
            world.ForEachAliveEntity(
                [&](Entity entity)
                {
                    const Camera *camera = world.GetComponent<Camera>(entity);
                    if (camera != nullptr && camera->mActive)
                        CutCamera(world, entity);
                });
        }
    }

    bool ResetAircraft(World &world, Entity entity, WorldPosition position, glm::quat rotation, float airspeed,
        float throttle)
    {
        Aircraft *aircraft = world.GetComponent<Aircraft>(entity);
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
        if (aircraft == nullptr || rigidBody == nullptr || physics == nullptr || !HasDynamicBody(*rigidBody))
            return false;
        if (aircraft->runtime.config == nullptr)
            return false;

        const BodyHandle body = rigidBody->runtime.body;
        if (!physics->BodyPosition(body))
            return false;
        if (!PhysicsSystem::Teleport(world, entity, position, rotation))
            return false;

        const float clampedThrottle = std::clamp(throttle, 0.0f, 1.0f);
        if (!physics->SetBodyLinearVelocity(body, rotation * BodyToLocal(glm::vec3{1.0f, 0.0f, 0.0f}) * airspeed)
            || !physics->SetBodyAngularVelocity(body, glm::vec3{0.0f}))
            return false;

        AircraftRuntime &runtime = aircraft->runtime;
        const AircraftConfig &config = *runtime.config;
        ResetActuators(clampedThrottle, runtime.actuators);
        ResetEngines(config, clampedThrottle, runtime.propulsion);
        runtime.gear = {};
        runtime.commands.Clear();
        runtime.pendingTouchdowns.Clear();

        CutActiveCameras(world);
        return true;
    }
}
