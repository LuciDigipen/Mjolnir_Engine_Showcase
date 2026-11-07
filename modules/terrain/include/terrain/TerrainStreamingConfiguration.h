/**
 * @file TerrainStreamingConfiguration.h
 * @author sumin.park
 * @brief The editable project terrain streaming JSON and the effective settings merged from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "terrain/TerrainStreamingSettings.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    // Writers edit Project() and consumers read Effective(). The machine-local overrides come only from their
    // file, so the last-attempted snapshot covers the project layer alone.
    class TerrainStreamingConfiguration
    {
      public:
        TerrainStreamingConfiguration(std::filesystem::path gameFile, std::filesystem::path localFile);

        TerrainStreamingConfiguration(const TerrainStreamingConfiguration &) = delete;
        TerrainStreamingConfiguration &operator=(const TerrainStreamingConfiguration &) = delete;
        TerrainStreamingConfiguration(TerrainStreamingConfiguration &&) = delete;
        TerrainStreamingConfiguration &operator=(TerrainStreamingConfiguration &&) = delete;

        // Null when the game file was missing or unreadable.
        nlohmann::json &Project()
        {
            return mProject;
        }
        const nlohmann::json &Project() const
        {
            return mProject;
        }

        // Empty until a merge validates, and the last valid value after a later one fails.
        const std::optional<TerrainStreamingSettings> &Effective() const
        {
            return mEffective;
        }
        // Why the latest attempt failed, empty when it succeeded.
        const std::vector<std::string> &Problems() const
        {
            return mProblems;
        }
        bool GameFileFound() const
        {
            return mGameFileFound;
        }
        bool LocalFileFound() const
        {
            return mLocalFileFound;
        }

        // True when a changed project JSON merged and validated, so Effective() is new. A failed attempt is
        // logged once and not retried until the project JSON changes again.
        bool Refresh();

      private:
        bool Attempt();

        nlohmann::json mProject;
        nlohmann::json mLastAttemptedProject;
        std::optional<nlohmann::json> mLocal;
        std::string mGameName;
        std::string mLocalName;
        bool mGameFileFound = false;
        bool mLocalFileFound = false;
        std::optional<TerrainStreamingSettings> mEffective;
        std::vector<std::string> mProblems;
    };
} // namespace mir
