/**
 * @file ControlsSettings.h
 * @author Quy Dong Ta
 * @brief The player's look options, inverted mouse Y and inverted gamepad Y,
 *        kept in a per-user file between runs. A World resource that
 *        InputSystem applies to every action as it reads the devices.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>

namespace mir
{
    struct ControlsSettings
    {
        // the mouse's vertical movement
        bool mInvertMouseY = false;
        // the right stick's vertical axis; the left stick moves and navigates, so it is never inverted
        bool mInvertGamepadY = false;

        bool operator==(const ControlsSettings &) const = default;
    };

    // A missing or unreadable file, or a bad value, falls back to the defaults and is logged here
    ControlsSettings LoadControlsSettings(const std::filesystem::path &path);

    // Writes the "controls" section and keeps any other section the file holds. False, and logged, when it cannot
    bool SaveControlsSettings(const std::filesystem::path &path, const ControlsSettings &settings);
}
