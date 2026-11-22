/**
 * @file FlightTelemetry.cpp
 * @author Jackey Cen
 * @brief Fills the per-aircraft telemetry from the post-step body state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightTelemetry.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/AirData.h>
#include <flight/FlightFrames.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        constexpr float kSeaLevelDensity = 1.225f;
        constexpr float kStandardGravity = 9.81f;

        float Heading(const glm::vec3 &nose)
        {
            float heading = std::atan2(nose.x, -nose.z);
            if (heading < 0.0f)
                heading += glm::two_pi<float>();
            return heading;
        }

        void FillStall(const AircraftConfig &config, const AircraftRuntime &runtime, const AirData &airData,
            FlightTelemetry &out)
        {
            if (!config.aero.l2)
            {
                out.stallMargin = kTelemetryNoStallMargin;
                return;
            }

            out.stallMargin = L2CriticalAngleOfAttack(*config.aero.l2, runtime.actuators.flaps) - airData.angleOfAttack;
            out.stallWarning = airData.trueAirspeed >= kAirDataMinSpeed && out.stallMargin < config.stallWarningMargin;
        }

        void FillEngines(const AircraftConfig &config, const AircraftRuntime &runtime, FlightTelemetry &out)
        {
            out.engineCount = std::min(config.engines.size(), kMaxEngines);
            for (std::size_t i = 0; i < out.engineCount; ++i)
                out.rpm[i] = runtime.propulsion.engines[i].rpm;
        }

        void FillGear(const AircraftConfig &config, const AircraftRuntime &runtime, FlightTelemetry &out)
        {
            out.gearCount = std::min(config.gear.size(), kMaxGear);
            for (std::size_t i = 0; i < out.gearCount; ++i)
            {
                out.gearCompression[i] = runtime.gear.points[i].compression;
                out.weightOnWheels = out.weightOnWheels || runtime.gear.points[i].inContact;
            }
        }
    }

    void FillFlightTelemetry(const AircraftConfig &config, const AircraftState &state, const AirData &airData,
        float heightAboveGround, bool hasGround, const AircraftRuntime &runtime, FlightTelemetry &out)
    {
        out = FlightTelemetry{};

        out.trueAirspeed = airData.trueAirspeed;
        out.indicatedAirspeed = airData.trueAirspeed * std::sqrt(airData.density / kSeaLevelDensity);
        out.altitude = airData.altitude;
        out.heightAboveGround = hasGround ? heightAboveGround : 0.0f;
        out.hasGround = hasGround;
        out.verticalSpeed = state.linearVelocity.y;

        const glm::vec3 nose = state.rotation * BodyToLocal(glm::vec3{1.0f, 0.0f, 0.0f});
        const glm::vec3 rightWing = state.rotation * BodyToLocal(glm::vec3{0.0f, 1.0f, 0.0f});
        const glm::vec3 up = state.rotation * BodyToLocal(glm::vec3{0.0f, 0.0f, -1.0f});
        out.heading = Heading(nose);
        out.pitch = std::asin(std::clamp(nose.y, -1.0f, 1.0f));
        out.roll = std::atan2(-rightWing.y, up.y);

        out.angleOfAttack = airData.angleOfAttack;
        out.sideslip = airData.sideslip;
        out.dynamicPressure = airData.dynamicPressure;
        out.mach = airData.mach;

        const glm::vec3 totalForce = runtime.aero.force + runtime.propulsion.force + runtime.gear.force;
        out.loadFactor = -totalForce.z / (config.mass * kStandardGravity);

        FillStall(config, runtime, airData, out);

        out.throttle = runtime.actuators.throttle;
        out.flaps = runtime.actuators.flaps;
        out.trim = runtime.actuators.trim;
        out.elevator = runtime.actuators.elevator;
        out.aileron = runtime.actuators.aileron;
        out.rudder = runtime.actuators.rudder;

        FillEngines(config, runtime, out);
        FillGear(config, runtime, out);

        out.aeroOutOfTable = runtime.aero.outOfTable;
        out.aeroForces = {runtime.aero.force, runtime.aero.moment};
        out.propulsionForces = {runtime.propulsion.force, runtime.propulsion.moment};
        out.gearForces = {runtime.gear.force, runtime.gear.moment};
    }
}
