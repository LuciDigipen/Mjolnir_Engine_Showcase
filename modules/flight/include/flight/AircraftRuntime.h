/**
 * @file AircraftRuntime.h
 * @author Jackey Cen
 * @brief The per-step aircraft state and the body-frame forces the pipeline accumulates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AerodynamicModel.h>

#include <core/ecs/ComponentFields.h>
#include <core/math/WorldReal.h>
#include <physics/PhysicsTypes.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace mir
{
    struct AircraftConfig;

    enum class FlightCommand : uint8_t
    {
        FlapsUp,
        FlapsDown,
        TrimUp,
        TrimDown,
    };

    inline constexpr std::size_t kFlightCommandCapacity = 16;

    // Discrete presses collected during a frame, applied in order by the next fixed step.
    struct FlightCommandQueue
    {
        FlightCommand commands[kFlightCommandCapacity]{};
        std::size_t count = 0;

        bool Push(FlightCommand command)
        {
            if (count == kFlightCommandCapacity)
                return false;
            commands[count++] = command;
            return true;
        }

        void Clear() { count = 0; }
    };

    // Analog controls held at the end of the frame.
    struct FlightControls
    {
        float pitch = 0.0f;
        float roll = 0.0f;
        float yaw = 0.0f;
        bool throttleUp = false;
        bool throttleDown = false;
        bool brake = false;
    };

    // Actuator deflections in radians, throttle in 0..1.
    struct ActuatorState
    {
        float elevator = 0.0f;
        float aileron = 0.0f;
        float rudder = 0.0f;
        float flaps = 0.0f;
        float elevatorLag = 0.0f;
        float aileronLag = 0.0f;
        float rudderLag = 0.0f;
        int flapDetent = 0;
        float trim = 0.0f;
        float throttle = 0.0f;
        bool brake = false;
    };

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
        // Script- and system-written wind, consumed and cleared by the environment stage.
        glm::vec3 additionalWind{ 0.0f };
        AirData airData;
        ForceMoment aero;
        FlightControls controls;
        FlightCommandQueue commands;
        ActuatorState actuators;
        // The path the RigidBody was last written for, and its decoded configuration; null until one loads.
        char appliedConfig[kFieldStringCapacity] = {};
        const AircraftConfig *config = nullptr;
        // The config already warned about for its missing tier profile.
        const AircraftConfig *aeroMissingConfig = nullptr;
    };
}
