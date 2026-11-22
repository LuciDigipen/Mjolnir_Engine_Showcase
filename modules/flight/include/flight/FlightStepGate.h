/**
 * @file FlightStepGate.h
 * @author Jackey Cen
 * @brief Runs the flight pipeline on every Aircraft before each physics step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsTypes.h>

namespace mir
{
    struct Aircraft;
    struct FlightEnvironment;

    class FlightStepGate final : public PhysicsStepGate
    {
      public:
        void BeforeStep(World &world, PhysicsWorld &physics, float dt) override;
        void AfterStep(World &world, PhysicsWorld &physics, float dt) override;

      private:
        // Environment and air data, actuators, propulsion, landing gear, aerodynamics, in this order.
        void RunEnvironmentAndAirData(Aircraft &aircraft, const FlightEnvironment &environment, float dt);
        void RunActuators(Aircraft &aircraft, float dt);
        void RunPropulsion(Aircraft &aircraft, float dt);
        void RunLandingGear(Aircraft &aircraft, const WorldPosition &origin, PhysicsWorld &physics,
            BodyHandle body, float dt);
        void RunAerodynamics(Aircraft &aircraft, float dt);
        void RunTelemetry(Aircraft &aircraft, PhysicsWorld &physics, const WorldPosition &position, BodyHandle body,
            const FlightEnvironment &environment);
    };
}
