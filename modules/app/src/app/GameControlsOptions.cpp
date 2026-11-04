/**
 * @file GameControlsOptions.cpp
 * @author Quy Dong Ta
 * @brief The pause menu's Controls screen, through the input module, which applies and saves each change.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameControlsOptions.h>

namespace mir
{
    bool GameControlsOptions::InvertY(LookDevice device) const
    {
        const ControlsSettings &controls = mInput.Controls();
        return device == LookDevice::Mouse ? controls.mInvertMouseY : controls.mInvertGamepadY;
    }

    void GameControlsOptions::SetInvertY(LookDevice device, bool invert)
    {
        ControlsSettings controls = mInput.Controls();
        (device == LookDevice::Mouse ? controls.mInvertMouseY : controls.mInvertGamepadY) = invert;
        mInput.SetControls(controls);
    }

    void GameControlsOptions::RestoreDefaults()
    {
        mInput.SetControls(ControlsSettings{});
    }
}
