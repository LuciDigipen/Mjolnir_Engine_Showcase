/**
 * @file PhysicsModule.h
 * @author sumin.park
 * @brief Physics components, the Jolt world and the physics systems.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <filesystem>

namespace mir
{
    class JobExecutor;
    // PhysicsWorld and CollisionEvents are World resources, so a Stop's restore keeps them
    class PhysicsModule
    {
    public:
        // The World holds the shared PhysicsLayerTable, which sets which layers collide.
        explicit PhysicsModule(ModuleContext context, JobExecutor *jobs = nullptr);

        PhysicsModule(const PhysicsModule &) = delete;
        PhysicsModule &operator=(const PhysicsModule &) = delete;

    private:
        // last: unregisters before anything above is destroyed
        Registration mHooks;
        Registration mPlayBegin;
        Registration mPlayEnd;
    };
}
