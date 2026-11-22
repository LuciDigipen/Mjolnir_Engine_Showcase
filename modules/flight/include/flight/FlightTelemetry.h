/**
 * @file FlightTelemetry.h
 * @author Jackey Cen
 * @brief The per-aircraft flight quantities a HUD or a script reads after a step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftConfig.h>

#include <glm/vec3.hpp>

#include <cstddef>

namespace mir
{
    struct AirData;
    struct AircraftRuntime;
    struct AircraftState;

    // The gate's downward ground ray never reaches farther than this, metres.
    inline constexpr float kTelemetryGroundRayLength = 10000.0f;
    // Reported as the stall margin when the config has no L2 profile.
    inline constexpr float kTelemetryNoStallMargin = 1000.0f;

    // Body frame, about the centre of mass.
    struct TelemetryForces
    {
        glm::vec3 force{0.0f};
        glm::vec3 moment{0.0f};
    };

    // SI units and radians; the gate fills it in AfterStep from the post-step body state.
    struct FlightTelemetry
    {
        float trueAirspeed = 0.0f;      // m/s
        float indicatedAirspeed = 0.0f; // m/s, at sea-level density
        float altitude = 0.0f;          // m above sea level
        float heightAboveGround = 0.0f; // m, 0 when hasGround is false
        bool hasGround = false;
        float verticalSpeed = 0.0f;    // m/s, world +Y
        float heading = 0.0f;          // radians, 0 along world -Z, increasing toward world +X
        float pitch = 0.0f;            // radians, nose elevation
        float roll = 0.0f;             // radians, positive right wing down
        float angleOfAttack = 0.0f;    // radians
        float sideslip = 0.0f;         // radians
        float dynamicPressure = 0.0f;  // Pa
        float mach = 0.0f;
        float loadFactor = 0.0f;       // g
        float stallMargin = 0.0f;      // radians, critical alpha minus angle of attack
        bool stallWarning = false;
        bool aeroOutOfTable = false;   // the L3 input was clamped to a table edge
        float throttle = 0.0f;
        float rpm[kMaxEngines]{};
        std::size_t engineCount = 0;
        float flaps = 0.0f;    // radians
        float trim = 0.0f;     // radians
        float elevator = 0.0f; // radians
        float aileron = 0.0f;  // radians
        float rudder = 0.0f;   // radians
        float gearCompression[kMaxGear]{};
        std::size_t gearCount = 0;
        bool weightOnWheels = false;
        TelemetryForces aeroForces;
        TelemetryForces propulsionForces;
        TelemetryForces gearForces;
    };

    // Fills out from a re-read state and its post-step air data; the gate supplies the ground query.
    void FillFlightTelemetry(const AircraftConfig &config, const AircraftState &state, const AirData &airData,
        float heightAboveGround, bool hasGround, const AircraftRuntime &runtime, FlightTelemetry &out);
}
