/**
 * @file AirData.h
 * @author Jackey Cen
 * @brief The aircraft's state relative to the air it flies through.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/FlightEnvironment.h>

#include <glm/vec3.hpp>

namespace mir
{
    struct AircraftState;

    // Below this true airspeed the flow direction is not meaningful.
    inline constexpr float kAirDataMinSpeed = 0.1f;

    struct AirData
    {
        glm::vec3 relativeVelocityBody{ 0.0f };
        float trueAirspeed = 0.0f;
        float angleOfAttack = 0.0f;
        float sideslip = 0.0f;
        float dynamicPressure = 0.0f;
        float density = 0.0f;
        float speedOfSound = 0.0f;
        float mach = 0.0f;
        float altitude = 0.0f;
    };

    AirData ComputeAirData(const AircraftState &state, const glm::vec3 &worldWind,
        const AtmosphereSample &atmosphere);
}
