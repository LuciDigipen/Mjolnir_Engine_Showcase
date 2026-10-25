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
        if (EntityChangeListeners *listeners = context.world.TryResource<EntityChangeListeners>())
            listeners->AddBeforeRemove(&ScriptSystem::OnBeforeRemove, this);
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

    void ScriptSystem::OnStop(SystemContext &context)
    {
        StopAllScripts(context);
        if (EntityChangeListeners *listeners = context.world.TryResource<EntityChangeListeners>())
            listeners->RemoveBeforeRemove(&ScriptSystem::OnBeforeRemove, this);
    }

    void ScriptSystem::OnBeforeRemove(World &world, const EntityChangeSet &changes, void *user)
    {
        auto *self = static_cast<ScriptSystem *>(user);

        for (Entity entity : changes.mRemovedEntities)
            self->ReleaseScript(world, entity);

        const TypeId scriptType = TypeIdOf<ScriptRef>();
        for (const EntityComponentRef &component : changes.mRemovedComponents)
        {
            if (component.mType == scriptType)
                self->ReleaseScript(world, component.mEntity);
        }

        for (const EntityComponentChange &change : changes.mChangedComponents)
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
