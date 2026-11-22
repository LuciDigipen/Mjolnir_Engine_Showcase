/**
 * @file FlightDebugSaving.h
 * @author Jackey Cen
 * @brief Writes an edited aircraft configuration back to its source file.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace mir
{
    struct AircraftConfig;

    // Runs with the absolute path of a source file Save to source just wrote.
    using SavedSourceCallback = std::function<void(const std::filesystem::path &)>;

    // Rewrites <sourceAssetsRoot>/<configPath> through a temp file in the same directory.
    bool SaveAircraftConfigToSource(const AircraftConfig &config, const std::filesystem::path &sourceAssetsRoot,
        std::string_view configPath, std::string &error, const SavedSourceCallback &onSaved = {});
}
