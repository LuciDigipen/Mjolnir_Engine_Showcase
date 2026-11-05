/**
 * @file TerrainStreamingConfiguration.cpp
 * @author sumin.park
 * @brief The editable project terrain streaming JSON and the effective settings merged from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainStreamingConfiguration.h"

#include <core/log/Log.h>

#include <utility>

namespace mir
{
    TerrainStreamingConfiguration::TerrainStreamingConfiguration(std::filesystem::path gameFile,
                                                                 std::filesystem::path localFile)
    {
        TerrainStreamingFiles files = ReadTerrainStreamingFiles(gameFile, localFile);
        mProject = std::move(files.game);
        mLastAttemptedProject = mProject;
        mLocal = std::move(files.local);
        mGameName = std::move(files.gameName);
        mLocalName = std::move(files.localName);
        mGameFileFound = files.gameFileFound;
        mLocalFileFound = files.localFileFound;
        // Startup problems are reported by the consumer that needs the settings, so nothing is logged here.
        if (files.problems.empty())
            Attempt();
        else
            mProblems = std::move(files.problems);
    }

    bool TerrainStreamingConfiguration::Refresh()
    {
        if (mProject == mLastAttemptedProject)
            return false;
        mLastAttemptedProject = mProject;
        if (Attempt())
            return true;

        std::string joined;
        for (const std::string &problem : mProblems)
            joined += (joined.empty() ? "" : "; ") + problem;
        MIR_LOG_ERROR("Terrain streaming settings not applied: {}", joined);
        return false;
    }

    bool TerrainStreamingConfiguration::Attempt()
    {
        TerrainStreamingSettingsLoad load =
            ParseTerrainStreamingSettings(mProject, mLocal ? &*mLocal : nullptr, mGameName, mLocalName);
        mProblems = std::move(load.problems);
        if (!load.settings)
            return false;
        mEffective = std::move(load.settings);
        return true;
    }
} // namespace mir
