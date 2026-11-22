/**
 * @file FlightBindings.h
 * @author Jackey Cen
 * @brief Extends the World usertype with aircraft telemetry, wind, reset, touchdown and control calls.
 *
 * Must run after RegisterWorldBindings - it extends that World usertype rather than creating a new one. Every
 * call looks the Aircraft component up again, so nothing here caches one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterFlightBindings(sol::state &lua);
}
