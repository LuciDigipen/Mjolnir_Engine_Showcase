/**
 * @file FlightInput.h
 * @author Jackey Cen
 * @brief The flight actions and their default keyboard and gamepad bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputMap.h>

#include <cstddef>
#include <string_view>

namespace mir
{
    inline constexpr std::string_view kFlightPitchAction = "FlightPitch";
    inline constexpr std::string_view kFlightRollAction = "FlightRoll";
    inline constexpr std::string_view kFlightYawAction = "FlightYaw";
    inline constexpr std::string_view kFlightThrottleUpAction = "FlightThrottleUp";
    inline constexpr std::string_view kFlightThrottleDownAction = "FlightThrottleDown";
    inline constexpr std::string_view kFlightBrakeAction = "FlightBrake";
    inline constexpr std::string_view kFlightFlapsUpAction = "FlightFlapsUp";
    inline constexpr std::string_view kFlightFlapsDownAction = "FlightFlapsDown";
    inline constexpr std::string_view kFlightTrimUpAction = "FlightTrimUp";
    inline constexpr std::string_view kFlightTrimDownAction = "FlightTrimDown";
    inline constexpr std::string_view kFlightCameraToggleAction = "FlightCameraToggle";
    inline constexpr std::string_view kFlightDebugToggleAction = "FlightDebugToggle";

    // Adds only the actions the map does not already have, so a rebound action is kept.
    std::size_t AddFlightDefaultActions(InputMap &map);
}
