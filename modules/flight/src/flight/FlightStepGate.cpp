/**
 * @file FlightStepGate.cpp
 * @author Jackey Cen
 * @brief Runs the flight pipeline on every Aircraft before each physics step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightStepGate.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightActuators.h>
#include <flight/FlightEnvironment.h>
#include <flight/FlightFrames.h>
#include <flight/FlightPropulsion.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <glm/gtc/quaternion.hpp>

#include <optional>

namespace mir
{
    void FlightStepGate::BeforeStep(World &world, PhysicsWorld &physics, float dt)
    {
        const FlightEnvironment *environment = world.TryResource<FlightEnvironment>();
        const FlightEnvironment fallbackEnvironment;

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

                RunEnvironmentAndAirData(aircraft, environment ? *environment : fallbackEnvironment, dt);
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

    void FlightStepGate::RunEnvironmentAndAirData(Aircraft &aircraft, const FlightEnvironment &environment, float)
    {
        AircraftRuntime &runtime = aircraft.runtime;
        const AtmosphereSample atmosphere = SampleAtmosphere(environment, runtime.state.position.y);
        runtime.airData =
            ComputeAirData(runtime.state, environment.wind + runtime.additionalWind, atmosphere);
        runtime.additionalWind = glm::vec3{ 0.0f };
    }

    void FlightStepGate::RunActuators(Aircraft &aircraft, float dt)
    {
        if (aircraft.runtime.config == nullptr)
            return;

        AircraftRuntime &runtime = aircraft.runtime;
        StepActuators(*runtime.config, runtime.controls, runtime.commands, runtime.actuators, dt);
    }

    void FlightStepGate::RunPropulsion(Aircraft &aircraft, float dt)
    {
        AircraftRuntime &runtime = aircraft.runtime;
        if (runtime.config == nullptr)
            return;

        StepPropulsion(*runtime.config, runtime, dt);
        runtime.accumulator.force += runtime.propulsion.force;
        runtime.accumulator.moment += runtime.propulsion.moment;
    }

    void FlightStepGate::RunLandingGear(Aircraft &, float) {}

    void FlightStepGate::RunAerodynamics(Aircraft &aircraft, float)
    {
        AircraftRuntime &runtime = aircraft.runtime;
        if (runtime.config == nullptr)
            return;

        const AircraftConfig &config = *runtime.config;
        if (!HasAerodynamicProfile(config.aero, aircraft.tier))
        {
            if (runtime.aeroMissingConfig != runtime.config)
            {
                runtime.aeroMissingConfig = runtime.config;
                MIR_LOG_WARN("Aircraft '{}': no {} aerodynamic profile, so it gets no aerodynamic force",
                    aircraft.config, kAerodynamicTierNames[static_cast<int>(aircraft.tier)]);
            }
            return;
        }

        AeroInput input;
        input.air = runtime.airData;
        input.angularVelocityBody = runtime.state.angularVelocityBody;
        input.elevator = runtime.actuators.elevator;
        input.aileron = runtime.actuators.aileron;
        input.rudder = runtime.actuators.rudder;
        input.flaps = runtime.actuators.flaps;
        input.elevatorMax = config.actuators.elevator.maxDeflection;
        input.aileronMax = config.actuators.aileron.maxDeflection;
        input.rudderMax = config.actuators.rudder.maxDeflection;
        input.area = config.reference.area;
        input.span = config.reference.span;
        input.chord = config.reference.chord;

        runtime.aero = AerodynamicModelFor(aircraft.tier).Evaluate(input, config.aero);
        runtime.accumulator.force += runtime.aero.force;
        runtime.accumulator.moment += runtime.aero.moment;
    }
}
