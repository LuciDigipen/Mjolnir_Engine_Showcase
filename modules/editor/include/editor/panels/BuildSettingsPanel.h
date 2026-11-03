/**
 * @file BuildSettingsPanel.h
 * @author Quy Dong Ta
 * @brief The "Build Settings" window: profile, player template, output folder and Run after build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/BuildSettings.h>
#include <editor/EditorPreferences.h>

#include <filesystem>
#include <string>
#include <utility>

namespace mir
{
    class BuildSettingsPanel
    {
    public:
        // true when @p preferences changed, for the editor to save them
        bool Draw(EditorPreferences &preferences, const BuildGame &game);

        void SetStatus(std::string message) { mStatusMessage = std::move(message); }

        // the profile is read again when the window opens
        void OnHidden() { mLoaded = false; }

    private:
        void Load(EditorPreferences &preferences, const BuildGame &game);
        void DrawTemplateStatus(const std::filesystem::path &dir, const BuildGame &game);

        bool mLoaded = false;
        BuildProfile mProfile = BuildProfile::Release;
        std::string mStatusMessage;
        char mTemplatePath[512]{};
        char mOutputPath[512]{};

        // checked again when the folder changes and every few seconds, so an install shows up
        std::filesystem::path mCheckedDir;
        BuildProfile mCheckedProfile = BuildProfile::Release;
        double mCheckedAt = 0.0;
        PlayerTemplateCheck mCheck;
    };
}
