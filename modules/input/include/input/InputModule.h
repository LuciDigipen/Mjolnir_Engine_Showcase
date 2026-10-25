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
        // the map comes from project settings, with the engine's default actions added
        InputModule(ModuleContext context, Window &window, const std::filesystem::path &projectSettingsPath);

        InputModule(const InputModule &) = delete;
        InputModule &operator=(const InputModule &) = delete;
    };
}
