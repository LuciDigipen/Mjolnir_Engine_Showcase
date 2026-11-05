/**
 * @file GameBuildCompile.h
 * @author Quy Dong Ta
 * @brief The game build step: compiles a CMake preset's game build from the checkout and installs it as the template.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VisualStudio.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::build
{
    // mjolnir_game_build.bat's exit codes, one per phase
    inline constexpr int kGameBuildExitNoCheckout = 10;
    inline constexpr int kGameBuildExitConfigure = 11;
    inline constexpr int kGameBuildExitCompile = 12;
    inline constexpr int kGameBuildExitInstall = 13;
    inline constexpr int kGameBuildExitVisualStudio = 14;
    inline constexpr int kGameBuildExitNoCompiler = 15;
    inline constexpr int kGameBuildExitNoCMake = 16;

    struct GameBuildCompile
    {
        // The checkout, holding CMakePresets.json
        std::filesystem::path mSourceRoot;
        // e.g. windows-msvc-game-release, which builds in its binaryDir
        std::string mPreset;
        // Replaced by a fresh install
        std::filesystem::path mGameBuildDir;
        // The editor's game_build_info.json, whose options the game build is configured with; empty keeps the preset's
        std::filesystem::path mEditorInfo;
    };

    // The preset's binaryDir, or nullopt when it sets none or uses a macro other than ${sourceDir} and the like
    std::optional<std::filesystem::path> PresetBuildDir(const std::filesystem::path &sourceRoot, std::string_view preset);

    // <template>.tmp, where the game build is installed before it replaces the template
    std::filesystem::path GameBuildStagingDir(const std::filesystem::path &gameBuildDir);

    // <template>.old, where the previous template waits until the new one is in place
    std::filesystem::path GameBuildPreviousDir(const std::filesystem::path &gameBuildDir);

    
    std::optional<std::string> PresetProblem(const std::filesystem::path &sourceRoot, std::string_view preset);

    
    std::optional<std::string> GameBuildDirProblem(const std::filesystem::path &dir);

    // Everything the game build step needs before it compiles, or nullopt
    std::optional<std::string> CheckGameBuildCompile(const GameBuildCompile &compile, const std::filesystem::path &outDir);

    // -DMIR_WITH_AUDIO=ON and the like, for each option editorInfo records
    std::vector<std::string> FeatureDefines(const nlohmann::json &editorInfo);

    struct GameBuildScript
    {
        std::filesystem::path mSourceRoot;
        std::string mPreset;
        std::filesystem::path mBuildDir;
        std::filesystem::path mPrefix;
        // Configure runs every compile, so a changed preset is never packaged with the old settings
        std::vector<std::string> mDefines;
        // Loaded first, so the compile needs no Developer Command Prompt
        VisualStudio mVisualStudio;
    };

    // The batch file that configures, compiles and installs, exiting with kGameBuildExit*
    std::string GameBuildScriptText(const GameBuildScript &script);

    // Throws std::runtime_error. A failure leaves the previous template as it was
    void CompileGameBuild(const GameBuildCompile &compile);
}
