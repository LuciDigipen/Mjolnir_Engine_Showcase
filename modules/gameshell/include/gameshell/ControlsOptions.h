/**
 * @file ControlsOptions.h
 * @author Quy Dong Ta
 * @brief What the pause menu's Controls screen reads and changes: whether the
 *        mouse and the gamepad look inverted. An interface, so gameshell
 *        builds without knowing where the setting is kept.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    enum class LookDevice : uint8_t
    {
        Mouse,
        Gamepad,
    };

    class IControlsOptions
    {
    public:
        virtual ~IControlsOptions() = default;

        virtual bool InvertY(LookDevice device) const = 0;
        virtual void SetInvertY(LookDevice device, bool invert) = 0;

        virtual void RestoreDefaults() = 0;
    };
} // namespace mir
