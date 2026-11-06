/**
 * @file BuildSettings.h
 * @author Quy Dong Ta
 * @brief File > Build Settings: the profile shared in project_settings.json, and the
 *        per-machine template and output paths resolved against the checkout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/EditorPreferences.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class BuildProfile
    {
        Development,
        Release,
    };

    // "development" or "release", as project_settings.json's build section names it
    std::string_view BuildProfileName(BuildProfile profile);

    
    std::string_view GameBuildPreset(BuildProfile profile);

    // project_settings.json's build.profile, Release when missing or unknown
    BuildProfile ReadBuildProfile(const std::filesystem::path &projectSettings);

    // writes build.profile only, so the checked-in file never gets a path
    bool WriteBuildProfile(const std::filesystem::path &projectSettings, BuildProfile profile);

    // The game a build packages and the checkout it is in
    struct BuildGame
    {
        std::filesystem::path mSourceRoot;
        // absolute, e.g. <checkout>/games/HelloWorld
        std::filesystem::path mGameDir;
        // game.json's name, which names the exe
        std::string mName;
        std::filesystem::path mProjectSettingsPath;
        // the editor's game_build_info.json, which a template is compared with
        std::filesystem::path mEditorInfo;
    };

    // <checkout>/builds/game-build/debug or release
    std::filesystem::path DefaultGameBuildTemplate(const std::filesystem::path &sourceRoot, BuildProfile profile);
    // <checkout>/builds/game-build-out/<game folder>
    std::filesystem::path DefaultBuildOutput(const BuildGame &game);

    
    std::string CheckoutPath(const std::filesystem::path &path, const std::filesystem::path &sourceRoot);

    // the key of buildOutputs in EditorPreferences
    std::string BuildOutputKey(const BuildGame &game);

    // The paths from @p preferences: empty is the default, and a relative path is from the checkout
    std::filesystem::path GameBuildTemplateDir(
        const EditorPreferences &preferences, const BuildGame &game, BuildProfile profile);
    std::filesystem::path BuildOutputDir(const EditorPreferences &preferences, const BuildGame &game);

    struct GameBuildTemplateCheck
    {

        std::string mProblem;

        nlohmann::json mInfo;

        std::vector<std::string> mWarnings;
    };

    GameBuildTemplateCheck CheckGameBuildTemplate(
        const std::filesystem::path &templateDir, BuildProfile profile, const std::filesystem::path &editorInfo);
}
