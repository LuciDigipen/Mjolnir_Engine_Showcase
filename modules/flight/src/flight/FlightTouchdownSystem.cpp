/**
 * @file FlightTouchdownSystem.cpp
 * @author Jackey Cen
 * @brief Moves each aircraft's pending touchdowns into the list scripts read.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightTouchdownSystem.h>

#include <flight/components/Aircraft.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

namespace mir
{
    void FlightTouchdownSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Aircraft>();
    }

    void FlightTouchdownSystem::OnUpdate(SystemContext &context)
    {
        // Held while paused so scripts still see the list on resume.
        if (IsPaused(context.world))
            return;

        const bool simulating = IsSimulating(context.world);
        mQuery->ForEach(
            [simulating](Entity, Aircraft &aircraft)
            {
                AircraftRuntime &runtime = aircraft.runtime;
                if (simulating)
                    runtime.touchdowns = runtime.pendingTouchdowns;
                else
                    runtime.touchdowns.Clear();

                runtime.pendingTouchdowns.Clear();
                runtime.gear.stepTouchdowns.Clear();
            });
    }
}
