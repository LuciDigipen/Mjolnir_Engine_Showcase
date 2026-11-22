/**
 * @file FlightPropulsion.cpp
 * @author Jackey Cen
 * @brief Runs the engine rpm response and the thrust curve.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightPropulsion.h>

#include <flight/AircraftConfig.h>
#include <flight/FlightCurve.h>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>

namespace mir
{
    namespace
    {
        constexpr float kSeaLevelDensity = 1.225f;
    }

    void StepPropulsion(const AircraftConfig &config, AircraftRuntime &runtime, float dt)
    {
        PropulsionState &state = runtime.propulsion;
        state.force = glm::vec3{0.0f};
        state.moment = glm::vec3{0.0f};

        if (runtime.propulsionConfig != &config)
        {
            runtime.propulsionConfig = &config;
            for (std::size_t i = 0; i < kMaxEngines; ++i)
            {
                state.engines[i].rpm = i < config.engines.size() ? config.engines[i].idleRpm : 0.0f;
                state.engines[i].thrust = 0.0f;
            }
        }

        const float densityRatio = runtime.airData.density / kSeaLevelDensity;
        for (std::size_t i = 0; i < config.engines.size(); ++i)
        {
            const AircraftConfig::Engine &engine = config.engines[i];
            EngineState &engineState = state.engines[i];

            const float target = engine.idleRpm + runtime.actuators.throttle * (engine.maxRpm - engine.idleRpm);
            engineState.rpm += (target - engineState.rpm) * (1.0f - std::exp(-dt / engine.rpmTau));

            const float axial = glm::dot(runtime.airData.relativeVelocityBody, engine.direction);
            const float thrust = EvaluateCurve(engine.staticThrust, engineState.rpm / engine.maxRpm)
                * EvaluateCurve(engine.speedFactor, axial) * densityRatio;
            engineState.thrust = thrust;

            const glm::vec3 force = engine.direction * thrust;
            state.force += force;
            state.moment += glm::cross(engine.position - config.centerOfMass, force);
        }

        for (std::size_t i = config.engines.size(); i < kMaxEngines; ++i)
            state.engines[i].thrust = 0.0f;
    }
}
