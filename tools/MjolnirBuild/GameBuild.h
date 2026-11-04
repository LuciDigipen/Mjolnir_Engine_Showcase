/**
 * @file GameBuild.h
 * @author Quy Dong Ta
 * @brief Packages one game: a player template, compiled first when asked, the game's cook, and the files the
 *        game reads raw.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mir::build
{
    struct BuildRequest
    {
        // <checkout>/games/<name>
        std::filesystem::path mGameDir;
        std::filesystem::path mPlayerDir;
        // Empty uses mPlayerDir as it is; otherwise the player step compiles this preset and installs it there
        std::string mPlayerPreset;
        std::filesystem::path mOutDir;
        std::filesystem::path mAssetCooker;
        std::filesystem::path mTerrainTool;

        // The editor's player_info.json, which the template is compared with and a compiled player takes its
        // options from; empty skips both
        std::filesystem::path mEditorInfo;
    };

    enum class StepStatus
    {
        Succeeded,
        Skipped,
        Failed,
    };

    struct BuildStep
    {
        std::string mName;
        StepStatus mStatus = StepStatus::Failed;
        double mSeconds = 0.0;
    };

    struct BuildReport
    {
        bool mOk = false;
        std::string mGame;
        std::filesystem::path mOutDir;

        // <out>/<game.json name>.exe, set once the build is in place
        std::filesystem::path mExecutable;
        std::vector<BuildStep> mSteps;
        std::vector<std::string> mWarnings;
        std::string mError;
        std::size_t mAssetCount = 0;
        std::uintmax_t mBytes = 0;
    };

    // Never throws. A failure leaves a previous build in mOutDir as it was
    BuildReport RunGameBuild(const BuildRequest &request);

    std::string BuildReportJson(const BuildReport &report);
}
