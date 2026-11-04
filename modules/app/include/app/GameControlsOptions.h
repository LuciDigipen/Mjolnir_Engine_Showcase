/**
 * @file GameControlsOptions.h
 * @author Quy Dong Ta
 * @brief The pause menu's Controls screen, through the input module, which applies and saves each change.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameshell/ControlsOptions.h>
#include <input/InputModule.h>

namespace mir
{
    class GameControlsOptions final : public IControlsOptions
    {
    public:
        explicit GameControlsOptions(InputModule &input) : mInput(input) {}

        bool InvertY(LookDevice device) const override;
        void SetInvertY(LookDevice device, bool invert) override;

        void RestoreDefaults() override;

    private:
        InputModule &mInput;
    };
}
