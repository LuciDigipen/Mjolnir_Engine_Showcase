/**
 * @file ScriptFixedSystem.cpp
 * @author sumin.park
 * @brief Dispatches OnFixedUpdate to every started script, once per fixed step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptFixedSystem.h>

#include "RunIsolated.h"

#include <script/IScriptHost.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

namespace mir
{
    void ScriptFixedSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<ScriptRef>();
    }

    void ScriptFixedSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        mQuery->ForEach(
            [&](Entity entity, ScriptRef &ref)
            {
                if (ref.instanceRef < 0 || !ref.started)
                    return;

                RunIsolated(entity, "OnFixedUpdate", [&]
                    {
                        mHost.CallOnFixedUpdate(context.world, context.commands, entity, ref.instanceRef, context.dt);
                    });
            });
    }
}
