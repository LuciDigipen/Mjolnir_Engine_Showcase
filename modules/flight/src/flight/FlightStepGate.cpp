/**
 * @file FlightStepGate.cpp
 * @author Jackey Cen
 * @brief Runs the flight pipeline on every Aircraft before each physics step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightStepGate.h>

#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <glm/gtc/quaternion.hpp>

#include <optional>

namespace mir
{
    void FlightStepGate::BeforeStep(World &world, PhysicsWorld &physics, float dt)
    {
        world.GetOrCreateQuery<Aircraft, RigidBody>().ForEach(
            [&](Entity, Aircraft &aircraft, RigidBody &rigidBody)
            {
                if (aircraft.runtime.config == nullptr)
                    return;

                if (!HasDynamicBody(rigidBody))
                    return;

                const BodyHandle body = rigidBody.runtime.body;
                const std::optional<WorldPosition> position = physics.BodyPosition(body);
                if (!position)
                    return;

                AircraftState &state = aircraft.runtime.state;
                state.position = position->Meters();
                state.rotation = physics.BodyRotation(body);
                state.linearVelocity = physics.BodyLinearVelocity(body);
                state.angularVelocity = physics.BodyAngularVelocity(body);
                const glm::quat toLocal = glm::inverse(state.rotation);
                state.linearVelocityBody = LocalToBody(toLocal * state.linearVelocity);
                state.angularVelocityBody = LocalToBody(toLocal * state.angularVelocity);
                aircraft.runtime.accumulator = {};

                RunEnvironmentAndAirData(aircraft, dt);
                RunActuators(aircraft, dt);
                RunPropulsion(aircraft, dt);
                RunLandingGear(aircraft, dt);
                RunAerodynamics(aircraft, dt);

                const ForceAccumulator &accumulator = aircraft.runtime.accumulator;
                if (accumulator.force != glm::vec3{0.0f})
                    physics.AddForce(body, state.rotation * BodyToLocal(accumulator.force));
                if (accumulator.moment != glm::vec3{0.0f})
                    physics.AddTorque(body, state.rotation * BodyToLocal(accumulator.moment));
            });
    }

    void FlightStepGate::AfterStep(World &, PhysicsWorld &, float) {}

    void FlightStepGate::RunEnvironmentAndAirData(Aircraft &, float) {}

    void FlightStepGate::RunActuators(Aircraft &, float) {}

    void FlightStepGate::RunPropulsion(Aircraft &, float) {}

    void FlightStepGate::RunLandingGear(Aircraft &, float) {}

    void FlightStepGate::RunAerodynamics(Aircraft &, float) {}
}
