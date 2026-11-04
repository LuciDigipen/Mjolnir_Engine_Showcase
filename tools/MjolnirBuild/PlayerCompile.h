/**
 * @file PlayerCompile.h
 * @author Quy Dong Ta
 * @brief The player step: compiles a CMake preset's player from the checkout and installs it as the template.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir::build
{
    // mjolnir_player.bat's exit codes, one per phase
    inline constexpr int kPlayerExitNoCheckout = 10;
    inline constexpr int kPlayerExitConfigure = 11;
    inline constexpr int kPlayerExitCompile = 12;
    inline constexpr int kPlayerExitInstall = 13;

    struct PlayerCompile
    {
        // The checkout, holding CMakePresets.json
        std::filesystem::path mSourceRoot;
        // e.g. windows-msvc-game-release, which builds in <checkout>/builds/<preset>
        std::string mPreset;
        // Replaced by a fresh install
        std::filesystem::path mPlayerDir;
        // The editor's player_info.json, whose options the player is configured with; empty keeps the preset's
        std::filesystem::path mEditorInfo;
    };

    std::filesystem::path PresetBuildDir(const std::filesystem::path &sourceRoot, std::string_view preset);

    // <template>.tmp, where the player is installed before it replaces the template
    std::filesystem::path PlayerStagingDir(const std::filesystem::path &playerDir);

    
    std::optional<std::string> PresetProblem(const std::filesystem::path &sourceRoot, std::string_view preset);

    
    std::optional<std::string> PlayerDirProblem(const std::filesystem::path &dir);

    // Everything the player step needs before it compiles, or nullopt
    std::optional<std::string> CheckPlayerCompile(const PlayerCompile &compile, const std::filesystem::path &outDir);

    // -DMIR_WITH_AUDIO=ON and the like, for each option editorInfo records
    std::vector<std::string> FeatureDefines(const nlohmann::json &editorInfo);

    // The cache holds every -DNAME=VALUE in defines; false when there is no cache
    bool CacheMatches(const std::filesystem::path &cacheFile, std::span<const std::string> defines);

    // No finished configure, or one with other values
    bool NeedsConfigure(const std::filesystem::path &buildDir, std::span<const std::string> defines);

    struct PlayerScript
    {
        std::filesystem::path mSourceRoot;
        std::string mPreset;
        std::filesystem::path mPrefix;
        // nullopt skips configure
        std::optional<std::vector<std::string>> mConfigure;
    };

    // The batch file that configures, compiles and installs, exiting with kPlayerExit*
    std::string PlayerScriptText(const PlayerScript &script);

    // Throws std::runtime_error. A failure leaves the previous template as it was
    void CompilePlayer(const PlayerCompile &compile);
}
