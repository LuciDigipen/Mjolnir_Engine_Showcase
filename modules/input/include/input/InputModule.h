/**
 * @file InputModule.h
 * @author sumin.park
 * @brief The input map, input state and the system that reads the window.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>

#include <filesystem>

namespace mir
{
    class Window;

    class InputModule
    {
    public:
        // The InputMap is a shared project resource, so the World already holds it.
        InputModule(ModuleContext context, Window &window);

        InputModule(const InputModule &) = delete;
        InputModule &operator=(const InputModule &) = delete;
    };
}
