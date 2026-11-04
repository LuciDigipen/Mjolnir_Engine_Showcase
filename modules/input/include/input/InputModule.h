/**
 * @file InputModule.h
 * @author sumin.park
 * @brief The input map, input state, the player's look options and the system that reads the window.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/ControlsSettings.h>

#include <core/ecs/ModuleContext.h>

#include <filesystem>

namespace mir
{
    class Window;
    class World;

    class InputModule
    {
    public:
        // the map comes from project settings, with the engine's default actions added; the look options come
        // from @p userSettingsPath, and an empty path neither reads nor saves them
        InputModule(ModuleContext context, Window &window, const std::filesystem::path &projectSettingsPath,
            const std::filesystem::path &userSettingsPath = {});

        InputModule(const InputModule &) = delete;
        InputModule &operator=(const InputModule &) = delete;

        const ControlsSettings &Controls() const;

        // applied from the next InputSystem update, and saved to the user's settings at once
        void SetControls(const ControlsSettings &controls);

    private:
        World &mWorld;
        std::filesystem::path mUserSettingsPath;
    };
}
