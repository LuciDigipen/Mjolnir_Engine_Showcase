/**
 * @file ScriptSystem.cpp
 * @author Rahul Nair
 * @brief Dispatches OnStart/OnUpdate to every entity with a ScriptRef, once
 *        per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <script/ScriptSystem.h>

#include "RunIsolated.h"

#include <script/IScriptHost.h>
#include <script/ScriptResolveSystem.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/TypeId.h>
#include <core/log/Log.h>

#include <exception>

namespace mir
{
    void ScriptSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<ScriptRef>();
        if (ComponentFieldListeners *listeners = context.world.TryResource<ComponentFieldListeners>())
            listeners->AddBeforeChange(&ScriptSystem::OnBeforeFieldChange, this);
    }

    void ScriptSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        mQuery->ForEach(
            [&](Entity entity, ScriptRef &ref)
            {
                if (ref.instanceRef < 0)
                    return;

                if (!ref.started)
                {
                    RunIsolated(entity, "OnStart", [&]
                        {
                            mHost.CallOnStart(context.world, context.commands, entity, ref.instanceRef);
                        });
                    ref.started = true;
                }

                RunIsolated(entity, "OnUpdate", [&]
                    {
                        mHost.CallOnUpdate(context.world, context.commands, entity, ref.instanceRef, context.dt);
                    });
            });
    }

    void ScriptSystem::StopAllScripts(SystemContext &context)
    {
        mQuery->ForEach(
            [&](Entity entity, ScriptRef &ref)
            {
                if (ref.instanceRef < 0 || !ref.started)
                    return;

                RunIsolated(entity, "OnStop", [&]
                    {
                        mHost.CallOnStop(context.world, context.commands, entity, ref.instanceRef);
                    });

                // Reset so a later Play session calls OnStart again.
                ref.started = false;
            });
    }

    void ScriptSystem::StopSceneScripts(SystemContext &context)
    {
        if (mQuery == nullptr) return;
        mQuery->ForEach([&](Entity entity, ScriptRef &ref)
            {
                if (context.world.EntityScene(entity) != context.sceneInstance ||
                    ref.instanceRef < 0 || !ref.started)
                    return;
                RunIsolated(entity, "OnStop", [&]
                    { mHost.CallOnStop(context.world, context.commands, entity, ref.instanceRef); });
                ref.started = false;
            });
    }

    void ScriptSystem::OnStop(SystemContext &context)
    {
        StopAllScripts(context);
        if (ComponentFieldListeners *listeners = context.world.TryResource<ComponentFieldListeners>())
            listeners->RemoveBeforeChange(&ScriptSystem::OnBeforeFieldChange, this);
    }

    void ScriptSystem::OnBeforeFieldChange(World &world, std::span<const ComponentFieldChange> changes, void *user)
    {
        auto *self = static_cast<ScriptSystem *>(user);
        const TypeId scriptType = TypeIdOf<ScriptRef>();

        for (const ComponentFieldChange &change : changes)
        {
            if (!(change.mType == scriptType))
                continue;

            for (const std::string &field : change.mFields)
            {
                if (field != "scriptName")
                    continue;
                self->ReleaseScript(world, change.mEntity);
                break;
            }
        }
    }

    void ScriptSystem::ReleaseScript(World &world, Entity entity)
    {
        ScriptRef *ref = world.GetComponent<ScriptRef>(entity);
        if (ref == nullptr)
            return;

        ReleaseScriptInstance(mHost, *ref);
    }
}
