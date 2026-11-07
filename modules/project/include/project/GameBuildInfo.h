/**
 * @file GameBuildInfo.h
 * @author Quy Dong Ta
 * @brief game_build_info.json, which says how an engine build was made: checking a game build
 *        template and comparing it with the editor and the options the game chose.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kGameBuildExecutableFileName = "MjolnirEngine.exe";
    inline constexpr std::string_view kGameBuildInfoFileName = "game_build_info.json";

    struct GameBuildFeature
    {
        std::string_view mKey;
        // the CMake option that sets it
        std::string_view mOption;
        // Build Settings' checkbox, and its tooltip: what it builds in, and what turning it on needs
        std::string_view mLabel;
        std::string_view mAbout;
    };

    inline constexpr GameBuildFeature kGameBuildFeatures[] = {
        { "largeWorld", "MIR_LARGE_WORLD", "Large world",
            "Double-precision world positions and Jolt. Changing it reinstalls Jolt through vcpkg." },
        { "audio", "MIR_WITH_AUDIO", "Audio",
            "Wwise audio and its Lua bindings. Turning it on needs the Wwise SDK on this machine (WWISESDK)." },
        { "dlss", "MIR_WITH_DLSS", "DLSS",
            "NVIDIA DLSS Ray Reconstruction. Turning it on downloads the NGX SDK when the game build configures." },
        { "rtxgi", "MIR_WITH_RTXGI", "RTXGI",
            "RTXGI DDGI probes. Turning it on downloads the RTXGI SDK and needs DXC from the Vulkan SDK." },
    };

    inline constexpr std::string_view kGameBuildOptionsKey = "gameBuildOptions";

    
    struct GameBuildChoices
    {
        std::map<std::string, bool, std::less<>> mOn;
        // entries that are not a feature set to true or false, which are left out
        std::vector<std::string> mProblems;
    };

    GameBuildChoices ReadGameBuildChoices(const nlohmann::json &projectSettings);
    // none when the file is missing or unreadable
    GameBuildChoices ReadGameBuildChoices(const std::filesystem::path &projectSettings);

    // nullopt when @p info has no such boolean
    std::optional<bool> GameBuildInfoFlag(const nlohmann::json &info, std::string_view key);

   
    std::optional<bool> WantedGameBuildFlag(
        const GameBuildFeature &feature, const GameBuildChoices &choices, const nlohmann::json &editor);

    // e.g. "0.1.0 at commit 8ae1f3718b"
    std::string GameBuildInfoVersion(const nlohmann::json &info);

    // Why @p dir is not a game build template, or nullopt, with its game_build_info.json put in @p info
    std::optional<std::string> GameBuildTemplateProblem(const std::filesystem::path &dir, nlohmann::json *info = nullptr);

    struct GameBuildInfoMismatch
    {
        // "version", or the feature's key such as "audio"
        std::string mKey;
        std::string mMessage;
    };

    // Where a game build template was built differently from the game's choices, or for a feature it did not choose,
    // from the editor; empty when they match. An @p editor that is not an object is left out
    std::vector<GameBuildInfoMismatch> CompareGameBuildInfo(
        const nlohmann::json &gameBuild, const nlohmann::json &editor, const GameBuildChoices &choices = {});
}
