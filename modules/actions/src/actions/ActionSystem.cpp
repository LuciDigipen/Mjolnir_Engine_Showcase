/**
 * @file ActionSystem.cpp
 * @author Quy Dong Ta
 * @brief Advances the Game clock's actions while the simulation runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/ActionSystem.h>

#include <actions/ActionScheduler.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

namespace mir
{
    void ActionSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        if (ActionScheduler *actions = context.world.TryResource<ActionScheduler>())
            actions->Tick(context.world, ActionClock::Game, context.dt);
    }
}
