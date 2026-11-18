/**
 * @file ComponentRegistration.cpp
 * @author Jackey Cen
 * @brief Registers every component the flight module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/ComponentRegistration.h>

#include <flight/components/Aircraft.h>

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterFlightComponents()
    {
        ComponentRegistry::Instance().Register<Aircraft>(kAircraftFields, kAircraftTransient);
    }
}
