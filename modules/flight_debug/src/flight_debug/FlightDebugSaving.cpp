/**
 * @file FlightDebugSaving.cpp
 * @author Jackey Cen
 * @brief Writes an edited aircraft configuration back to its source file.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_debug/FlightDebugSaving.h>

#include <flight/AircraftConfig.h>

#include <core/fs/FileIO.h>

#include <format>

namespace mir
{
    bool SaveAircraftConfigToSource(const AircraftConfig &config, const std::filesystem::path &sourceAssetsRoot,
        std::string_view configPath, std::string &error)
    {
        if (sourceAssetsRoot.empty() || configPath.empty())
        {
            error = "no source assets root to save into";
            return false;
        }

        const std::string text = SerializeAircraftConfig(config);
        AircraftConfig parsed;
        std::string invalid;
        if (!ParseAircraftConfig(text, parsed, invalid))
        {
            error = std::format("'{}' was not saved, the edit is invalid: {}", configPath, invalid);
            return false;
        }

        const std::filesystem::path target = sourceAssetsRoot / std::filesystem::path(configPath);
        if (!WriteTextAtomic(target, text))
        {
            error = std::format("could not write '{}'", target.string());
            return false;
        }

        error.clear();
        return true;
    }
}
