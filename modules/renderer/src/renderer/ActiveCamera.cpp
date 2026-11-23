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

        void KeepPreferred(World &world, Entity &slot, Entity entity)
        {
            const auto priority = [&](Entity candidate)
                {
                    const auto owner = world.EntityScene(candidate);
                    if (owner == world.PrimaryScene() && owner != kOwnedBySession) return 0;
                    return owner == kOwnedBySession ? 1 : 2;
                };
            if (slot.IsNull() || priority(entity) < priority(slot) ||
                (priority(entity) == priority(slot) && entity.mIndex < slot.mIndex))
                slot = entity;
        }
    }

    Entity FindActiveCamera(World &world)
    {
        Entity active = kNullEntity;
        world.GetOrCreateQuery<const Camera>().ForEach([&](Entity entity, const Camera &camera)
            {
                if (camera.mActive)
                    KeepPreferred(world, active, entity);
            });
        return active;
    }

    bool SetActiveCamera(World &world, Entity camera)
    {
        if (world.GetComponent<Camera>(camera) == nullptr)
            return false;

        world.GetOrCreateQuery<Camera>().ForEach([&](Entity entity, Camera &other)
            {
                if (world.EntityScene(entity) == world.EntityScene(camera)) other.mActive = entity == camera;
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
                KeepPreferred(world, lowest, entity);
                if (camera.mActive)
                    KeepPreferred(world, active, entity);
            });

        if (!active.IsNull())
            return PlayCamera{ active, true };
        return PlayCamera{ lowest, false };
    }

    void ActiveCameraSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Camera>();
    }

    void ActiveCameraSystem::OnUpdate(SystemContext &context)
    {
        struct Candidates { Entity switched; Entity kept; Entity arrived; };
        std::unordered_map<SceneInstanceId, Candidates> candidates;
        mQuery->ForEach([&](Entity entity, const Camera &camera)
            {
                if (!camera.mActive)
                    return;
                auto &scene = candidates[context.world.EntityScene(entity)];
                const auto it = mWasActive.find(PackEntity(entity));
                if (it == mWasActive.end())
                    KeepLowest(scene.arrived, entity);
                else if (it->second)
                    KeepLowest(scene.kept, entity);
                else
                    KeepLowest(scene.switched, entity);
            });

        mWasActive.clear();
        mQuery->ForEach([&](Entity entity, Camera &camera)
            {
                const auto &scene = candidates[context.world.EntityScene(entity)];
                const Entity active = !scene.switched.IsNull() ? scene.switched :
                    !scene.kept.IsNull() ? scene.kept : scene.arrived;
                if (camera.mActive && entity != active)
                    camera.mActive = false;
                mWasActive[PackEntity(entity)] = camera.mActive;
            });

        if (mPicked)
            mPicked(context.world);
    }
}
