/**
 * @file FlightScriptModule.h
 * @author Jackey Cen
 * @brief Adds flight's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    class FlightScriptModule
    {
      public:
        explicit FlightScriptModule(ScriptModule &scripts);

        FlightScriptModule(const FlightScriptModule &) = delete;
        FlightScriptModule &operator=(const FlightScriptModule &) = delete;

      private:
        Registration mBindings;
    };
}
