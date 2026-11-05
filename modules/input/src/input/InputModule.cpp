/**
 * @file InputModule.cpp
 * @author sumin.park
 * @brief The input map, input state and the system that reads the window.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/InputModule.h>

#include <input/InputState.h>
#include <input/InputSystem.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

namespace mir
{
    InputModule::InputModule(ModuleContext context, Window &window)
    {
        context.world.EmplaceResource<InputState>();

        // before ScriptSystem, which reads this frame's input
        context.scheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kEarly}, window);
    }
}
