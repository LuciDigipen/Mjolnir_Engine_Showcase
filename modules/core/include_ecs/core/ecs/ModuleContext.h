/**
 * @file ModuleContext.h
 * @author sumin.park
 * @brief What App hands every module's constructor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir
{
    class World;
    class SystemScheduler;

    // Only the ECS: a module gets the other modules it needs and its settings as
    // explicit constructor parameters
    struct ModuleContext
    {
        World &world;
        SystemScheduler &scheduler;
    };
}
