/**
 * @file FlightPropulsion.h
 * @author Jackey Cen
 * @brief The propulsion stage: engine rpm response and thrust.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftRuntime.h>

namespace mir
{
    struct AircraftConfig;

    // Advances each engine's rpm toward its throttle target and fills runtime.propulsion.
    void StepPropulsion(const AircraftConfig &config, AircraftRuntime &runtime, float dt);
}
