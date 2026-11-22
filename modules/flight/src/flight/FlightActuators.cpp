/**
 * @file FlightActuators.cpp
 * @author Jackey Cen
 * @brief The actuator stage: commands and held controls to surface, flap, trim and throttle state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightActuators.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        float MoveToward(float current, float target, float maxDelta)
        {
            return current + std::clamp(target - current, -maxDelta, maxDelta);
        }

        float Lagged(float current, float target, float delay, float dt)
        {
            if (delay <= 0.0f)
                return target;
            return current + (target - current) * (1.0f - std::exp(-dt / delay));
        }

        float StepSurface(float current, float &lag, float target, const AircraftConfig::Actuator &config, float dt)
        {
            lag = Lagged(lag, target, config.delay, dt);
            return MoveToward(current, lag, config.rate * dt);
        }

        void ApplyCommand(const AircraftConfig &config, FlightCommand command, ActuatorState &state)
        {
            const int lastDetent = std::max(0, static_cast<int>(config.actuators.flaps.detents.size()) - 1);
            switch (command)
            {
            case FlightCommand::FlapsUp:
                state.flapDetent = std::max(0, state.flapDetent - 1);
                break;
            case FlightCommand::FlapsDown:
                state.flapDetent = std::min(lastDetent, state.flapDetent + 1);
                break;
            case FlightCommand::TrimUp:
                state.trim = std::min(state.trim + config.actuators.trim.step, config.actuators.trim.max);
                break;
            case FlightCommand::TrimDown:
                state.trim = std::max(state.trim - config.actuators.trim.step, -config.actuators.trim.max);
                break;
            }
        }
    }

    void StepActuators(const AircraftConfig &config, const FlightControls &controls, FlightCommandQueue &commands,
        ActuatorState &state, float dt)
    {
        for (std::size_t i = 0; i < commands.count; ++i)
            ApplyCommand(config, commands.commands[i], state);
        commands.Clear();

        const AircraftConfig::Actuators &actuators = config.actuators;

        const float elevatorTarget =
            std::clamp(controls.pitch * actuators.elevator.maxDeflection + state.trim, -actuators.elevator.maxDeflection,
                actuators.elevator.maxDeflection);
        state.elevator = StepSurface(state.elevator, state.elevatorLag, elevatorTarget, actuators.elevator, dt);

        const float aileronTarget = controls.roll * actuators.aileron.maxDeflection;
        state.aileron = StepSurface(state.aileron, state.aileronLag, aileronTarget, actuators.aileron, dt);

        const float rudderTarget = controls.yaw * actuators.rudder.maxDeflection;
        state.rudder = StepSurface(state.rudder, state.rudderLag, rudderTarget, actuators.rudder, dt);

        // A reloaded config may have fewer detents than the index points at.
        state.flapDetent = std::clamp(state.flapDetent, 0,
            std::max(0, static_cast<int>(actuators.flaps.detents.size()) - 1));
        const float flapTarget = actuators.flaps.detents.empty()
            ? 0.0f
            : actuators.flaps.detents[static_cast<std::size_t>(state.flapDetent)];
        state.flaps = MoveToward(state.flaps, flapTarget, actuators.flaps.rate * dt);

        const float throttleChange =
            (controls.throttleUp ? 1.0f : 0.0f) - (controls.throttleDown ? 1.0f : 0.0f);
        state.throttle = std::clamp(state.throttle + throttleChange * config.throttleRate * dt, 0.0f, 1.0f);
        state.brake = controls.brake;
    }
}
