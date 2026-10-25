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

namespace mir
{
    // PhysicsWorld and CollisionEvents are World resources, so a Stop's restore keeps them
    class PhysicsModule
    {
    public:
        explicit PhysicsModule(ModuleContext context);

        PhysicsModule(const PhysicsModule &) = delete;
        PhysicsModule &operator=(const PhysicsModule &) = delete;

    private:
        // last: unregisters before anything above is destroyed
        Registration mHooks;
        Registration mPlayBegin;
        Registration mPlayEnd;
    };
}
