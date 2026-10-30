/**
 * @file ActiveCamera.cpp
 * @author Quy Dong Ta
 * @brief Which Camera Play renders through: the one flagged active, kept to
 *        one by ActiveCameraSystem, with a defined fallback when none is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/ActiveCamera.h"

#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>

namespace mir
{
    namespace
    {
        void KeepLowest(Entity &slot, Entity entity)
        {
            if (slot.IsNull() || entity.mIndex < slot.mIndex)
                slot = entity;
        }
    }

    Entity FindActiveCamera(World &world)
    {
        Entity active = kNullEntity;
        world.GetOrCreateQuery<const Camera>().ForEach([&](Entity entity, const Camera &camera)
            {
                if (camera.mActive)
                    KeepLowest(active, entity);
            });
        return active;
    }

    bool SetActiveCamera(World &world, Entity camera)
    {
        if (world.GetComponent<Camera>(camera) == nullptr)
            return false;

        world.GetOrCreateQuery<Camera>().ForEach([&](Entity entity, Camera &other)
            {
                other.mActive = entity == camera;
            });
        return true;
    }

    PlayCamera ResolvePlayCamera(World &world)
    {
        Entity active = kNullEntity;
        Entity lowest = kNullEntity;
        world.GetOrCreateQuery<const WorldTransform, const Camera>().ForEach(
            [&](Entity entity, const WorldTransform &, const Camera &camera)
            {
                KeepLowest(lowest, entity);
                if (camera.mActive)
                    KeepLowest(active, entity);
            });

        if (!active.IsNull())
            return PlayCamera{ active, true };
        return PlayCamera{ lowest, false };
    }

    void ActiveCameraSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Camera>();
    }

    void ActiveCameraSystem::OnUpdate(SystemContext &)
    {
        Entity switched = kNullEntity;
        Entity kept = kNullEntity;
        Entity arrived = kNullEntity;
        mQuery->ForEach([&](Entity entity, const Camera &camera)
            {
                if (!camera.mActive)
                    return;
                const auto it = mWasActive.find(PackEntity(entity));
                if (it == mWasActive.end())
                    KeepLowest(arrived, entity);
                else if (it->second)
                    KeepLowest(kept, entity);
                else
                    KeepLowest(switched, entity);
            });

        const Entity active = !switched.IsNull() ? switched : !kept.IsNull() ? kept : arrived;

        mWasActive.clear();
        mQuery->ForEach([&](Entity entity, Camera &camera)
            {
                if (camera.mActive && entity != active)
                    camera.mActive = false;
                mWasActive[PackEntity(entity)] = camera.mActive;
            });
    }
}
