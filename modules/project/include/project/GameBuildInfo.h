/**
 * @file GameBuildInfo.h
 * @author Quy Dong Ta
 * @brief game_build_info.json, which says how an engine build was made: checking a game build
 *        template and comparing it with the editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
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
    };

    inline constexpr GameBuildFeature kGameBuildFeatures[] = {
        { "largeWorld", "MIR_LARGE_WORLD" },
        { "audio", "MIR_WITH_AUDIO" },
        { "dlss", "MIR_WITH_DLSS" },
        { "rtxgi", "MIR_WITH_RTXGI" },
    };

    // nullopt when @p info has no such boolean
    std::optional<bool> GameBuildInfoFlag(const nlohmann::json &info, std::string_view key);

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

    // Where a game build template was built differently from the editor; empty when they match
    std::vector<GameBuildInfoMismatch> CompareGameBuildInfo(const nlohmann::json &gameBuild, const nlohmann::json &editor);
}
