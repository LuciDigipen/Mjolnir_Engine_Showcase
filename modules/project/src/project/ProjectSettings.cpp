/**
 * @file ProjectSettings.cpp
 * @author sumin.park
 * @brief Project_settings.json validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <project/ProjectSettings.h>

#include <core/log/SetupError.h>
#include <serial/JsonFile.h>

#include <string>
#include <system_error>

namespace mir
{
    std::optional<nlohmann::json> ReadProjectSettings(const std::filesystem::path &path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return std::nullopt;

        std::string error;
        std::optional<nlohmann::json> settings = ReadJsonFile(path, &error);
        if (!settings || !settings->is_object())
        {
            throw SetupError(ErrorCategory::ProjectData, "project_settings.json is not a valid JSON object", path,
                settings ? "the top level is not an object" : error,
                "Fix the JSON in " + path.string() + ", or restore it from git.");
        }
        return settings;
    }
} // namespace mir
