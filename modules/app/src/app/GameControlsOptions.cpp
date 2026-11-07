/**
 * @file GameControlsOptions.cpp
 * @author Quy Dong Ta
 * @brief The pause menu's Controls screen, edits shared player settings and saves each change.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameControlsOptions.h>

namespace mir
{
    bool GameControlsOptions::InvertY(LookDevice device) const
    {
        const ControlsSettings &controls = mControls;
        return device == LookDevice::Mouse ? controls.mInvertMouseY : controls.mInvertGamepadY;
    }

    void GameControlsOptions::SetInvertY(LookDevice device, bool invert)
    {
        ControlsSettings controls = mControls;
        (device == LookDevice::Mouse ? controls.mInvertMouseY : controls.mInvertGamepadY) = invert;
        Apply(controls);
    }

    void GameControlsOptions::Apply(const ControlsSettings &controls)
    {
        if (mControls == controls)
            return;
        mControls = controls;
        if (!mPath.empty())
            SaveControlsSettings(mPath, mControls);
    }

    void GameControlsOptions::RestoreDefaults()
    {
        Apply(ControlsSettings{});
    }
}
