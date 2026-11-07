/**
 * @file BuildSettingsPanel.h
 * @author Quy Dong Ta
 * @brief The "Build Settings" window: profile, build options, game build template and whether Build compiles it,
 *        output folder, Run after build, and Build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/BuildJob.h>
#include <editor/BuildSettings.h>
#include <editor/EditorPreferences.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
        // set when Compile the game build is on
        std::string mGameBuildPreset;
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
        // nullopt follows the editor again
        void SaveChoice(const BuildGame &game, std::string_view key, std::optional<bool> on);
        void DrawOptions(const BuildGame &game, bool compile);
        void DrawTemplateStatus(const std::filesystem::path &dir, const BuildGame &game, bool compile);
        // @p target is the Build to request, without its command
        void DrawBuild(const BuildGame &game, const BuildJobView &build, BuildAction &action, const BuildAction &target);
        void DrawSavePrompt(const BuildJobView &build, BuildAction &action, const BuildAction &target);
        void DrawResult(const BuildResult &result, const BuildGame &game);

        bool mLoaded = false;
        BuildProfile mProfile = BuildProfile::Release;
        std::string mStatusMessage;
        char mTemplatePath[512]{};
        char mOutputPath[512]{};

        GameBuildChoices mChoices;
        double mChoicesReadAt = 0.0;
        // the editor's game_build_info.json, null when it cannot be read
        nlohmann::json mEditorInfo;
        bool mHasSoundBanks = false;

        // checked again when the folder changes and every few seconds, so an install shows up
        std::filesystem::path mCheckedDir;
        BuildProfile mCheckedProfile = BuildProfile::Release;
        double mCheckedAt = 0.0;
        GameBuildTemplateCheck mCheck;
        // missing or empty, so a compile has nothing to replace
        bool mTemplateMissing = false;
    };
}
