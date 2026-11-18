/**
 * @file FlightModule.h
 * @author Jackey Cen
 * @brief Flight components and the step gate that flies them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftConfigStore.h>
#include <flight/FlightStepGate.h>

#include <core/ecs/ModuleContext.h>

namespace mir
{
    class World;

    class FlightModule
    {
      public:
        FlightModule(ModuleContext context, AircraftConfigStore::Source source);
        ~FlightModule();

        FlightModule(const FlightModule &) = delete;
        FlightModule &operator=(const FlightModule &) = delete;

      private:
        World *mWorld = nullptr;
        FlightStepGate mGate;
    };
}
