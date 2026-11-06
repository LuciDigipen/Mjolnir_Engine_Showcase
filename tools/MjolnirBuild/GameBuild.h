/**
 * @file GameBuild.h
 * @author Quy Dong Ta
 * @brief Packages one game: a game build template, compiled first when asked, the game's cook, and the files the
 *        game reads raw.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mir::build
{
    struct BuildProgress
    {
        std::string mStep;
        // In the game build step: environment, configure, compile or install, empty otherwise
        std::string mPhase;
        // Ninja's [done/total] in the compile phase, both 0 until it prints one
        std::size_t mDone = 0;
        std::size_t mTotal = 0;
    };

    struct BuildRequest
    {
        // <checkout>/games/<name>
        std::filesystem::path mGameDir;
        std::filesystem::path mGameBuildDir;
        // Empty uses mGameBuildDir as it is; otherwise the game build step compiles this preset and installs it there
        std::string mGameBuildPreset;
        std::filesystem::path mOutDir;
        std::filesystem::path mAssetCooker;
        std::filesystem::path mTerrainTool;

        // The editor's game_build_info.json, which the template is compared with and a compiled game build takes its
        // options from; empty skips both
        std::filesystem::path mEditorInfo;

        // Told as each step starts and as the compile moves on, may be empty
        std::function<void(const BuildProgress &)> mOnProgress;
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

    // { "version": 1, "step": ..., "phase": ..., "done": ..., "total": ... }
    // total only once Ninja prints them
    std::string BuildProgressJson(const BuildProgress &progress);
}
