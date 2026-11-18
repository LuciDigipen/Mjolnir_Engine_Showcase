/**
 * @file FlightDebugRows.h
 * @author Jackey Cen
 * @brief The label and value rows the Flight Debug window draws, built without ImGui.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/components/Aircraft.h>

#include <string>
#include <vector>

namespace mir
{
    struct FlightDebugRow
    {
        std::string label;
        std::string value;
        bool warning = false;
    };

    // True when the config carries the tier's aerodynamic profile.
    bool FlightDebugTierAvailable(const AircraftConfig &config, AerodynamicTier tier);

    // Writes the tier and reports whether the config can fly it.
    bool SetFlightDebugTier(Aircraft &aircraft, const AircraftConfig *config, AerodynamicTier tier);

    // Editor builds show the window only over a Play world; a game build shows it whenever it is on.
    bool FlightDebugWindowVisible(bool editorBuild, bool playWorldActive, bool toggledOn);

    float FlightDebugDegrees(float radians);

    std::vector<FlightDebugRow> BuildFlightDebugConfigRows(const AircraftConfig &config);
    std::vector<FlightDebugRow> BuildFlightDebugTelemetryRows(const AircraftRuntime &runtime);
    std::vector<FlightDebugRow> BuildFlightDebugForceRows(const AircraftRuntime &runtime);
}
