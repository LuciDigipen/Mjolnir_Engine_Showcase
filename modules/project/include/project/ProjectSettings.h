/**
 * @file ProjectSettings.h
 * @author sumin.park
 * @brief Project_settings.json validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace mir
{
    // Sections that describe one machine, such as "ide" with the IDE's install path. A packaged
    // game's copy leaves them out, so add any new machine-specific section here
    inline constexpr std::string_view kMachineSettingsSections[] = { "ide" };

    // Check ProjectSetting.json if it's valid, nullopt when the file does not exist
    std::optional<nlohmann::json> ReadProjectSettings(const std::filesystem::path &path);
}// namespace mir
