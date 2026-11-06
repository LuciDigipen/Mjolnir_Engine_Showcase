/**
 * @file GameControlsOptions.h
 * @author Quy Dong Ta
 * @brief The pause menu's Controls screen, edits shared player settings and saves each change.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameshell/ControlsOptions.h>
#include <input/ControlsSettings.h>

#include <filesystem>

namespace mir
{
    class GameControlsOptions final : public IControlsOptions
    {
    public:
        GameControlsOptions(ControlsSettings &controls, const std::filesystem::path &path)
            : mControls(controls), mPath(path) {}

        bool InvertY(LookDevice device) const override;
        void SetInvertY(LookDevice device, bool invert) override;

        void RestoreDefaults() override;

    private:
        void Apply(const ControlsSettings &controls);

        ControlsSettings &mControls;
        std::filesystem::path mPath;
    };
}
