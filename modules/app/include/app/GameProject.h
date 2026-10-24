/**
 * @file GameProject.h
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/log/SetupError.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kEngineManifestFileName = "engine.json";
    inline constexpr std::string_view kGameManifestFileName = "game.json";
    inline constexpr std::string_view kProjectSettingsFileName = "project_settings.json";

    // One game directory; paths in its game.json are relative to it
    struct GameProject
    {
        // relative to the executable, e.g. "games/HelloWorld"
        std::string mDir;
        
        // game.json's "name", else the directory name
        std::string mName;

        std::string mTitle = "MjolnirEngine";

        // asset id prefix, e.g. "games/HelloWorld/assets" - not a disk path
        std::string mAssetIdRoot;

        // Source tree in a development build (asset_workspace.json), else next to the executable
        std::filesystem::path mRootDir;

        // mAssetIdRoot on disk
        std::filesystem::path mAssetsDir;

        std::filesystem::path mSceneDir;
        std::filesystem::path mProjectSettingsPath;
        std::filesystem::path mSoundBankDir;
    };

    // engine.json's defaultGame; throws SetupError if engine.json or the key is missing
    std::string ReadDefaultGame();

    // To the executable-relative form asset ids use ("games/<name>"). Throws SetupError of
    // @p notFound if outside the project root or missing
    std::filesystem::path NormalizeGameDir(const std::string &requested, ErrorCategory notFound);

    // Throws SetupError: @p notFound without a game.json, ProjectData for a malformed one
    GameProject LoadGameProject(const std::filesystem::path &gameDir,
                                ErrorCategory notFound = ErrorCategory::ProjectData);

    // NormalizeGameDir, then LoadGameProject
    GameProject ResolveGameProject(const std::string &requested, ErrorCategory notFound);

    // Creates any missing assets, scene or SoundBank directory; throws SetupError (System) if it cannot
    void EnsureGameDirectories(const GameProject &project);

    // Every games/*/ next to the executable with a game.json, sorted by directory
    std::vector<GameProject> DiscoverGameProjects();

    // Writes games/<name>/: game.json, an EmptyScene-shaped scenes/default/scene.json
    bool CreateGameProject(const std::string &name, bool useRayTracing);
}
