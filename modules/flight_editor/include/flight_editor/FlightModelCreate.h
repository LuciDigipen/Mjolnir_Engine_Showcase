/**
 * @file FlightModelCreate.h
 * @author Jackey Cen
 * @brief Validates a new flight model settings path and writes the chosen template.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight_editor/FlightModelTemplates.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    struct FlightModelCreateRequest
    {
        std::string name;
        FlightModelTemplate templateKind = FlightModelTemplate::Powered;
        // Empty at the source assets root; null when the browser folder is outside it.
        std::optional<std::string> assetsFolder;
    };

    struct FlightModelCreatePreview
    {
        // The field's value, "<folder>/<name>.aircraft.json".
        std::string assetsRelativePath;
        std::string error;

        bool Valid() const { return error.empty(); }
    };

    FlightModelCreatePreview PreviewFlightModelCreate(const FlightModelCreateRequest &request,
        const std::filesystem::path &sourceAssetsRoot);

    // Writes the template atomically, creating the folder when it is missing.
    bool CreateFlightModelFile(const FlightModelCreateRequest &request,
        const std::filesystem::path &sourceAssetsRoot, std::string &assetsRelativePath,
        std::filesystem::path &absolutePath, std::string &error);
} // namespace mir
