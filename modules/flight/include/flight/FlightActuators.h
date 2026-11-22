/**
 * @file FlightActuators.h
 * @author Jackey Cen
 * @brief The actuator stage: commands and held controls to surface, flap, trim and throttle state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir
{
    struct ActuatorState;
    struct AircraftConfig;
    struct FlightCommandQueue;
    struct FlightControls;

    // Drains the queued commands in order, then advances every actuator by dt.
    void StepActuators(const AircraftConfig &config, const FlightControls &controls, FlightCommandQueue &commands,
        ActuatorState &state, float dt);
}
