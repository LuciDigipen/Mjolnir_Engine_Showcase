/**
 * @file AirData.cpp
 * @author Jackey Cen
 * @brief The aircraft's state relative to the air it flies through.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AirData.h>

#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

namespace mir
{
    AirData ComputeAirData(const AircraftState &state, const glm::vec3 &worldWind,
        const AtmosphereSample &atmosphere)
    {
        AirData airData;
        const glm::vec3 relativeVelocity = state.linearVelocity - worldWind;
        airData.relativeVelocityBody = LocalToBody(glm::inverse(state.rotation) * relativeVelocity);
        airData.trueAirspeed = glm::length(airData.relativeVelocityBody);

        if (airData.trueAirspeed >= kAirDataMinSpeed)
        {
            airData.angleOfAttack = AngleOfAttack(airData.relativeVelocityBody);
            airData.sideslip = Sideslip(airData.relativeVelocityBody);
        }

        airData.dynamicPressure = 0.5f * atmosphere.density * airData.trueAirspeed * airData.trueAirspeed;
        airData.density = atmosphere.density;
        airData.speedOfSound = atmosphere.speedOfSound;
        airData.mach = airData.trueAirspeed / atmosphere.speedOfSound;
        airData.altitude = static_cast<float>(atmosphere.altitude);
        return airData;
    }
}
