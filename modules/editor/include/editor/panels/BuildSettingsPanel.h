/**
 * @file BuildSettingsPanel.h
 * @author Quy Dong Ta
 * @brief The "Build Settings" window: profile, game build template, output folder, Run after build, and Build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/BuildJob.h>
#include <editor/BuildSettings.h>
#include <editor/EditorPreferences.h>

#include <filesystem>
#include <string>
#include <utility>

namespace mir
{
    enum class BuildCommand
    {
        None,
        Build,
        SaveThenBuild,
        DeclineSave,
        Cancel,
    };

    struct BuildAction
    {
        BuildCommand mCommand = BuildCommand::None;
        // what Build packages with, resolved from the preferences
        std::filesystem::path mGameBuildDir;
        std::filesystem::path mOutDir;
    };

    struct BuildSettingsRequest
    {
        // for the editor to save the preferences
        bool mSavePreferences = false;
        BuildAction mBuild;
    };

    class BuildSettingsPanel
    {
    public:
        BuildSettingsRequest Draw(EditorPreferences &preferences, const BuildGame &game, const BuildJobView &build);

        void SetStatus(std::string message) { mStatusMessage = std::move(message); }

        // the profile is read again when the window opens
        void OnHidden() { mLoaded = false; }

    private:
        void Load(EditorPreferences &preferences, const BuildGame &game);
        void DrawTemplateStatus(const std::filesystem::path &dir, const BuildGame &game);
        void DrawBuild(const BuildGame &game, const BuildJobView &build, BuildAction &action,
            const std::filesystem::path &gameBuildDir, const std::filesystem::path &outDir);
        void DrawSavePrompt(const BuildJobView &build, BuildAction &action, const std::filesystem::path &gameBuildDir,
            const std::filesystem::path &outDir);
        void DrawResult(const BuildResult &result, const BuildGame &game);

        bool mLoaded = false;
        BuildProfile mProfile = BuildProfile::Release;
        std::string mStatusMessage;
        char mTemplatePath[512]{};
        char mOutputPath[512]{};

        // checked again when the folder changes and every few seconds, so an install shows up
        std::filesystem::path mCheckedDir;
        BuildProfile mCheckedProfile = BuildProfile::Release;
        double mCheckedAt = 0.0;
        GameBuildTemplateCheck mCheck;
    };
}
