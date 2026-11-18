/**
 * @file FlightScriptModule.cpp
 * @author Jackey Cen
 * @brief Adds flight's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_script/FlightScriptModule.h>

#include <flight_script/FlightBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    FlightScriptModule::FlightScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterFlightBindings))
    {
    }
}
